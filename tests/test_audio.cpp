// The audio lane (protocol 1.3) in one process over real local sockets, with
// the timer-paced device backend on both ends: the wire format, the jitter
// buffer, and a known tone each way through the lane, checked for its
// frequency, its level and its latency (measured from the tone's onset, on
// the one clock both ends share here, and compared with the latency the
// session reports from its clock-offset arithmetic). Also: refusals, mute,
// and an audio lane ending without touching the control connection.
#include "broremote/audio.h"
#include "broremote/audio_client.h"
#include "broremote/client.h"
#include "broremote/protocol.h"
#include "broremote/server.h"
#include "check.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <random>
#include <thread>

using namespace broremote;
namespace au = broremote::audio;

namespace {

std::string unique_name(const char* what) {
    static std::random_device rd;
    return std::string("ta-") + what + "-" + std::to_string(rd() % 1000000);
}

std::unique_ptr<Stream> dial(const std::string& name) {
    std::string err;
    auto s = connect_local(name, &err);
    if (!s) std::printf("   connect_local: %s\n", err.c_str());
    return s;
}

void settle(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

double db(double amplitude) { return 20.0 * std::log10(amplitude / std::sqrt(2.0)); }

// A tone that starts at the first block generated at or after `on_us` (the
// steady clock), and records when that block was captured, as the paced
// endpoint stamps it (a period before it is delivered).
struct OnsetTone {
    double hz, amp;
    int64_t on_us = 0;
    uint32_t period_frames = 0;
    std::atomic<int64_t> onset_capture_us{0};
    std::unique_ptr<au::Tone> tone;
    void fill(float* out, uint32_t n, uint32_t ch, uint32_t rate) {
        if (!tone) tone = std::make_unique<au::Tone>(hz, amp, rate, ch);
        const int64_t now = au::now_us();
        if (now < on_us) {
            std::fill(out, out + size_t(n) * ch, 0.0f);
            return;
        }
        if (onset_capture_us.load() == 0) onset_capture_us = now - int64_t(n) * 1000000 / rate;
        tone->fill(out, n);
    }
};

// What a paced sink played, with the time its first loud sample left.
struct Recorder {
    std::mutex m;
    std::vector<float> frames;  // first channel only
    uint32_t rate = 0;
    int64_t onset_out_us = 0;
    void add(const float* f, uint32_t n, uint32_t ch, uint32_t r, int64_t t) {
        std::lock_guard<std::mutex> lk(m);
        rate = r;
        for (uint32_t i = 0; i < n; ++i) {
            const float v = f[size_t(i) * ch];
            if (!onset_out_us && std::abs(v) >= 0.05f) onset_out_us = t + int64_t(i) * 1000000 / r;
            frames.push_back(v);
        }
    }
    // The analysis of the last `ms` recorded.
    au::ToneAnalysis tail(uint32_t ms) {
        std::lock_guard<std::mutex> lk(m);
        const size_t want = size_t(rate) * ms / 1000;
        const size_t n = std::min(want, frames.size());
        return au::analyze(frames.data() + (frames.size() - n), n, 1, rate ? rate : 48000);
    }
};

// ---------------------------------------------------------------------------------

void test_wire() {
    check::phase("wire");
    AudioStartMsg s;
    s.source = "laptop";
    s.playback_format = {44100, 2, au::SampleFormat::F32};
    s.mic_format = {16000, 1, au::SampleFormat::S16};
    std::string msg = s.encode();
    wire::MessageSplitter sp;
    sp.feed(msg.data(), msg.size());
    wire::MessageSplitter::Message m;
    CHECK(sp.next(m));
    CHECK_EQ(m.type, uint16_t(MsgType::AudioStart));
    AudioStartMsg d;
    CHECK(d.decode(m.payload));
    CHECK_EQ(d.source, std::string("laptop"));
    CHECK(d.playback_format == s.playback_format);
    CHECK(d.mic_format == s.mic_format);

    // An invalid format is malformed: 0 channels, a rate out of range, an unknown sample format.
    for (int bad = 0; bad < 3; ++bad) {
        wire::Writer w;
        w.str("x");
        w.boolean(true);
        w.varint(bad == 1 ? 1000 : 48000);
        w.u8(bad == 0 ? 0 : 2);
        w.u8(bad == 2 ? 7 : 1);
        w.boolean(false);
        w.varint(48000);
        w.u8(1);
        w.u8(1);
        AudioStartMsg b;
        CHECK(!b.decode(w.data()));
    }

    AudioStartedMsg st;
    st.playback = true;
    st.mic = true;
    st.mic_node = "broremote: laptop mic";
    st.message = "";
    msg = st.encode();
    AudioStartedMsg st2;
    CHECK(st2.decode(std::string_view(msg).substr(wire::kHeaderBytes)));
    CHECK_EQ(st2.mic_node, st.mic_node);
    CHECK(st2.playback && st2.mic);

    AudioDataMsg ad;
    ad.type = MsgType::AudioDown;
    ad.seq = 7;
    ad.capture_us = 123456789;
    ad.pcm = std::string(960, 'a');
    msg = ad.encode();
    AudioDataMsg ad2;
    CHECK(ad2.decode(std::string_view(msg).substr(wire::kHeaderBytes), MsgType::AudioDown));
    CHECK_EQ(ad2.seq, uint64_t(7));
    CHECK_EQ(ad2.capture_us, uint64_t(123456789));
    CHECK_EQ(ad2.pcm.size(), size_t(960));

    AudioStatsMsg sm;
    sm.mic_valid = true;
    sm.mic_capture_us = 5;
    sm.mic_out_us = 9;
    sm.mic_buffer_us = 20000;
    msg = sm.encode();
    AudioStatsMsg sm2;
    CHECK(sm2.decode(std::string_view(msg).substr(wire::kHeaderBytes)));
    CHECK(sm2.mic_valid && sm2.mic_out_us == 9 && sm2.mic_buffer_us == 20000);
    CHECK(sm2.status.empty());
    // A 1.4 body (no status at the end) still decodes, with no status.
    CHECK(sm2.decode(std::string_view(msg).substr(wire::kHeaderBytes, msg.size() - wire::kHeaderBytes - 1)));
    CHECK(sm2.mic_out_us == 9 && sm2.status.empty());
    // 1.5: the host's audio status.
    sm.status = "the PipeWire daemon went away; reconnecting";
    msg = sm.encode();
    CHECK(sm2.decode(std::string_view(msg).substr(wire::kHeaderBytes)));
    CHECK_EQ(sm2.status, sm.status);

    // PCM both formats: s16 within a quantum, f32 exact; a partial frame is refused.
    const float src[6] = {0.0f, 0.5f, -0.5f, 0.999f, -1.0f, 0.25f};
    for (au::SampleFormat sf : {au::SampleFormat::S16, au::SampleFormat::F32}) {
        const au::Format f{48000, 2, sf};
        std::string bytes;
        au::encode_pcm(src, 3, f, bytes);
        CHECK_EQ(bytes.size(), size_t(3 * f.frame_bytes()));
        std::vector<float> back;
        CHECK(au::decode_pcm(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), f, back));
        CHECK_EQ(back.size(), size_t(6));
        for (int i = 0; i < 6; ++i) CHECK(std::abs(back[size_t(i)] - src[i]) <= (sf == au::SampleFormat::F32 ? 0.0f : 1.0f / 16000));
        CHECK(!au::decode_pcm(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size() - 1, f, back));
    }
}

void test_analysis() {
    check::phase("analysis");
    for (double hz : {440.0, 1000.0, 3000.0}) {
        au::Tone t(hz, 0.5, 48000, 2);
        std::vector<float> f(48000 * 2);
        t.fill(f.data(), 48000);
        const au::ToneAnalysis a = au::analyze(f.data(), 24000, 2, 48000);
        std::printf("   %.0f Hz -> %.3f Hz, %.2f dBFS\n", hz, a.frequency_hz, a.rms_dbfs);
        CHECK(std::abs(a.frequency_hz - hz) < 0.5);
        CHECK(std::abs(a.rms_dbfs - db(0.5)) < 0.05);
    }
}

void test_jitter_buffer() {
    check::phase("jitter buffer");
    au::JitterBuffer jb(1, 48000, 20, 60);  // 960 target, 2880 max
    std::vector<float> in(480, 0.5f), out(480);
    // Primes: silence until 20 ms are queued.
    jb.push(in.data(), 480, 1000);
    jb.pull(out.data(), 480, 5000);
    CHECK_EQ(out[0], 0.0f);
    jb.push(in.data(), 480, 11000);
    jb.pull(out.data(), 480, 6000);
    CHECK_EQ(out[0], 0.5f);
    auto st = jb.stats();
    CHECK(st.valid);
    CHECK_EQ(st.stamp_us, int64_t(1000));  // the first frame played was the first pushed
    CHECK_EQ(st.out_us, int64_t(6000));
    // The next pull plays the second block: stamp 11000.
    jb.pull(out.data(), 240, 7000);
    CHECK_EQ(jb.stats().stamp_us, int64_t(11000));
    // Runs dry: an underrun, then silence until primed again.
    jb.pull(out.data(), 480, 8000);
    CHECK_EQ(jb.stats().underruns, uint64_t(1));
    jb.push(in.data(), 480, 21000);
    jb.pull(out.data(), 480, 9000);
    CHECK_EQ(out[0], 0.0f);
    // Over the bound: dropped down to the target.
    for (int i = 0; i < 8; ++i) jb.push(in.data(), 480, 30000 + i * 10000);
    jb.pull(out.data(), 480, 10000);
    st = jb.stats();
    CHECK(st.dropped > 0);
    CHECK_EQ(st.depth_frames, uint32_t(960 - 480));
}

struct Loopback {
    std::unique_ptr<Server> server;
    std::unique_ptr<Client> client;
    std::shared_ptr<au::PacedBackend> host_be, view_be;
    Recorder host_mic;   // what the host's virtual mic node played
    Recorder speakers;   // what the viewer's speakers played
    OnsetTone host_tone{1000.0, 0.25};
    OnsetTone mic_tone{440.0, 0.5};
};

bool open_loopback(Loopback& lb, const std::string& name, bool audio_enabled = true) {
    lb.host_be = std::make_shared<au::PacedBackend>();
    lb.host_be->period_frames = 240;  // 5 ms, as a PipeWire graph at node.latency 240/48000
    lb.host_be->generate = [&lb](au::Source src, float* f, uint32_t n, uint32_t ch, uint32_t rate) {
        if (src == au::Source::Monitor) lb.host_tone.fill(f, n, ch, rate);
        else std::fill(f, f + size_t(n) * ch, 0.0f);
    };
    lb.host_be->played = [&lb](au::Sink sink, const float* f, uint32_t n, uint32_t ch, uint32_t rate, int64_t t) {
        if (sink == au::Sink::VirtualMic) lb.host_mic.add(f, n, ch, rate, t);
    };
    ServerConfig cfg;
    cfg.socket_name = name;
    cfg.codecs = {Codec::Raw};
    cfg.name = "audio-test";
    cfg.audio.enabled = audio_enabled;
    cfg.audio.backend = lb.host_be;
    std::string err;
    lb.server = Server::create(cfg, &err);
    if (!lb.server) {
        std::printf("   Server::create: %s\n", err.c_str());
        return false;
    }
    lb.client = Client::connect(dial(name), ClientHandlers{}, &err);
    if (!lb.client) std::printf("   Client::connect: %s\n", err.c_str());
    return lb.client && lb.client->welcome().grant.has_value();
}

AudioSessionOptions view_options(Loopback& lb) {
    lb.view_be = std::make_shared<au::PacedBackend>();
    lb.view_be->played = [&lb](au::Sink sink, const float* f, uint32_t n, uint32_t ch, uint32_t rate, int64_t t) {
        if (sink == au::Sink::Speakers) lb.speakers.add(f, n, ch, rate, t);
    };
    AudioSessionOptions o;
    o.lane.source = "test viewer";
    o.backend = lb.view_be;
    o.period_ms = 10;  // as WASAPI shared mode
    o.mic_generator = [&lb](float* f, uint32_t n) { lb.mic_tone.fill(f, n, 1, 48000); };
    return o;
}

void test_tone_both_ways() {
    check::phase("a tone each way");
    Loopback lb;
    const std::string name = unique_name("tone");
    CHECK(open_loopback(lb, name));
    if (!lb.client) return;
    // Both tones start 300 ms from now: everything is running by then, so
    // the onset times the whole path, not the start-up.
    const int64_t on = au::now_us() + 300000;
    lb.host_tone.on_us = on;
    lb.mic_tone.on_us = on;
    std::string err;
    auto session = AudioSession::start(dial(name), *lb.client->welcome().grant, view_options(lb), &err);
    CHECK(session != nullptr);
    if (!session) {
        std::printf("   AudioSession::start: %s\n", err.c_str());
        return;
    }
    settle(1500);

    const AudioSession::Stats st = session->stats();
    CHECK(st.connected);
    CHECK(st.mic);
    CHECK(st.playback);
    CHECK_EQ(st.mic_node, std::string("broremote: test viewer mic"));
    const auto viewers = lb.server->audio_viewers();
    CHECK_EQ(viewers.size(), size_t(1));
    if (!viewers.empty()) {
        CHECK_EQ(viewers[0].source, std::string("test viewer"));
        CHECK(viewers[0].mic && viewers[0].playback);
    }

    // Frequency and level, each way (s16 on the wire; 0.5 dB slack).
    const au::ToneAnalysis up = lb.host_mic.tail(500);
    const au::ToneAnalysis down = lb.speakers.tail(500);
    std::printf("   mic -> host node: %.2f Hz at %.2f dBFS (want 440 Hz, %.2f)\n", up.frequency_hz, up.rms_dbfs, db(0.5));
    std::printf("   host -> speakers: %.2f Hz at %.2f dBFS (want 1000 Hz, %.2f)\n", down.frequency_hz, down.rms_dbfs,
                db(0.25));
    CHECK(std::abs(up.frequency_hz - 440.0) < 1.0);
    CHECK(std::abs(down.frequency_hz - 1000.0) < 1.0);
    CHECK(std::abs(up.rms_dbfs - db(0.5)) < 0.5);
    CHECK(std::abs(down.rms_dbfs - db(0.25)) < 0.5);

    // Latency: the tone's onset, captured -> played, on the shared clock.
    const double up_ms = double(lb.host_mic.onset_out_us - lb.mic_tone.onset_capture_us.load()) / 1000.0;
    const double down_ms = double(lb.speakers.onset_out_us - lb.host_tone.onset_capture_us.load()) / 1000.0;
    std::printf("   onset latency: mic -> host node %.1f ms, host -> speakers %.1f ms\n", up_ms, down_ms);
    std::printf("   reported:      mic -> host node %.1f ms, host -> speakers %.1f ms (rtt %.2f ms)\n",
                st.mic_latency_ms, st.playback_latency_ms, st.rtt_ms);
    std::printf("   buffers: host mic %.1f ms (%llu underruns), playback %.1f ms (%llu underruns)\n",
                st.host_mic_buffer_ms, static_cast<unsigned long long>(st.host_mic_underruns), st.playback_buffer_ms,
                static_cast<unsigned long long>(st.playback_underruns));
    CHECK(lb.host_mic.onset_out_us > 0 && lb.speakers.onset_out_us > 0);
    // A 20 ms jitter target plus a device period or two each end: well under 80.
    CHECK(up_ms > 0 && up_ms < 80);
    CHECK(down_ms > 0 && down_ms < 80);
    // The reported latencies (stamps carried per frame, the clock offset from
    // Ping / Pong) agree with the ones measured from the onset.
    CHECK(st.mic_latency_ms >= 0 && std::abs(st.mic_latency_ms - up_ms) < 5);
    CHECK(st.playback_latency_ms >= 0 && std::abs(st.playback_latency_ms - down_ms) < 5);

    const Server::Stats ss = lb.server->stats();
    CHECK_EQ(ss.audio_lanes, uint64_t(1));
    CHECK(ss.audio_up > 50);
    CHECK(ss.audio_down > 50);

    // Mute the mic: the host's node goes quiet; unmute: the tone comes back.
    check::phase("mute");
    session->set_mic_muted(true);
    WAIT(!lb.server->audio_viewers().empty() && lb.server->audio_viewers()[0].mic_muted, 2000);
    settle(300);
    CHECK(lb.host_mic.tail(150).rms_dbfs < -60);
    session->set_mic_muted(false);
    settle(400);
    // The tone is back (by level: a paced timer's hiccup can leave a short
    // gap in a 150 ms window, which upsets a zero-crossing count, not this).
    CHECK(lb.host_mic.tail(150).rms_dbfs > db(0.5) - 3);
    session->set_playback_muted(true);
    settle(300);
    CHECK(lb.speakers.tail(150).rms_dbfs < -60);

    // The audio lane ends: the control connection and input are untouched,
    // and the host drops the lane's devices.
    check::phase("audio lane ends");
    session.reset();
    WAIT(lb.server->audio_viewers().empty(), 2000);
    CHECK(lb.client->connected());
    CHECK_EQ(lb.server->client_count(), size_t(1));
    InputEvent e;
    e.kind = InputKind::Key;
    e.code = 30;
    e.pressed = true;
    lb.client->send_input(e);
    std::vector<InputEvent> got;
    WAIT((lb.server->drain_input(got), !got.empty()), 2000);

    // A second audio lane in the same session is refused (each lane once).
    auto again = AudioSession::start(dial(name), *lb.client->welcome().grant, view_options(lb), &err);
    CHECK(again == nullptr);
    CHECK(err.find("join refused") != std::string::npos);
    CHECK(lb.client->connected());
}

void test_control_ends_audio() {
    check::phase("the control connection ends the audio lane");
    Loopback lb;
    const std::string name = unique_name("ctl");
    CHECK(open_loopback(lb, name));
    if (!lb.client) return;
    std::string err;
    auto session = AudioSession::start(dial(name), *lb.client->welcome().grant, view_options(lb), &err);
    CHECK(session != nullptr);
    WAIT(lb.server->audio_viewers().size() == 1, 2000);
    lb.client.reset();
    WAIT(lb.server->audio_viewers().empty(), 3000);
    if (session) WAIT(!session->stats().connected, 3000);
}

void test_refusals() {
    check::phase("audio off on the host");
    {
        Loopback lb;
        const std::string name = unique_name("off");
        CHECK(open_loopback(lb, name, false));
        if (lb.client) {
            std::string err;
            auto session = AudioSession::start(dial(name), *lb.client->welcome().grant, view_options(lb), &err);
            CHECK(session != nullptr);
            if (session) {
                const auto st = session->stats();
                CHECK(!st.mic && !st.playback);
                CHECK(st.notes.find("audio is off") != std::string::npos);
            }
        }
    }
    check::phase("audio lane protocol errors");
    Loopback lb;
    const std::string name = unique_name("err");
    CHECK(open_loopback(lb, name));
    if (!lb.client) return;
    const brolink::lanes::Grant grant = *lb.client->welcome().grant;
    // Read one message from a raw stream.
    auto read_one = [](Stream& s, wire::MessageSplitter& sp, wire::MessageSplitter::Message& m) {
        char buf[4096];
        while (!sp.next(m)) {
            const size_t n = s.read(buf, sizeof buf);
            if (n == 0) return false;
            sp.feed(buf, n);
        }
        return true;
    };
    {
        // A wrong token.
        auto s = dial(name);
        JoinMsg j;
        j.join.session = grant.session;
        j.join.token = grant.token;
        j.join.token.bytes[0] ^= 1;
        j.join.lane = std::string(kAudioLane);
        s->write(j.encode());
        wire::MessageSplitter sp;
        wire::MessageSplitter::Message m;
        CHECK(read_one(*s, sp, m));
        CHECK_EQ(m.type, uint16_t(MsgType::Error));
    }
    {
        // AudioUp before AudioStart: closed with BadMessage.
        auto s = dial(name);
        JoinMsg j;
        j.join.session = grant.session;
        j.join.token = grant.token;
        j.join.lane = std::string(kAudioLane);
        s->write(j.encode());
        wire::MessageSplitter sp;
        wire::MessageSplitter::Message m;
        CHECK(read_one(*s, sp, m));
        CHECK_EQ(m.type, uint16_t(MsgType::Joined));
        s->write(AudioDataMsg::encode_message(MsgType::AudioUp, 1, 1, std::string(4, '\0')));
        CHECK(read_one(*s, sp, m));
        CHECK_EQ(m.type, uint16_t(MsgType::Error));
        ErrorMsg e;
        CHECK(e.decode(m.payload));
        CHECK(e.code == ErrorCode::BadMessage);
    }
    CHECK(lb.client->connected());
}

}  // namespace

int main() {
    check::watchdog(120);
    test_wire();
    test_analysis();
    test_jitter_buffer();
    test_tone_both_ways();
    test_control_ends_audio();
    test_refusals();
    return check::finish();
}
