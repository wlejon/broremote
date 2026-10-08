// The viewer-side protocol client, and its input lane.
#include "broremote/client.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace broremote {

namespace {

// Runs `on_timeout` if done() is not called within `ms`.
class Watchdog {
public:
    Watchdog(uint32_t ms, std::function<void()> on_timeout)
        : thread_([this, ms, f = std::move(on_timeout)] {
              std::unique_lock<std::mutex> lk(m_);
              if (!cv_.wait_for(lk, std::chrono::milliseconds(ms), [&] { return done_; })) {
                  fired_ = true;
                  f();
              }
          }) {}
    ~Watchdog() { done(); }
    void done() {
        {
            std::lock_guard<std::mutex> lk(m_);
            done_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    [[nodiscard]] bool fired() const { return fired_; }

private:
    std::mutex m_;
    std::condition_variable cv_;
    bool done_ = false;
    std::atomic<bool> fired_{false};
    std::thread thread_;  // last: starts once the rest exists
};

}  // namespace

struct Client::Impl {
    std::unique_ptr<Stream> stream;
    ClientHandlers h;
    WelcomeMsg welcome;
    wire::MessageSplitter splitter;
    std::mutex write_m;
    std::atomic<bool> open{true};
    std::thread reader;
    std::string fatal;  // the server's last Error, the likely reason it closed

    // The input lane: its own connection, its own writer lock, and a reader
    // that only notices it ending (the server sends nothing on it but an
    // Error, or a Pong).
    std::unique_ptr<Stream> lane;
    std::mutex lane_write_m;
    std::atomic<bool> lane_open{false};
    std::thread lane_reader;
    mutable std::mutex lane_m;
    std::string lane_error;

    bool send(const std::string& msg) {
        if (!open) return false;
        std::lock_guard<std::mutex> lk(write_m);
        return stream->write(msg);
    }

    void set_lane_error(std::string why) {
        std::lock_guard<std::mutex> lk(lane_m);
        if (lane_error.empty()) lane_error = std::move(why);
    }

    // Input on the lane while it is up; on the control connection otherwise.
    bool send_input(const std::string& msg) {
        if (lane_open) {
            std::lock_guard<std::mutex> lk(lane_write_m);
            if (lane_open && lane->write(msg)) return true;
            lane_open = false;
            set_lane_error("the input lane closed");
        }
        return send(msg);
    }

