// The client side of text (protocol 1.6): text_client.h.
#include "broremote/text_client.h"

#include "watchdog.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace broremote {

struct TextClient::Impl {
    std::unique_ptr<Stream> stream;
    wire::MessageSplitter splitter;
    WelcomeMsg welcome;
    std::mutex write_m;
    std::thread reader;

    mutable std::mutex m;  // everything below
    bool open = true;
    std::string reason;
    uint64_t next_id = 1;
    std::map<uint64_t, Callback> pending;
    std::function<void(const std::string&)> closed_cb;

    // Reads until a message of type `want` (into *payload), an Error, or the
    // end. Before the reader starts.
    std::string expect(MsgType want, std::string* payload) {
        char buf[16384];
        for (;;) {
            wire::MessageSplitter::Message msg;
            while (splitter.next(msg)) {
                if (MsgType(msg.type) == want) {
                    *payload = std::string(msg.payload);
                    return {};
                }
                if (MsgType(msg.type) == MsgType::Error) {
                    ErrorMsg e;
                    return e.decode(msg.payload) ? std::string("refused: ") + error_code_name(e.code) + ": " + e.message
                                                 : "malformed Error from the host";
                }
            }
            if (splitter.error()) return "framing error from the host";
            const size_t n = stream->read(buf, sizeof buf);
            if (n == 0) return "the connection closed during the handshake";
            splitter.feed(buf, n);
        }
    }

    // Reader thread: one message. False ends the connection with *why.
    bool dispatch(const wire::MessageSplitter::Message& msg, std::string* why) {
        switch (MsgType(msg.type)) {
            case MsgType::TextReply: {
                TextReplyMsg r;
                if (!r.decode(msg.payload)) {
                    *why = "malformed TextReply from the host";
                    return false;
                }
                Callback cb;
                {
                    std::lock_guard<std::mutex> lk(m);
                    auto it = pending.find(r.id);
                    if (it == pending.end()) return true;  // not ours (or already failed)
                    cb = std::move(it->second);
                    pending.erase(it);
                }
                if (cb) cb(TextReply{r.ok, std::move(r.payload), std::move(r.file)});
                return true;
            }
            case MsgType::Error: {
                ErrorMsg e;
                if (!e.decode(msg.payload)) {
                    *why = "malformed Error from the host";
                    return false;
                }
                if (e.code == ErrorCode::UnknownMessage) return true;
                *why = std::string(error_code_name(e.code)) + ": " + e.message;
                return true;  // the host closes after a fatal one
            }
            default:
                return true;  // a newer minor's
        }
    }

    void run() {
        std::string why;
        bool going = true;
        wire::MessageSplitter::Message msg;
        while (going && splitter.next(msg)) going = dispatch(msg, &why);
        std::unique_ptr<char[]> buf(new char[64u << 10]);
        while (going) {
            const size_t n = stream->read(buf.get(), 64u << 10);
            if (n == 0) break;
            splitter.feed(buf.get(), n);
            while (going && splitter.next(msg)) going = dispatch(msg, &why);
            if (splitter.error()) {
                why = "framing error from the host";
                going = false;
            }
        }
        std::map<uint64_t, Callback> orphans;
        std::function<void(const std::string&)> closed;
        {
            std::lock_guard<std::mutex> lk(m);
            if (why.empty()) why = open ? "the host closed the connection" : "closed";
            const std::string diag = stream->diagnostics();
            if (open && !diag.empty()) why += " (" + diag + ")";
            open = false;
            reason = why;
            orphans.swap(pending);
            closed = closed_cb;
        }
        stream->shutdown();
        for (auto& [id, cb] : orphans) {
            if (cb) cb(TextReply{false, "the connection closed before a reply: " + why, {}});
        }
        if (closed) closed(why);
    }

    static std::unique_ptr<TextClient> finish(std::shared_ptr<Impl> impl, std::string failure, Watchdog& dog,
                                              uint32_t timeout_ms, std::string* err) {
        dog.done();
        if (dog.fired()) failure = "no answer from the host within " + std::to_string(timeout_ms) + " ms";
        if (!failure.empty()) {
            const std::string diag = impl->stream->diagnostics();
            if (err) *err = diag.empty() ? failure : failure + " (" + diag + ")";
            impl->stream->shutdown();
            return nullptr;
        }
        Impl* p = impl.get();
        p->reader = std::thread([p] { p->run(); });
        return std::unique_ptr<TextClient>(new TextClient(std::move(impl)));
    }
};

