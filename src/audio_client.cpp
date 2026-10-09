// The viewer's side of the audio lane: AudioClient (the protocol) and
// AudioSession (the devices on its two ends).
#include "broremote/audio_client.h"

#include "watchdog.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace broremote {

std::string local_host_name() {
#ifdef _WIN32
    char buf[256];
    DWORD n = sizeof buf;
    if (GetComputerNameExA(ComputerNameDnsHostname, buf, &n) && n) return std::string(buf, n);
    return "windows";
#else
    char buf[256] = {};
    if (gethostname(buf, sizeof buf - 1) == 0 && buf[0]) return buf;
    return "viewer";
#endif
}

// ---- AudioClient ----------------------------------------------------------------------------

struct AudioClient::Impl {
    std::unique_ptr<Stream> stream;
    AudioLaneHandlers h;
    AudioStartedMsg started;
    wire::MessageSplitter splitter;
    std::mutex write_m;
    std::atomic<bool> open{true};
    std::thread reader;
    std::string fatal;
    uint64_t up_seq = 0;      // under write_m
    std::string pcm;          // under write_m
    std::vector<float> down;  // reader thread

    bool send(const std::string& msg) {
        if (!open) return false;
        std::lock_guard<std::mutex> lk(write_m);
        return stream->write(msg);
    }

    // Reads until a message of type `want` (into *payload), an Error, or the
    // end. Used before the reader thread starts.
    std::string expect(MsgType want, std::string* payload) {
        char buf[16384];
        for (;;) {
            wire::MessageSplitter::Message m;
            while (splitter.next(m)) {
                if (MsgType(m.type) == want) {
                    *payload = std::string(m.payload);
                    return {};
                }
                if (MsgType(m.type) == MsgType::Error) {
                    ErrorMsg e;
                    return e.decode(m.payload) ? std::string("refused: ") + error_code_name(e.code) + ": " + e.message
                                               : "malformed Error on the audio lane";
                }
            }
            if (splitter.error()) return "framing error on the audio lane";
            const size_t n = stream->read(buf, sizeof buf);
            if (n == 0) return "the audio lane closed";
            splitter.feed(buf, n);
        }
    }

    bool dispatch(const wire::MessageSplitter::Message& m, std::string* why) {
        switch (MsgType(m.type)) {
            case MsgType::AudioDown: {
                AudioDataMsg d;
                if (!d.decode(m.payload, MsgType::AudioDown)) return bad("AudioDown", why);
                const audio::Format& f = started.playback_format;
                if (!audio::decode_pcm(reinterpret_cast<const uint8_t*>(d.pcm.data()), d.pcm.size(), f, down)) {
                    return bad("AudioDown (not whole frames)", why);
                }
                if (h.on_audio) h.on_audio(d.seq, d.capture_us, down.data(), uint32_t(down.size() / f.channels));
                return true;
            }
            case MsgType::AudioStats: {
                AudioStatsMsg s;
                if (!s.decode(m.payload)) return bad("AudioStats", why);
                if (h.on_stats) h.on_stats(s);
                return true;
            }
            case MsgType::Pong: {
                const auto received = std::chrono::steady_clock::now();
                PongMsg p;
                if (!p.decode(m.payload)) return bad("Pong", why);
                const std::chrono::steady_clock::time_point sent{std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::nanoseconds(int64_t(p.token)))};
                if (h.on_pong) h.on_pong(sent, received, p.server_time_us);
                return true;
            }
            case MsgType::Error: {
                ErrorMsg e;
                if (!e.decode(m.payload)) return bad("Error", why);
                if (e.code != ErrorCode::UnknownMessage) fatal = std::string(error_code_name(e.code)) + ": " + e.message;
                return true;
            }
            default:
                return true;  // a newer minor's
        }
    }

    static bool bad(const char* what, std::string* why) {
        *why = std::string("malformed ") + what + " from the host";
        return false;
    }

    void run() {
        std::string why;
        bool going = true;
        wire::MessageSplitter::Message m;
        while (going && splitter.next(m)) going = dispatch(m, &why);
        std::unique_ptr<char[]> buf(new char[64u << 10]);
        while (going && open) {
            const size_t n = stream->read(buf.get(), 64u << 10);
            if (n == 0) break;
            splitter.feed(buf.get(), n);
            while (going && splitter.next(m)) going = dispatch(m, &why);
            if (splitter.error()) {
                why = "framing error on the audio lane";
                going = false;
            }
        }
        if (why.empty()) why = !open ? "closed by the viewer" : !fatal.empty() ? fatal : "the host closed the audio lane";
        open = false;
        stream->shutdown();
        if (h.on_closed) h.on_closed(why);
    }
};