    // Join the lane the server granted. Not fatal: on failure input stays on
    // the control connection and lane_error says why.
    void join_lane(uint32_t timeout_ms) {
        if (!lane) return;
        if (!welcome.grant) {
            set_lane_error("the server offers no lanes (it speaks protocol " + std::to_string(welcome.major) + "." +
                           std::to_string(welcome.minor) + ")");
            lane->shutdown();
            lane.reset();
            return;
        }
        Stream& ls = *lane;
        Watchdog dog(timeout_ms, [&ls] { ls.shutdown(); });
        JoinMsg j;
        j.join.session = welcome.grant->session;
        j.join.token = welcome.grant->token;
        j.join.lane = std::string(kInputLane);
        std::string failure;
        bool joined = false;
        if (!ls.write(j.encode())) failure = "cannot send Join";
        wire::MessageSplitter sp;
        char buf[4096];
        while (failure.empty() && !joined) {
            const size_t n = ls.read(buf, sizeof buf);
            if (n == 0) {
                failure = "the input lane closed during its Join";
                break;
            }
            sp.feed(buf, n);
            wire::MessageSplitter::Message m;
            while (failure.empty() && !joined && sp.next(m)) {
                if (MsgType(m.type) == MsgType::Joined) {
                    joined = true;
                } else if (MsgType(m.type) == MsgType::Error) {
                    ErrorMsg e;
                    failure = e.decode(m.payload) ? std::string("refused: ") + error_code_name(e.code) + ": " + e.message
                                                  : "malformed Error on the input lane";
                }
            }
            if (sp.error()) failure = "framing error on the input lane";
        }
        dog.done();
        if (dog.fired()) failure = "no answer to Join within " + std::to_string(timeout_ms) + " ms";
        if (!joined) {
            const std::string diag = ls.diagnostics();
            set_lane_error(diag.empty() ? failure : failure + " (" + diag + ")");
            ls.shutdown();
            lane.reset();
            return;
        }
        lane_open = true;
        lane_reader = std::thread([this] {
            char b[4096];
            while (lane->read(b, sizeof b) > 0) {
            }
            if (lane_open.exchange(false)) set_lane_error("the input lane closed");
        });
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
            case MsgType::Pong: {
                const auto received = std::chrono::steady_clock::now();
                PongMsg p;
                if (!p.decode(m.payload)) return bad("Pong", why);
                // The token is the steady clock (ns) when ping() sent it.
                const std::chrono::steady_clock::time_point sent{std::chrono::steady_clock::duration(
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::nanoseconds(int64_t(p.token))))};
                if (h.on_pong) h.on_pong(sent, received, p.server_time_us);
                return true;
            }
            case MsgType::FrameSent: {
                FrameSentMsg f;
                if (!f.decode(m.payload)) return bad("FrameSent", why);
                if (h.on_frame_sent) h.on_frame_sent(f);
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
        if (lane) lane->shutdown();
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

    // The input lane's connection starts now, so it comes up (an ssh
    // connecting, say) while the control connection's handshake runs.
    if (options.open_input_lane) {
        std::string lerr;
        impl->lane = options.open_input_lane(&lerr);
        if (!impl->lane) impl->set_lane_error("cannot open the input lane: " + lerr);
    }

    // A watchdog ends the handshake if the server does not answer in time.
    Watchdog dog(options.connect_timeout_ms, [&s] { s.shutdown(); });

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
    dog.done();
    if (dog.fired()) failure = "no answer from the server within " + std::to_string(options.connect_timeout_ms) + " ms";
    if (!failure.empty()) {
        if (impl->lane) impl->lane->shutdown();
        const std::string diag = s.diagnostics();
        if (err) *err = diag.empty() ? failure : failure + " (" + diag + ")";
        return nullptr;
    }
    // Joined before anything else is sent, so input never changes connection
    // mid-stream (except to fall back if the lane ends).
    impl->join_lane(options.connect_timeout_ms);
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
    if (impl_->lane) impl_->lane->shutdown();
    if (impl_->lane_reader.joinable()) impl_->lane_reader.join();
}

const WelcomeMsg& Client::welcome() const { return impl_->welcome; }

void Client::ack(uint64_t frame_id) { impl_->send(AckMsg{frame_id}.encode()); }

void Client::request_keyframe() { impl_->send(RequestKeyframeMsg{}.encode()); }

void Client::send_input(const InputEvent& event) {
    if (impl_->open) impl_->send_input(InputMsg{event}.encode());
}

void Client::set_codecs(const std::vector<Codec>& codecs, uint32_t max_bitrate_kbps) {
    SetCodecMsg sc;
    sc.codecs = codecs;
    sc.max_bitrate_kbps = max_bitrate_kbps;
    impl_->send(sc.encode());
}

bool Client::ping() {
    if (impl_->welcome.minor < 1) return false;
    const auto now = std::chrono::steady_clock::now();
    PingMsg p;
    p.token = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
    return impl_->send(p.encode());
}

bool Client::input_lane() const { return impl_->lane_open.load(); }

std::string Client::input_lane_error() const {
    std::lock_guard<std::mutex> lk(impl_->lane_m);
    return impl_->lane_error;
}

bool Client::connected() const { return impl_->open.load(); }

void Client::close() {
    if (impl_->open.exchange(false)) {
        impl_->stream->shutdown();
        if (impl_->lane) impl_->lane->shutdown();
    }
}

}  // namespace broremote
