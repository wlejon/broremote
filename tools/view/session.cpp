#include "session.h"

#include "picture.h"
#include "test_pattern.h"

#include <algorithm>
#include <cstdio>

namespace broremote::view {

namespace {

// A stream owned jointly by the Client and the Session, so the Session can
// shut it down (to abandon a connect in progress) and read ssh's stderr
// after the Client is gone, without racing the Client's own lifetime.
class SharedStream final : public Stream {
public:
    explicit SharedStream(std::shared_ptr<Stream> s) : s_(std::move(s)) {}
    size_t read(char* buf, size_t n) override { return s_->read(buf, n); }
    bool write(std::string_view data) override { return s_->write(data); }
    void shutdown() override { s_->shutdown(); }
    std::string diagnostics() const override { return s_->diagnostics(); }

private:
    std::shared_ptr<Stream> s_;
};

std::string codec_list(const std::vector<Codec>& v) {
    std::string s;
    for (Codec c : v) s += (s.empty() ? "" : ", ") + std::string(codec_name(c));
    return s;
}

}  // namespace

Session::Session(std::function<void()> wake) : wake_(std::move(wake)) {}

Session::~Session() {
    std::shared_ptr<Stream> stream;
    {
        std::lock_guard<std::mutex> lk(m_);
        stopping_ = true;
        user_closed_ = true;
        stream = stream_;
    }
    cv_.notify_all();
    // Unblocks a connect in progress (ssh still starting, or the handshake)
    // as well as a live connection's reader.
    if (stream) stream->shutdown();
    if (connector_.joinable()) connector_.join();
    if (decoder_thread_.joinable()) decoder_thread_.join();
    if (pinger_.joinable()) pinger_.join();
    client_.reset();  // joins the reader (on_closed takes m_, so not under it)
}

void Session::start(const SessionOptions& options) {
    connector_ = std::thread([this, options] { connect_thread(options); });
    decoder_thread_ = std::thread([this] { decode_thread(); });
    pinger_ = std::thread([this] { ping_thread(); });
}

// The round trip and the clock offset, four times a second while connected.
void Session::ping_thread() {
    std::unique_lock<std::mutex> lk(m_);
    for (;;) {
        cv_.wait_for(lk, std::chrono::milliseconds(250),
                     [&] { return stopping_ || status_.state == SessionState::Closed; });
        if (stopping_ || status_.state == SessionState::Closed) return;
        Client* c = live_;
        if (!c) continue;
        lk.unlock();
        c->ping();
        lk.lock();
    }
}

bool Session::probe() {
    Client* c = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (status_.state != SessionState::Connected) return false;
        c = live_;
    }
    if (!c) return false;
    latency_.set_probing(true);
    if (!latency_.begin_probe(Clock::now())) return false;
    constexpr uint32_t kKeyF13 = 183;  // evdev KEY_F13: nothing a desktop binds
    InputEvent e;
    e.kind = InputKind::Key;
    e.code = kKeyF13;
    e.pressed = true;
    c->send_input(e);
    e.pressed = false;
    c->send_input(e);
    return true;
}

void Session::close() {
    Client* c = nullptr;
    std::shared_ptr<Stream> stream;
    {
        std::lock_guard<std::mutex> lk(m_);
        user_closed_ = true;
        c = live_;
        stream = stream_;
    }
    if (c) c->close();
    else if (stream) stream->shutdown();
}

void Session::set_status(const std::function<void(SessionStatus&)>& f) {
    {
        std::lock_guard<std::mutex> lk(m_);
        f(status_);
    }
    if (wake_) wake_();
}

SessionStatus Session::status() const {
    std::lock_guard<std::mutex> lk(m_);
    return status_;
}

