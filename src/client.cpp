// The viewer-side protocol client.
#include "broremote/client.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace broremote {

struct Client::Impl {
    std::unique_ptr<Stream> stream;
    ClientHandlers h;
    WelcomeMsg welcome;
    wire::MessageSplitter splitter;
    std::mutex write_m;
    std::atomic<bool> open{true};
    std::thread reader;
    std::string fatal;  // the server's last Error, the likely reason it closed

    bool send(const std::string& msg) {
        if (!open) return false;
        std::lock_guard<std::mutex> lk(write_m);
        return stream->write(msg);
    }

    // Dispatch one message; false ends the connection with *why.
    bool dispatch(const wire::MessageSplitter::Message& m, std::string* why) {
        switch (MsgType(m.type)) {
            case MsgType::StreamConfig: {
                StreamConfig sc;
                if (!sc.decode(m.payload)) return bad("StreamConfig", why);
                if (h.on_config) h.on_config(sc);
                return true;
            }
            case MsgType::Video: {
                VideoPacket v;
                if (!v.decode(m.payload)) return bad("Video", why);
                if (h.on_video) h.on_video(v);
                return true;
            }
            case MsgType::Cursor: {
                CursorMsg c;
                if (!c.decode(m.payload)) return bad("Cursor", why);
                if (h.on_cursor) h.on_cursor(c.state);
                return true;
            }
            case MsgType::Error: {
                ErrorMsg e;
                if (!e.decode(m.payload)) return bad("Error", why);
                if (e.code != ErrorCode::UnknownMessage) fatal = std::string(error_code_name(e.code)) + ": " + e.message;
                if (h.on_error) h.on_error(e.code, e.message);
                return true;
            }
            default:
                return true;  // a newer minor's message (or a stray Welcome): ignored
        }
    }

    static bool bad(const char* what, std::string* why) {
        *why = std::string("malformed ") + what + " from the server";
        return false;
    }

    void run() {
        std::string why;
        bool going = true;
        wire::MessageSplitter::Message m;
        // Whatever arrived together with Welcome first.
        while (going && splitter.next(m)) going = dispatch(m, &why);
        std::unique_ptr<char[]> buf(new char[256u << 10]);
        while (going && open) {
            const size_t n = stream->read(buf.get(), 256u << 10);
            if (n == 0) break;
            splitter.feed(buf.get(), n);
            while (going && splitter.next(m)) going = dispatch(m, &why);
            if (splitter.error()) {
                why = "framing error from the server";
                going = false;
            }
        }
        if (why.empty()) why = !open ? "closed by the client" : !fatal.empty() ? fatal : "the server closed the connection";
        open = false;
        stream->shutdown();
        if (h.on_closed) h.on_closed(why);
    }
};

std::unique_ptr<Client> Client::connect(std::unique_ptr<Stream> stream, ClientHandlers handlers,
                                        const ClientOptions& options, std::string* err) {
    if (!stream) {
        if (err) *err = "no stream";
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->stream = std::move(stream);
    impl->h = std::move(handlers);
    Stream& s = *impl->stream;

    // A watchdog ends the handshake if the server does not answer in time.
    std::mutex wm;
    std::condition_variable wcv;
    bool done = false;
    bool timed_out = false;
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> lk(wm);
        if (!wcv.wait_for(lk, std::chrono::milliseconds(options.connect_timeout_ms), [&] { return done; })) {
            timed_out = true;
            s.shutdown();
        }
    });
    auto finish = [&] {
        {
            std::lock_guard<std::mutex> lk(wm);
            done = true;
        }
        wcv.notify_all();
        watchdog.join();
    };

    std::string failure;
    bool welcomed = false;
    HelloMsg hello;
    hello.name = options.name;
    if (!s.write(hello.encode())) failure = "cannot send Hello";
    std::unique_ptr<char[]> buf(new char[64u << 10]);
    while (failure.empty() && !welcomed) {
        const size_t n = s.read(buf.get(), 64u << 10);
        if (n == 0) {
            failure = "the server closed the connection during the handshake";
            break;
        }
        impl->splitter.feed(buf.get(), n);
        wire::MessageSplitter::Message m;
        while (failure.empty() && !welcomed && impl->splitter.next(m)) {
            if (MsgType(m.type) == MsgType::Welcome) {
                if (!impl->welcome.decode(m.payload)) failure = "malformed Welcome from the server";
                else welcomed = true;
            } else if (MsgType(m.type) == MsgType::Error) {
                ErrorMsg e;
                failure = e.decode(m.payload) ? std::string("refused: ") + error_code_name(e.code) + ": " + e.message
                                              : "malformed Error from the server";
            }
        }
        if (impl->splitter.error()) failure = "framing error from the server";
    }
    finish();
    if (timed_out) failure = "no answer from the server within " + std::to_string(options.connect_timeout_ms) + " ms";
    if (!failure.empty()) {
        const std::string diag = s.diagnostics();
        if (err) *err = diag.empty() ? failure : failure + " (" + diag + ")";
        return nullptr;
    }
    if (!options.codecs.empty()) {
        SetCodecMsg sc;
        sc.codecs = options.codecs;
        sc.max_bitrate_kbps = options.max_bitrate_kbps;
        impl->send(sc.encode());
    }
    Impl* p = impl.get();
    p->reader = std::thread([p] { p->run(); });
    return std::unique_ptr<Client>(new Client(std::move(impl)));
}

Client::Client(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Client::~Client() {
    close();
    if (impl_->reader.joinable()) impl_->reader.join();
}

const WelcomeMsg& Client::welcome() const { return impl_->welcome; }

void Client::ack(uint64_t frame_id) { impl_->send(AckMsg{frame_id}.encode()); }

void Client::request_keyframe() { impl_->send(RequestKeyframeMsg{}.encode()); }

void Client::send_input(const InputEvent& event) { impl_->send(InputMsg{event}.encode()); }

void Client::set_codecs(const std::vector<Codec>& codecs, uint32_t max_bitrate_kbps) {
    SetCodecMsg sc;
    sc.codecs = codecs;
    sc.max_bitrate_kbps = max_bitrate_kbps;
    impl_->send(sc.encode());
}

bool Client::connected() const { return impl_->open.load(); }

void Client::close() {
    if (impl_->open.exchange(false)) impl_->stream->shutdown();
}

}  // namespace broremote