std::unique_ptr<TextClient> TextClient::open(std::unique_ptr<Stream> stream, const TextClientOptions& options,
                                             std::string* err) {
    if (!stream) {
        if (err) *err = "no stream";
        return nullptr;
    }
    auto impl = std::make_shared<Impl>();
    impl->stream = std::move(stream);
    Stream& s = *impl->stream;
    Watchdog dog(options.timeout_ms, [&s] { s.shutdown(); });
    std::string failure;
    HelloMsg hello;
    hello.name = options.name;
    hello.flags = kHelloNoVideo;
    std::string payload;
    if (!s.write(hello.encode())) failure = "cannot send Hello";
    if (failure.empty()) failure = impl->expect(MsgType::Welcome, &payload);
    if (failure.empty() && !impl->welcome.decode(payload)) failure = "malformed Welcome from the host";
    if (failure.empty() && impl->welcome.minor < 6) {
        failure = "the host speaks protocol " + std::to_string(impl->welcome.major) + "." +
                  std::to_string(impl->welcome.minor) + "; text requests need 1.6 (update broremote there)";
    }
    return Impl::finish(std::move(impl), std::move(failure), dog, options.timeout_ms, err);
}

std::unique_ptr<TextClient> TextClient::join(std::unique_ptr<Stream> stream, const brolink::lanes::Grant& grant,
                                             const TextClientOptions& options, std::string* err) {
    if (!stream) {
        if (err) *err = "no stream";
        return nullptr;
    }
    auto impl = std::make_shared<Impl>();
    impl->stream = std::move(stream);
    Stream& s = *impl->stream;
    Watchdog dog(options.timeout_ms, [&s] { s.shutdown(); });
    std::string failure;
    JoinMsg j;
    j.join.session = grant.session;
    j.join.token = grant.token;
    j.join.lane = std::string(kTextLane);
    std::string payload;
    if (!s.write(j.encode())) failure = "cannot send Join";
    if (failure.empty()) failure = impl->expect(MsgType::Joined, &payload);
    return Impl::finish(std::move(impl), std::move(failure), dog, options.timeout_ms, err);
}

TextClient::TextClient(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

TextClient::~TextClient() {
    close();
    if (impl_->reader.joinable()) impl_->reader.join();
}

void TextClient::request(std::vector<std::string> argv, size_t out_arg, Callback done) {
    Impl& s = *impl_;
    TextRequestMsg req;
    std::string closed;
    {
        std::lock_guard<std::mutex> lk(s.m);
        if (!s.open) {
            closed = s.reason.empty() ? "closed" : s.reason;
        } else {
            req.id = s.next_id++;
            s.pending.emplace(req.id, std::move(done));
        }
    }
    if (!closed.empty()) {
        if (done) done(TextReply{false, "not connected: " + closed, {}});
        return;
    }
    req.argv = std::move(argv);
    req.out_arg = out_arg;
    bool sent = false;
    {
        std::lock_guard<std::mutex> lk(s.write_m);
        sent = s.stream->write(req.encode());
    }
    if (sent) return;
    // The write failed: the reader will see the end, but this request is
    // answered now (unless the reader got to it first).
    Callback cb;
    {
        std::lock_guard<std::mutex> lk(s.m);
        auto it = s.pending.find(req.id);
        if (it != s.pending.end()) {
            cb = std::move(it->second);
            s.pending.erase(it);
        }
    }
    if (cb) cb(TextReply{false, "the connection to the host is broken", {}});
}

TextReply TextClient::request_sync(std::vector<std::string> argv, size_t out_arg) {
    struct Wait {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        TextReply reply;
    };
    auto w = std::make_shared<Wait>();
    request(std::move(argv), out_arg, [w](TextReply r) {
        {
            std::lock_guard<std::mutex> lk(w->m);
            w->reply = std::move(r);
            w->done = true;
        }
        w->cv.notify_all();
    });
    std::unique_lock<std::mutex> lk(w->m);
    w->cv.wait(lk, [&] { return w->done; });
    return std::move(w->reply);
}

const WelcomeMsg& TextClient::welcome() const { return impl_->welcome; }

bool TextClient::connected() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    return impl_->open;
}

std::string TextClient::closed_reason() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    return impl_->reason;
}

void TextClient::on_closed(std::function<void(const std::string&)> f) {
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->closed_cb = std::move(f);
}

void TextClient::close() {
    {
        std::lock_guard<std::mutex> lk(impl_->m);
        if (!impl_->open) return;
        impl_->open = false;
    }
    impl_->stream->shutdown();
}

}  // namespace broremote