std::unique_ptr<AudioClient> AudioClient::connect(std::unique_ptr<Stream> stream, const brolink::lanes::Grant& grant,
                                                  AudioLaneHandlers handlers, const AudioLaneOptions& options,
                                                  std::string* err) {
    if (!stream) {
        if (err) *err = "no stream";
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->stream = std::move(stream);
    impl->h = std::move(handlers);
    Stream& s = *impl->stream;
    Watchdog dog(options.timeout_ms, [&s] { s.shutdown(); });
    std::string failure;
    JoinMsg j;
    j.join.session = grant.session;
    j.join.token = grant.token;
    j.join.lane = std::string(kAudioLane);
    std::string payload;
    if (!s.write(j.encode())) failure = "cannot send Join";
    if (failure.empty()) failure = impl->expect(MsgType::Joined, &payload);
    if (failure.empty()) {
        AudioStartMsg st;
        st.source = options.source.empty() ? local_host_name() : options.source;
        st.playback = options.playback;
        st.playback_format = options.playback_format;
        st.mic = options.mic;
        st.mic_format = options.mic_format;
        if (!s.write(st.encode())) failure = "cannot send AudioStart";
    }
    if (failure.empty()) failure = impl->expect(MsgType::AudioStarted, &payload);
    if (failure.empty() && !impl->started.decode(payload)) failure = "malformed AudioStarted";
    dog.done();
    if (dog.fired()) failure = "no answer on the audio lane within " + std::to_string(options.timeout_ms) + " ms";
    if (!failure.empty()) {
        const std::string diag = s.diagnostics();
        if (err) *err = diag.empty() ? failure : failure + " (" + diag + ")";
        s.shutdown();
        return nullptr;
    }
    Impl* p = impl.get();
    p->reader = std::thread([p] { p->run(); });
    return std::unique_ptr<AudioClient>(new AudioClient(std::move(impl)));
}

AudioClient::AudioClient(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

AudioClient::~AudioClient() {
    close();
    if (impl_->reader.joinable()) impl_->reader.join();
}

const AudioStartedMsg& AudioClient::started() const { return impl_->started; }

bool AudioClient::send_mic(const float* frames, uint32_t n, uint64_t capture_us) {
    Impl& s = *impl_;
    if (!s.open || !s.started.mic) return false;
    std::lock_guard<std::mutex> lk(s.write_m);
    s.pcm.clear();
    audio::encode_pcm(frames, n, s.started.mic_format, s.pcm);
    return s.stream->write(AudioDataMsg::encode_message(MsgType::AudioUp, ++s.up_seq, capture_us, s.pcm));
}

void AudioClient::set_muted(bool playback_muted, bool mic_muted) {
    impl_->send(AudioControlMsg{playback_muted, mic_muted}.encode());
}

bool AudioClient::ping() {
    PingMsg p;
    p.token = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count());
    return impl_->send(p.encode());
}

bool AudioClient::connected() const { return impl_->open.load(); }

void AudioClient::close() {
    if (impl_->open.exchange(false)) impl_->stream->shutdown();
}

// ---- AudioSession ---------------------------------------------------------------------------

namespace {
constexpr uint32_t kMaxUpPacketMs = 20;
constexpr auto kPingInterval = std::chrono::milliseconds(250);
constexpr size_t kOffsetSamples = 16;
}  // namespace

struct AudioSession::Impl {
    AudioSessionOptions opt;
    std::shared_ptr<audio::Backend> backend;
    std::unique_ptr<audio::PcmRing> mic_ring;     // mic (realtime) -> the sender thread
    std::unique_ptr<audio::JitterBuffer> play;    // the lane's reader -> speakers (realtime)
    std::unique_ptr<audio::Endpoint> mic_ep, spk_ep;
    std::unique_ptr<AudioClient> client;
    std::thread sender;
    std::mutex wake_m;
    std::condition_variable wake_cv;
    std::atomic<bool> mic_pending{false};
    std::atomic<bool> stopping{false};
    std::atomic<bool> mic_muted{false}, playback_muted{false};
    std::atomic<uint64_t> mic_packets{0}, mic_overflow{0}, playback_packets{0};

    mutable std::mutex m;  // what follows
    Stats base;            // the fixed parts (devices, notes, flags)
    std::string closed;
    struct Sample {
        int64_t rtt_us;
        int64_t offset_us;  // host clock - this clock
    };
    std::deque<Sample> samples;
    AudioStatsMsg host;
    bool have_host = false;
    uint64_t down_last_seq = 0;

    void note(const std::string& s) { base.notes += (base.notes.empty() ? "" : "; ") + s; }

    bool best_offset(int64_t* offset, int64_t* rtt) const {
        if (samples.empty()) return false;
        const Sample* b = &samples.front();
        for (const Sample& s : samples) {
            if (s.rtt_us < b->rtt_us) b = &s;
        }
        *offset = b->offset_us;
        *rtt = b->rtt_us;
        return true;
    }

    void send_loop() {
        auto next_ping = std::chrono::steady_clock::now();
        const audio::Format f = client->started().mic_format;
        std::vector<float> buf(size_t(f.rate * kMaxUpPacketMs / 1000) * f.channels);
        // Whatever the mic captured while the lane came up is stale.
        if (mic_ring) mic_ring->skip(mic_ring->available());
        while (!stopping) {
            {
                std::unique_lock<std::mutex> lk(wake_m);
                wake_cv.wait_for(lk, std::chrono::milliseconds(5),
                                 [&] { return mic_pending.load(std::memory_order_acquire) || stopping.load(); });
                mic_pending.store(false, std::memory_order_relaxed);
            }
            if (stopping) break;
            if (mic_ring) {
                while (uint32_t avail = mic_ring->available()) {
                    const uint32_t n = std::min<uint32_t>(avail, uint32_t(buf.size() / f.channels));
                    int64_t stamp = 0;
                    mic_ring->read(buf.data(), n, &stamp);
                    if (mic_muted) continue;
                    if (!client->send_mic(buf.data(), n, uint64_t(stamp))) break;
                    mic_packets.fetch_add(1, std::memory_order_relaxed);
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_ping) {
                next_ping = now + kPingInterval;
                client->ping();
            }
        }
    }
};

std::unique_ptr<AudioSession> AudioSession::start(std::unique_ptr<Stream> stream, const brolink::lanes::Grant& grant,
                                                  const AudioSessionOptions& options, std::string* err) {
    auto impl = std::make_unique<Impl>();
    Impl& s = *impl;
    s.opt = options;
    AudioLaneOptions lane = options.lane;
    const bool need_device = (lane.mic && !options.mic_generator) || lane.playback;
    s.backend = options.backend;
    if (!s.backend && need_device) {
        std::string e;
        s.backend = audio::platform_backend(&e);
        if (!s.backend) s.note("no audio devices: " + e);
    }

    // The mic first, so a mic that cannot open is not offered to the host.
    if (lane.mic) {
        const audio::Format& f = lane.mic_format;
        s.mic_ring = std::make_unique<audio::PcmRing>(f.channels, f.rate, f.rate);
        Impl* self = &s;
        audio::CaptureFn deliver = [self](const float* frames, uint32_t n, int64_t t) {
            if (!self->mic_ring->write(frames, n, t)) self->mic_overflow.fetch_add(n, std::memory_order_relaxed);
            self->mic_pending.store(true, std::memory_order_release);
            self->wake_cv.notify_one();
        };
        const uint32_t period = std::max<uint32_t>(1, f.rate * options.period_ms / 1000);
        std::string e;
        if (options.mic_generator) {
            s.mic_ep = audio::generated_capture(f.rate, f.channels, period, options.mic_generator, std::move(deliver),
                                                "generated mic");
        } else if (s.backend) {
            audio::CaptureSpec spec;
            spec.source = audio::Source::Microphone;
            spec.rate = f.rate;
            spec.channels = f.channels;
            spec.period_frames = period;
            spec.name = "broremote mic";
            spec.device = options.mic_device;
            spec.echo_reference = options.speaker_device;
            s.mic_ep = s.backend->open_capture(spec, std::move(deliver), &e);
        }
        if (s.mic_ep && s.mic_ep->start(&e)) {
            const audio::EndpointInfo info = s.mic_ep->info();
            s.base.mic_device = info.device;
            s.base.echo_cancel = info.echo_cancel;
        } else {
            if (s.backend || options.mic_generator) s.note("no mic: " + e);
            s.mic_ep.reset();
            s.mic_ring.reset();
            lane.mic = false;
        }
    }
    if (lane.playback && s.backend) {
        const audio::Format& f = lane.playback_format;
        s.play = std::make_unique<audio::JitterBuffer>(f.channels, f.rate, options.jitter_ms, options.max_buffer_ms);
        Impl* self = &s;
        audio::PlaybackSpec spec;
        spec.sink = audio::Sink::Speakers;
        spec.rate = f.rate;
        spec.channels = f.channels;
        spec.period_frames = std::max<uint32_t>(1, f.rate * options.period_ms / 1000);
        spec.name = "broremote";
        spec.device = options.speaker_device;
        std::string e;
        s.spk_ep = s.backend->open_playback(
            spec,
            [self](float* frames, uint32_t n, int64_t t) {
                self->play->pull(frames, n, t);
                if (self->opt.on_played) self->opt.on_played(frames, n, t);
            },
            &e);
        if (s.spk_ep && s.spk_ep->start(&e)) {
            s.base.speaker_device = s.spk_ep->info().device;
        } else {
            s.note("no speakers: " + e);
            s.spk_ep.reset();
            s.play.reset();
            lane.playback = false;
        }
    } else {
        lane.playback = false;
    }

    AudioLaneHandlers h;
    Impl* self = &s;
    h.on_audio = [self](uint64_t seq, uint64_t capture_us, const float* frames, uint32_t n) {
        self->playback_packets.fetch_add(1, std::memory_order_relaxed);
        if (self->play) self->play->push(frames, n, int64_t(capture_us));
        std::lock_guard<std::mutex> lk(self->m);
        self->down_last_seq = seq;
    };
    h.on_stats = [self](const AudioStatsMsg& st) {
        std::lock_guard<std::mutex> lk(self->m);
        self->host = st;
        self->have_host = true;
    };
    h.on_pong = [self](std::chrono::steady_clock::time_point sent, std::chrono::steady_clock::time_point received,
                       uint64_t server_us) {
        using us = std::chrono::microseconds;
        const int64_t a = std::chrono::duration_cast<us>(sent.time_since_epoch()).count();
        const int64_t b = std::chrono::duration_cast<us>(received.time_since_epoch()).count();
        std::lock_guard<std::mutex> lk(self->m);
        self->samples.push_back({b - a, int64_t(server_us) - (a + (b - a) / 2)});
        if (self->samples.size() > kOffsetSamples) self->samples.pop_front();
    };
    h.on_closed = [self](const std::string& why) {
        std::lock_guard<std::mutex> lk(self->m);
        self->closed = why;
    };
    std::string e;
    s.client = AudioClient::connect(std::move(stream), grant, std::move(h), lane, &e);
    if (!s.client) {
        if (err) *err = e;
        s.mic_ep.reset();
        s.spk_ep.reset();
        return nullptr;
    }
    const AudioStartedMsg& st = s.client->started();
    if (!st.message.empty()) s.note("host: " + st.message);
    if (!st.mic && s.mic_ep) {
        s.mic_ep.reset();
        s.base.mic_device.clear();
    }
    if (st.playback && st.playback_format != lane.playback_format) {
        s.note("the host sends " + st.playback_format.describe() + ", not the " + lane.playback_format.describe() +
               " asked for: playback off");
        s.client->set_muted(true, false);
        s.spk_ep.reset();
        s.base.speaker_device.clear();
    } else if (!st.playback && s.spk_ep) {
        s.spk_ep.reset();
        s.base.speaker_device.clear();
    }
    s.base.mic = st.mic && s.mic_ep;
    s.base.playback = st.playback && s.spk_ep;
    s.base.mic_node = st.mic_node;
    s.client->ping();
    s.sender = std::thread([self] { self->send_loop(); });
    return std::unique_ptr<AudioSession>(new AudioSession(std::move(impl)));
}

AudioSession::AudioSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

AudioSession::~AudioSession() {
    Impl& s = *impl_;
    s.stopping = true;
    {
        std::lock_guard<std::mutex> lk(s.wake_m);
    }
    s.wake_cv.notify_all();
    if (s.sender.joinable()) s.sender.join();
    // The lane's reader pushes into the playback buffer, and the devices'
    // callbacks use the rings: both stop before the rings go.
    s.client.reset();
    s.mic_ep.reset();
    s.spk_ep.reset();
}

void AudioSession::set_mic_muted(bool muted) {
    impl_->mic_muted = muted;
    impl_->client->set_muted(impl_->playback_muted, muted);
}

void AudioSession::set_playback_muted(bool muted) {
    impl_->playback_muted = muted;
    impl_->client->set_muted(muted, impl_->mic_muted);
}

bool AudioSession::mic_muted() const { return impl_->mic_muted; }
bool AudioSession::playback_muted() const { return impl_->playback_muted; }

AudioSession::Stats AudioSession::stats() const {
    const Impl& s = *impl_;
    std::lock_guard<std::mutex> lk(s.m);
    Stats st = s.base;
    st.connected = s.client->connected();
    st.closed = s.closed;
    st.mic_packets = s.mic_packets.load();
    st.mic_overflow = s.mic_overflow.load();
    st.playback_packets = s.playback_packets.load();
    int64_t offset = 0, rtt = 0;
    const bool synced = s.best_offset(&offset, &rtt);
    if (synced) st.rtt_ms = double(rtt) / 1000.0;
    if (s.have_host) {
        st.host_mic_buffer_ms = double(s.host.mic_buffer_us) / 1000.0;
        st.host_mic_underruns = s.host.mic_underruns;
        st.host_mic_dropped = s.host.mic_dropped_frames;
        st.host_status = s.host.status;
        if (!st.host_status.empty()) st.notes += (st.notes.empty() ? "host: " : "; host: ") + st.host_status;
        // The host's clock less the offset is this clock.
        if (synced && s.host.mic_valid) {
            st.mic_latency_ms = double(int64_t(s.host.mic_out_us) - offset - int64_t(s.host.mic_capture_us)) / 1000.0;
        }
    }
    if (s.play) {
        const audio::JitterBuffer::Stats js = s.play->stats();
        st.playback_buffer_ms = double(js.depth_frames) * 1000.0 / double(s.play->rate());
        st.playback_underruns = js.underruns;
        st.playback_dropped = js.dropped + js.overflow + (s.have_host ? s.host.playback_dropped : 0);
        if (synced && js.valid) st.playback_latency_ms = double(js.out_us - (js.stamp_us - offset)) / 1000.0;
    }
    return st;
}

}  // namespace broremote