void Session::connect_thread(SessionOptions options) {
    const std::string where = options.target.describe();
    {
        std::lock_guard<std::mutex> lk(m_);
        where_ = where;
    }
    std::string err;
    std::unique_ptr<Stream> opened = tools::open_stream(options.target, &err);
    if (!opened) {
        set_status([&](SessionStatus& s) {
            s.state = SessionState::Closed;
            s.failed = true;
            s.message = "cannot connect to " + where + ": " + err;
        });
        return;
    }
    std::shared_ptr<Stream> shared(std::move(opened));
    {
        std::lock_guard<std::mutex> lk(m_);
        stream_ = shared;
        if (stopping_) return;
    }

    ClientOptions co;
    co.name = options.client_name;
    // Probing the decoders (Media Foundation decodes a keyframe of each
    // codec once) overlaps with ssh starting up.
    const std::vector<Codec> decoders = available_decoders();
    if (options.negotiate) co.codecs = decoders;

    ClientHandlers h;
    h.on_config = [this](const StreamConfig& sc) {
        Item it;
        it.is_config = true;
        it.config = sc;
        it.received = Clock::now();
        std::lock_guard<std::mutex> lk(m_);
        queue_.push_back(std::move(it));
        cv_.notify_all();
    };
    h.on_pong = [this](Clock::time_point sent, Clock::time_point received, uint64_t server_us) {
        latency_.on_pong(sent, received, server_us);
    };
    h.on_frame_sent = [this](const FrameSentMsg& f) { latency_.on_frame_sent(f); };
    h.on_video = [this](const VideoPacket& v) {
        Item it;
        it.packet = v;
        it.received = Clock::now();
        latency_.on_video(v, it.received);
        std::lock_guard<std::mutex> lk(m_);
        queue_.push_back(std::move(it));
        cv_.notify_all();
    };
    h.on_cursor = [last = CursorState{}, first = true](const CursorState& c) mutable {
        // Logged for now (the pointer is drawn locally by the OS).
        if (first || c.shape != last.shape || c.visible != last.visible) {
            std::fprintf(stderr, "broremote-view: server cursor %s%s\n", c.shape.c_str(),
                         c.visible ? "" : " (hidden)");
        }
        first = false;
        last = c;
    };
    h.on_error = [this, decoders](ErrorCode code, const std::string& msg) {
        if (code == ErrorCode::ServerShutdown) {
            std::lock_guard<std::mutex> lk(m_);
            server_shutdown_ = true;
        } else if (code == ErrorCode::NoCommonCodec) {
            std::lock_guard<std::mutex> lk(m_);
            no_common_codec_ = "the server cannot send any codec this machine decodes (decoders here: " +
                               codec_list(decoders) + "): " + msg;
        }
    };
    h.on_closed = [this](const std::string& why) { on_closed(why); };

    std::unique_ptr<Client> c = Client::connect(std::make_unique<SharedStream>(shared), std::move(h), co, &err);
    if (!c) {
        set_status([&](SessionStatus& s) {
            s.state = SessionState::Closed;
            s.failed = !user_closed_;
            s.message = user_closed_ ? "closed" : "cannot connect to " + where + ": " + err;
        });
        return;
    }
    bool abandon = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        abandon = stopping_;
        if (!abandon) {
            client_ = std::move(c);
            live_ = client_.get();
            status_.server = client_->welcome().name;
            // The connection may already have ended (on_closed ran).
            if (status_.state == SessionState::Connecting) status_.state = SessionState::Connected;
        }
    }
    cv_.notify_all();
    if (abandon) {
        c.reset();
        return;
    }
    if (user_closed_) live_->close();
    if (wake_) wake_();
}

void Session::on_closed(const std::string& why) {
    std::shared_ptr<Stream> stream;
    bool user = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        stream = stream_;
        user = user_closed_;
    }
    std::string diag;
    if (!user && stream) {
        // ssh's last words arrive on stderr just after its stdout closes.
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        diag = stream->diagnostics();
    }
    set_status([&](SessionStatus& s) {
        s.state = SessionState::Closed;
        if (user) {
            s.message = "closed";
            s.failed = false;
        } else if (!s.message.empty() && s.failed) {
            // Already closing for a reason of our own (no decoder).
        } else {
            s.message = !no_common_codec_.empty() ? no_common_codec_ : "the connection to " + where_ + " ended: " + why;
            if (!diag.empty()) s.message += "\n" + diag;
            s.failed = !server_shutdown_;
        }
    });
    cv_.notify_all();
}

