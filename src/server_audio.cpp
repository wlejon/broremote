// The server's audio lanes (protocol 1.3).
//
// A viewer's audio lane starts with AudioStart. For the host's audio the
// server opens a monitor capture (what this machine plays) whose realtime
// callback writes a PcmRing and wakes the audio thread, which packs what is
// there into AudioDown messages on the lane at once (a packet is whatever
// one device period delivered: no batching, latency first). For the mic it
// opens a virtual mic node whose realtime callback pulls a JitterBuffer the
// I/O thread fills from AudioUp. Both endpoints live exactly as long as the
// lane. The audio thread also sends AudioStats twice a second.
//
// An audio lane that backs up (a viewer not reading) loses its oldest
// unsent audio rather than queueing it: audio that late is worthless, and
// the lane's queue stays short. Nothing here touches the control connection
// or the input lane, so audio failing leaves video and input as they were.
#include "server_impl.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace broremote {

namespace {

// AudioDown messages queued unsent on one lane before the oldest go.
constexpr size_t kMaxQueuedAudio = 16;
// The largest AudioDown packet: a monitor delivering a burst is split.
constexpr uint32_t kMaxPacketMs = 20;
constexpr auto kStatsInterval = std::chrono::milliseconds(500);

SharedMessage shared(std::string s) { return std::make_shared<const std::string>(std::move(s)); }

MsgType message_type(const std::string& m) {
    if (m.size() < wire::kHeaderBytes) return MsgType(0);
    return MsgType(uint16_t(uint8_t(m[4]) | (uint8_t(m[5]) << 8)));
}

// A node name: the viewer's name reduced to [a-z0-9-].
std::string node_token(const std::string& s) {
    std::string out;
    for (char ch : s) {
        const char c = char(std::tolower(static_cast<unsigned char>(ch)));
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out += c;
        else if (!out.empty() && out.back() != '-') out += '-';
        if (out.size() >= 48) break;
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "viewer" : out;
}

}  // namespace

AudioPeer* Server::Impl::audio_peer(brolink::ConnId lane) {
    for (auto& p : audio_peers) {
        if (p->lane == lane) return p.get();
    }
    return nullptr;
}

void Server::Impl::drop_audio_peer(brolink::ConnId lane) {
    auto it = std::find_if(audio_peers.begin(), audio_peers.end(), [lane](const auto& p) { return p->lane == lane; });
    if (it == audio_peers.end()) return;
    // The endpoints stop (their callbacks finish) before the rings go.
    std::unique_ptr<AudioPeer> peer = std::move(*it);
    audio_peers.erase(it);
    peer->monitor.reset();
    peer->mic_node.reset();
}

void Server::Impl::handle_audio_message(ClientConn& c, uint16_t type, std::string_view payload) {
    AudioPeer* peer = audio_peer(c.id);
    switch (MsgType(type)) {
        case MsgType::AudioStart: {
            if (peer) return close_client(c, ErrorCode::BadMessage, "AudioStart sent twice");
            AudioStartMsg s;
            if (!s.decode(payload)) return close_client(c, ErrorCode::BadMessage, "malformed AudioStart");
            start_audio(c, s);
            return;
        }
        case MsgType::AudioUp: {
            if (!peer) return close_client(c, ErrorCode::BadMessage, "AudioUp before AudioStart");
            AudioDataMsg d;
            if (!d.decode(payload, MsgType::AudioUp)) return close_client(c, ErrorCode::BadMessage, "malformed AudioUp");
            ++stats.audio_up;
            if (!peer->mic) return;  // the host said no mic: ignored
            const audio::Format& f = peer->started.mic_format;
            if (!audio::decode_pcm(reinterpret_cast<const uint8_t*>(d.pcm.data()), d.pcm.size(), f, peer->decoded)) {
                return close_client(c, ErrorCode::BadMessage, "AudioUp is not whole frames of the mic format");
            }
            if (peer->up_seq && d.seq != peer->up_seq + 1) ++peer->up_gaps;
            peer->up_seq = d.seq;
            if (peer->mic_muted) return;
            const uint32_t frames = uint32_t(peer->decoded.size() / f.channels);
            if (frames) peer->mic->push(peer->decoded.data(), frames, int64_t(d.capture_us));
            return;
        }
        case MsgType::AudioControl: {
            if (!peer) return close_client(c, ErrorCode::BadMessage, "AudioControl before AudioStart");
            AudioControlMsg ctl;
            if (!ctl.decode(payload)) return close_client(c, ErrorCode::BadMessage, "malformed AudioControl");
            peer->playback_muted = ctl.playback_muted;
            peer->mic_muted = ctl.mic_muted;
            return;
        }
        case MsgType::Ping:
            handle_ping(c, payload);
            return;
        case MsgType::Hello:
        case MsgType::Join:
            close_client(c, ErrorCode::BadMessage, "Hello or Join on a lane");
            return;
        default:
            queue(c, shared(ErrorMsg{ErrorCode::UnknownMessage,
                                     "message type " + std::to_string(type) + " does not go on the audio lane"}
                                .encode()));
            return;
    }
}

void Server::Impl::start_audio(ClientConn& c, const AudioStartMsg& s) {
    auto peer = std::make_unique<AudioPeer>();
    AudioPeer& p = *peer;
    p.lane = c.id;
    p.source = s.source.empty() ? "viewer" : s.source;
    AudioStartedMsg& st = p.started;
    st.playback_format = s.playback_format;
    st.mic_format = s.mic_format;
    std::string why;
    auto note = [&why](const std::string& w) { why += (why.empty() ? "" : "; ") + w; };

    if (!cfg.audio.enabled) {
        note("audio is off on this host");
    } else if (!audio_backend && !audio_backend_tried) {
        audio_backend_tried = true;
        audio_backend = cfg.audio.backend;
        if (!audio_backend) audio_backend = audio::platform_backend(&audio_backend_error);
    }
    if (cfg.audio.enabled && !audio_backend) note("no audio devices on this host: " + audio_backend_error);
    const std::shared_ptr<audio::Backend> backend = cfg.audio.enabled ? audio_backend : nullptr;
    const brolink::lanes::Registry::Member* member = lanes.member(c.id);
    const std::string session = member ? std::to_string(member->session) : "0";

    if (backend && s.playback) {
        const audio::Format& f = s.playback_format;
        p.down = std::make_unique<audio::PcmRing>(f.channels, f.rate, f.rate);  // a second
        audio::CaptureSpec spec;
        spec.source = audio::Source::Monitor;
        spec.rate = f.rate;
        spec.channels = f.channels;
        spec.period_frames = std::max<uint32_t>(1, f.rate * cfg.audio.period_ms / 1000);
        spec.name = "broremote.desktop." + session;
        Impl* self = this;
        std::string err;
        p.monitor = backend->open_capture(
            spec,
            [self, &p](const float* frames, uint32_t n, int64_t t) {
                if (!p.down->write(frames, n, t)) p.down_overflow.fetch_add(n, std::memory_order_relaxed);
                self->audio_pending.store(true, std::memory_order_release);
                self->audio_cv.notify_one();
            },
            &err);
        if (p.monitor && p.monitor->start(&err)) {
            st.playback = true;
        } else {
            note("cannot capture this machine's audio: " + err);
            p.monitor.reset();
            p.down.reset();
        }
    }
    if (backend && s.mic) {
        const audio::Format& f = s.mic_format;
        p.mic = std::make_unique<audio::JitterBuffer>(f.channels, f.rate, cfg.audio.jitter_ms, cfg.audio.max_buffer_ms);
        audio::PlaybackSpec spec;
        spec.sink = audio::Sink::VirtualMic;
        spec.rate = f.rate;
        spec.channels = f.channels;
        spec.period_frames = std::max<uint32_t>(1, f.rate * cfg.audio.period_ms / 1000);
        spec.name = "broremote.mic." + node_token(p.source) + "." + session;
        spec.description = "broremote: " + p.source + " mic";
        spec.make_default = cfg.audio.mic_as_default;
        std::string err;
        p.mic_node = backend->open_playback(
            spec, [&p](float* frames, uint32_t n, int64_t t) { p.mic->pull(frames, n, t); }, &err);
        if (p.mic_node && p.mic_node->start(&err)) {
            st.mic = true;
            st.mic_node = spec.description;
            p.mic_default = p.mic_node->info().is_default;
        } else {
            note("cannot make a microphone here: " + err);
            p.mic_node.reset();
            p.mic.reset();
        }
    }
    st.message = why;
    p.next_stats = Clock::now() + kStatsInterval;
    ++stats.audio_lanes;
    queue(c, shared(st.encode()));
    audio_peers.push_back(std::move(peer));
}

void Server::Impl::queue_audio(ClientConn& c, AudioPeer& p, SharedMessage msg) {
    // Drop the oldest unsent AudioDown while too many wait; never the
    // message the loop already has, nor anything else (Pong, AudioStats).
    size_t waiting = 0;
    for (size_t i = c.handed ? 1 : 0; i < c.out.size(); ++i) {
        if (message_type(*c.out[i]) == MsgType::AudioDown) ++waiting;
    }
    for (size_t i = c.handed ? 1 : 0; waiting >= kMaxQueuedAudio && i < c.out.size();) {
        if (message_type(*c.out[i]) == MsgType::AudioDown) {
            c.out_bytes -= c.out[i]->size();
            c.out.erase(c.out.begin() + std::ptrdiff_t(i));
            --waiting;
            ++p.down_dropped;
        } else {
            ++i;
        }
    }
    queue(c, std::move(msg));
}

void Server::Impl::pump_audio(AudioPeer& p, Clock::time_point now) {
    ClientConn* c = find(p.lane);
    if (!c || c->dead || c->closing || c->closed) return;
    if (p.down) {
        const audio::Format& f = p.started.playback_format;
        const uint32_t max_frames = f.rate * kMaxPacketMs / 1000;
        p.scratch.resize(size_t(max_frames) * f.channels);
        while (uint32_t avail = p.down->available()) {
            const uint32_t n = std::min(avail, max_frames);
            int64_t stamp = 0;
            p.down->read(p.scratch.data(), n, &stamp);
            if (p.playback_muted) continue;
            p.pcm.clear();
            audio::encode_pcm(p.scratch.data(), n, f, p.pcm);
            queue_audio(*c, p, shared(AudioDataMsg::encode_message(MsgType::AudioDown, ++p.down_seq, uint64_t(stamp), p.pcm)));
            ++stats.audio_down;
        }
    }
    if (p.mic && now >= p.next_stats) {
        p.next_stats = now + kStatsInterval;
        const audio::JitterBuffer::Stats js = p.mic->stats();
        AudioStatsMsg sm;
        sm.mic_valid = js.valid;
        sm.mic_capture_us = uint64_t(js.stamp_us);
        sm.mic_out_us = uint64_t(js.out_us);
        sm.mic_buffer_us = uint32_t(uint64_t(js.depth_frames) * 1000000 / p.mic->rate());
        sm.mic_underruns = js.underruns;
        sm.mic_dropped_frames = js.dropped + js.overflow;
        sm.playback_dropped = p.down_dropped;
        queue(*c, shared(sm.encode()));
    }
}

void Server::Impl::audio_loop() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(audio_wake_m);
            audio_cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
                return audio_pending.load(std::memory_order_acquire) || audio_stop.load();
            });
            audio_pending.store(false, std::memory_order_relaxed);
        }
        if (audio_stop.load()) return;
        bool any = false;
        {
            std::lock_guard<std::mutex> lk(m);
            if (stop) return;
            const auto now = Clock::now();
            for (auto& p : audio_peers) {
                pump_audio(*p, now);
                any = true;
            }
        }
        if (any) waker.wake();
    }
}

std::vector<Server::AudioViewer> Server::audio_viewers() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    std::vector<AudioViewer> out;
    for (const auto& p : impl_->audio_peers) {
        AudioViewer v;
        v.source = p->source;
        v.playback = p->started.playback;
        v.playback_muted = p->playback_muted;
        v.mic = p->started.mic;
        v.mic_muted = p->mic_muted;
        v.mic_node = p->started.mic_node;
        v.mic_default = p->mic_default;
        if (p->mic) {
            const audio::JitterBuffer::Stats js = p->mic->stats();
            v.mic_buffer_ms = double(js.depth_frames) * 1000.0 / double(p->mic->rate());
            v.mic_underruns = js.underruns;
            v.mic_dropped_frames = js.dropped + js.overflow;
        }
        out.push_back(std::move(v));
    }
    return out;
}

}  // namespace broremote
