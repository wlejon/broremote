#include "broremote/viewer.h"

#include <algorithm>
#include <cstdio>

namespace broremote {

namespace {

// A stream owned jointly by the Client and the session, so the session can
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

ViewerSession::ViewerSession(std::function<void()> wake) : wake_(std::move(wake)) {}

ViewerSession::~ViewerSession() {
    std::shared_ptr<Stream> stream, audio_stream;
    {
        std::lock_guard<std::mutex> lk(m_);
        stopping_ = true;
        user_closed_ = true;
        stream = stream_;
        audio_stream = audio_stream_;
    }
    cv_.notify_all();
    // Unblocks a connect in progress (ssh still starting, or the handshake)
    // as well as a live connection's reader.
    if (stream) stream->shutdown();
    if (audio_stream) audio_stream->shutdown();
    if (connector_.joinable()) connector_.join();
    {
        std::lock_guard<std::mutex> lk(m_);
        audio_stream = audio_stream_;
    }
    if (audio_stream) audio_stream->shutdown();
    if (audio_thread_.joinable()) audio_thread_.join();
    std::unique_ptr<AudioSession> audio;
    {
        std::lock_guard<std::mutex> lk(m_);
        audio = std::move(audio_);
    }
    audio.reset();
    if (decoder_thread_.joinable()) decoder_thread_.join();
    if (pinger_.joinable()) pinger_.join();
    client_.reset();  // joins the reader (on_closed takes m_, so not under it)
}

void ViewerSession::say(const std::string& line) const {
    if (options_.log) options_.log(line);
}

void ViewerSession::start(const ViewerOptions& options) {
    options_ = options;
    {
        std::lock_guard<std::mutex> lk(m_);
        want_mic_muted_ = options.audio.mic_muted;
        want_playback_muted_ = options.audio.playback_muted;
    }
    connector_ = std::thread([this, options] { connect_thread(options); });
    decoder_thread_ = std::thread([this] { decode_thread(); });
    pinger_ = std::thread([this] { ping_thread(); });
}

// The round trip and the clock offset, four times a second while connected.
void ViewerSession::ping_thread() {
    std::unique_lock<std::mutex> lk(m_);
    for (;;) {
        cv_.wait_for(lk, std::chrono::milliseconds(250),
                     [&] { return stopping_ || status_.state == ViewerState::Closed; });
        if (stopping_ || status_.state == ViewerState::Closed) return;
        Client* c = live_;
        if (!c) continue;
        lk.unlock();
        c->ping();
        lk.lock();
    }
}

bool ViewerSession::probe() {
    Client* c = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (status_.state != ViewerState::Connected) return false;
        c = live_;
    }
    if (!c) return false;
    latency_.set_probing(true);
    if (!latency_.begin_probe(Clock::now())) return false;
    constexpr uint32_t kKeyF13 = 183;  // evdev KEY_F13: nothing a desktop binds
    c->send_input(InputEvent::key(kKeyF13, true));
    c->send_input(InputEvent::key(kKeyF13, false));
    return true;
}

void ViewerSession::close() {
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

void ViewerSession::set_status(const std::function<void(ViewerStatus&)>& f) {
    {
        std::lock_guard<std::mutex> lk(m_);
        f(status_);
    }
    if (wake_) wake_();
}

ViewerStatus ViewerSession::status() const {
    std::lock_guard<std::mutex> lk(m_);
    return status_;
}

uint64_t ViewerSession::cursor(CursorState& out) const {
    std::lock_guard<std::mutex> lk(m_);
    out = cursor_;
    return cursor_changes_;
}

void ViewerSession::connect_thread(ViewerOptions options) {
    const std::string where = options.target.describe();
    {
        std::lock_guard<std::mutex> lk(m_);
        where_ = where;
    }
    std::string err;
    std::unique_ptr<Stream> opened = open_stream(options.target, &err);
    if (!opened) {
        set_status([&](ViewerStatus& s) {
            s.state = ViewerState::Closed;
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
    const std::vector<Codec> decoders = brovideo::codecs(brovideo::Direction::Decode);
    if (options.negotiate) co.codecs = decoders;
    // Input on a lane of its own: a second connection opened like the first.
    co.open_input_lane = input_lane_opener(options.target);

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
    h.on_cursor = [this](const CursorState& c) {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (cursor_changes_ && c == cursor_) return;
            cursor_ = c;
            ++cursor_changes_;
        }
        if (wake_) wake_();
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
        set_status([&](ViewerStatus& s) {
            s.state = ViewerState::Closed;
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
            status_.server_minor = client_->welcome().minor;
            status_.input_lane = client_->input_lane();
            status_.input_lane_error = client_->input_lane_error();
            // The connection may already have ended (on_closed ran).
            if (status_.state == ViewerState::Connecting) status_.state = ViewerState::Connected;
        }
    }
    cv_.notify_all();
    if (abandon) {
        c.reset();
        return;
    }
    if (live_->input_lane()) {
        say("input on its own lane");
    } else if (!live_->input_lane_error().empty()) {
        say("input on the control connection: " + live_->input_lane_error());
    }
    if (user_closed_) live_->close();
    if (wake_) wake_();
    // The audio lane comes up on its own thread, so video never waits for it.
    if (options.audio.enabled) {
        const std::optional<brolink::lanes::Grant> grant = live_->welcome().grant;
        if (!grant || live_->welcome().minor < 3) {
            say("no audio: the server speaks protocol " + std::to_string(live_->welcome().major) + "." +
                std::to_string(live_->welcome().minor) + " (audio is 1.3)");
        } else {
            std::lock_guard<std::mutex> lk(m_);
            if (!stopping_) audio_thread_ = std::thread([this, options, g = *grant] { audio_thread(options, g); });
        }
    }
}

void ViewerSession::audio_thread(ViewerOptions options, brolink::lanes::Grant grant) {
    const ViewerAudioOptions& ao = options.audio;
    AudioSessionOptions o;
    o.lane.mic = ao.mic;
    o.lane.playback = ao.playback;
    o.mic_device = ao.mic_device;
    o.speaker_device = ao.speaker_device;
    o.jitter_ms = ao.jitter_ms;
    o.max_buffer_ms = std::max<uint32_t>(ao.jitter_ms * 4, ao.jitter_ms + 40);
    if (!ao.mic_file.empty()) {
        audio::WavData wav;
        std::string err;
        if (!audio::read_wav(ao.mic_file, wav, &err)) {
            say("mic file: " + err + "; no mic");
            o.lane.mic = false;
        } else {
            // Mono at the file's own rate: the host's node converts the rate.
            const size_t n = wav.frames.size() / wav.channels;
            mic_file_.resize(n);
            audio::convert_channels(wav.frames.data(), uint32_t(n), wav.channels, mic_file_.data(), 1);
            mic_file_rate_ = wav.rate;
            o.lane.mic_format.rate = wav.rate;
            o.mic_generator = [this](float* out, uint32_t frames) {
                for (uint32_t i = 0; i < frames; ++i) {
                    out[i] = mic_file_.empty() ? 0.0f : mic_file_[mic_file_pos_];
                    if (++mic_file_pos_ >= mic_file_.size()) mic_file_pos_ = 0;
                }
            };
        }
    } else if (ao.mic_tone_hz > 0) {
        mic_tone_ = std::make_unique<audio::Tone>(ao.mic_tone_hz, 0.5, o.lane.mic_format.rate, 1);
        o.mic_generator = [this](float* out, uint32_t frames) { mic_tone_->fill(out, frames); };
    }
    if (ao.record_seconds > 0) {
        auto rec = std::make_unique<Recording>();
        rec->rate = o.lane.playback_format.rate;
        rec->channels = o.lane.playback_format.channels;
        rec->frames.resize(size_t(rec->rate) * ao.record_seconds);
        Recording* r = rec.get();
        o.on_played = [r](const float* f, uint32_t n, int64_t) {
            const size_t at = r->used.load(std::memory_order_relaxed);
            const size_t k = std::min<size_t>(n, r->frames.size() - at);
            for (size_t i = 0; i < k; ++i) r->frames[at + i] = f[i * r->channels];
            r->used.store(at + k, std::memory_order_release);
        };
        recording_ = std::move(rec);
    }

    std::string err;
    std::unique_ptr<Stream> opened = open_stream(options.target, &err);
    if (!opened) {
        say("no audio: cannot open the audio lane: " + err);
        return;
    }
    std::shared_ptr<Stream> shared(std::move(opened));
    {
        std::lock_guard<std::mutex> lk(m_);
        if (stopping_) return;
        audio_stream_ = shared;
    }
    std::unique_ptr<AudioSession> a = AudioSession::start(std::make_unique<SharedStream>(shared), grant, o, &err);
    if (!a) {
        say("no audio: " + err);
        return;
    }
    const AudioSession::Stats st = a->stats();
    std::string line = "audio lane: mic ";
    if (st.mic) {
        line += "-> " + st.mic_node + " (from " + (st.mic_device.empty() ? std::string("?") : st.mic_device) +
                (st.echo_cancel ? ", echo cancelled)" : ")");
    } else {
        line += "off";
    }
    line += ", host audio " + (st.playback ? "-> " + st.speaker_device : std::string("off"));
    if (!st.notes.empty()) line += " [" + st.notes + "]";
    say(line);
    {
        std::lock_guard<std::mutex> lk(m_);
        if (stopping_) return;
        a->set_mic_muted(want_mic_muted_);
        a->set_playback_muted(want_playback_muted_);
        audio_ = std::move(a);
    }
    if (wake_) wake_();
}

bool ViewerSession::audio_stats(AudioSession::Stats& out) const {
    std::lock_guard<std::mutex> lk(m_);
    if (!audio_) return false;
    out = audio_->stats();
    return true;
}

void ViewerSession::set_mic_muted(bool muted) {
    std::lock_guard<std::mutex> lk(m_);
    want_mic_muted_ = muted;
    if (audio_) audio_->set_mic_muted(muted);
}

void ViewerSession::set_playback_muted(bool muted) {
    std::lock_guard<std::mutex> lk(m_);
    want_playback_muted_ = muted;
    if (audio_) audio_->set_playback_muted(muted);
}

bool ViewerSession::mic_muted() const {
    std::lock_guard<std::mutex> lk(m_);
    return audio_ ? audio_->mic_muted() : want_mic_muted_;
}

bool ViewerSession::playback_muted() const {
    std::lock_guard<std::mutex> lk(m_);
    return audio_ ? audio_->playback_muted() : want_playback_muted_;
}

void ViewerSession::recorded_audio(std::vector<float>& frames, uint32_t& rate, uint32_t& channels) const {
    rate = channels = 0;
    frames.clear();
    if (!recording_) return;
    const size_t n = recording_->used.load(std::memory_order_acquire);
    frames.assign(recording_->frames.begin(), recording_->frames.begin() + std::ptrdiff_t(n));
    rate = recording_->rate;
    channels = 1;
}

void ViewerSession::on_closed(const std::string& why) {
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
    set_status([&](ViewerStatus& s) {
        s.state = ViewerState::Closed;
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

void ViewerSession::decode_thread() {
    for (;;) {
        Item item;
        Client* client = nullptr;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] {
                return stopping_ || status_.state == ViewerState::Closed || (live_ && !queue_.empty());
            });
            if (stopping_ || status_.state == ViewerState::Closed) return;
            item = std::move(queue_.front());
            queue_.pop_front();
            client = live_;
        }
        decode_one(item, *client);
    }
}

void ViewerSession::decode_one(Item& item, Client& client) {
    std::string err;
    if (item.is_config) {
        config_ = item.config;
        if (!decoder_ || decoder_codec_ != config_.codec) {
            decoder_.reset();
            DecoderConfig dc;
            dc.codec = config_.codec;  // hardware where it works ($BROVIDEO_HARDWARE=0: software)
            dc.output = options_.output;
            decoder_ = brovideo::create_decoder(dc, &err);
            if (!decoder_) {
                set_status([&](ViewerStatus& s) {
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
        const bool hw = decoder_->hardware();
        set_status([&](ViewerStatus& s) {
            s.have_config = true;
            s.config = config_;
            s.decoder = what.empty() ? codec_name(config_.codec) : what;
            s.hardware = hw;
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
        if (failures <= 10) say("frame " + std::to_string(v.frame_id) + ": " + err);
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
    const bool gpu = work_.memory != brovideo::PictureMemory::Cpu;
    // While probing, the picture's input marker says which presses it answers.
    int64_t marker = -1;
    if (latency_.probing()) {
        marker = gpu ? (options_.read_marker ? options_.read_marker(work_) : -1) : read_marker(work_);
    }
    if (options_.prepare) options_.prepare(work_);
    const auto t2 = Clock::now();
    latency_.on_decoded(v.frame_id, t0, t2, marker);
    {
        std::lock_guard<std::mutex> lk(frame_m_);
        std::swap(latest_, work_);
        ++published_;
        latest_info_.frame_id = v.frame_id;
        latest_info_.sequence = published_;
        latest_info_.received = item.received;
        latest_info_.decoded = t2;
        if (stats_.decoded++ == 0) stats_.first_decoded = t2;
        if (gpu) ++stats_.gpu_pictures;
        stats_.last_decoded = t2;
        if (stats_.decode_ms.size() >= 100000) stats_.decode_ms.erase(stats_.decode_ms.begin(), stats_.decode_ms.begin() + 50000);
        stats_.decode_ms.push_back(std::chrono::duration<double, std::milli>(t2 - t0).count());
    }
    // A GPU picture borrows the decoder's surface: the one the display has
    // not taken goes back now rather than when the next one replaces it.
    work_.gpu = {};
    if (wake_) wake_();
}

bool ViewerSession::take_frame(DecodedFrame& frame, ViewerFrameInfo& info) {
    std::lock_guard<std::mutex> lk(frame_m_);
    if (published_ == taken_) return false;
    std::swap(frame, latest_);
    info = latest_info_;
    taken_ = published_;
    return true;
}

void ViewerSession::send_input(const InputEvent& e) {
    Client* c = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (status_.state != ViewerState::Connected) return;
        c = live_;
    }
    if (c) c->send_input(e);
}

ViewerStats ViewerSession::stats() const {
    std::lock_guard<std::mutex> lk(frame_m_);
    return stats_;
}

}  // namespace broremote