void Session::decode_thread() {
    for (;;) {
        Item item;
        Client* client = nullptr;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] {
                return stopping_ || status_.state == SessionState::Closed || (live_ && !queue_.empty());
            });
            if (stopping_ || status_.state == SessionState::Closed) return;
            item = std::move(queue_.front());
            queue_.pop_front();
            client = live_;
        }
        decode_one(item, *client);
    }
}

void Session::decode_one(Item& item, Client& client) {
    std::string err;
    if (item.is_config) {
        config_ = item.config;
        if (!decoder_ || decoder_codec_ != config_.codec) {
            decoder_.reset();
            decoder_ = create_decoder(config_.codec, &err);
            if (!decoder_) {
                set_status([&](SessionStatus& s) {
                    s.failed = true;
                    s.message = std::string("this machine cannot decode the server's ") + codec_name(config_.codec) +
                                " stream: " + err;
                });
                client.close();
                return;
            }
            decoder_codec_ = config_.codec;
        }
        keyframe_requested_ = false;
        const std::string what = decoder_->describe();
        set_status([&](SessionStatus& s) {
            s.have_config = true;
            s.config = config_;
            s.decoder = what.empty() ? codec_name(config_.codec) : what;
        });
        return;
    }

    const VideoPacket& v = item.packet;
    {
        std::lock_guard<std::mutex> lk(frame_m_);
        ++stats_.packets;
        stats_.bytes += v.data.size();
    }
    if (!decoder_) {
        client.ack(v.frame_id);
        return;
    }
    if (v.keyframe) keyframe_requested_ = false;
    const auto t0 = Clock::now();
    const bool ok = decoder_->decode(v.data, work_, &err);
    const auto t1 = Clock::now();
    // Ack whatever happened: an unacked frame holds the server's window shut.
    client.ack(v.frame_id);
    if (!ok) {
        bool request = !keyframe_requested_;
        uint64_t failures = 0;
        {
            std::lock_guard<std::mutex> lk(frame_m_);
            failures = ++stats_.failed;
            if (request) ++stats_.keyframe_requests;
        }
        if (failures <= 10) std::fprintf(stderr, "broremote-view: frame %llu: %s\n", (unsigned long long)v.frame_id, err.c_str());
        if (request) {
            client.request_keyframe();
            keyframe_requested_ = true;
        }
        return;
    }
    if (!work_.ready) return;
    // The stream's size is the truth: a decoder can hand back a picture with
    // padding the encoder could not crop (AV1 on some hardware codes 1080
    // lines as 1082). Rows and the UV plane stay where they are; only the
    // visible size shrinks.
    if (config_.width && config_.height) {
        work_.width = std::min(work_.width, config_.width);
        work_.height = std::min(work_.height, config_.height);
    }
    // While probing, the picture's input marker says which presses it answers.
    const int64_t marker = latency_.probing()
                               ? tools::read_block_row(work_, tools::kInputMarkerRow, tools::kPatternBlock)
                               : -1;
    latency_.on_decoded(v.frame_id, t0, t1, marker);
    {
        std::lock_guard<std::mutex> lk(frame_m_);
        std::swap(latest_, work_);
        ++published_;
        latest_info_.frame_id = v.frame_id;
        latest_info_.sequence = published_;
        latest_info_.received = item.received;
        latest_info_.decoded = t1;
        if (stats_.decoded++ == 0) stats_.first_decoded = t1;
        stats_.last_decoded = t1;
        if (stats_.decode_ms.size() >= 100000) stats_.decode_ms.erase(stats_.decode_ms.begin(), stats_.decode_ms.begin() + 50000);
        stats_.decode_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    if (wake_) wake_();
}

bool Session::take_frame(DecodedFrame& frame, FrameInfo& info) {
    std::lock_guard<std::mutex> lk(frame_m_);
    if (published_ == taken_) return false;
    std::swap(frame, latest_);
    info = latest_info_;
    taken_ = published_;
    return true;
}

void Session::send_input(const InputEvent& e) {
    Client* c = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (status_.state != SessionState::Connected) return;
        c = live_;
    }
    if (c) c->send_input(e);
}

SessionStats Session::stats() const {
    std::lock_guard<std::mutex> lk(frame_m_);
    return stats_;
}

}  // namespace broremote::view
