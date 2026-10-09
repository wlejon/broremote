// broremote probe: a viewer with no window, for scripted end-to-end checks.
// It runs the library's ViewerSession (connect, decode, ping, audio) and
// "shows" a picture by taking it the moment it is published, so its
// "presented" is the decode thread handing the picture over, with no
// display after it. See usage() in broremote.cpp for the options.
#include "probe.h"

#include "broremote/audio.h"
#include "broremote/viewer.h"
#include "connect.h"
#include "picture.h"
#include "png.h"
#include "test_pattern.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace broremote::tools {

namespace {

std::atomic<bool> g_quit{false};
void on_signal(int) { g_quit = true; }

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[size_t(p * double(v.size() - 1))];
}

double mean(const std::vector<double>& v) {
    double s = 0;
    for (double x : v) s += x;
    return v.empty() ? 0 : s / double(v.size());
}

bool parse_uint(const char* s, uint32_t& out) {
    char* end = nullptr;
    const unsigned long v = std::strtoul(s, &end, 10);
    if (!end || *end || end == s || v > 0xFFFFFFFFul) return false;
    out = uint32_t(v);
    return true;
}

struct ProbeOptions {
    ViewerOptions session;
    uint32_t frames = 0;          // exit after this many pictures (0: until the other limits)
    double timeout_s = 0;         // with frames: fail (exit 1) after this long
    double seconds = 0;           // stop (exit 0) after this long
    std::string dump_png;         // the last picture, on exit
    bool check_pattern = false;   // the last picture against serve-test's pattern
    bool stats = false;           // a timing line every second
    bool audio_stats = false;     // an audio line every second
    std::string record_audio;     // what the speakers played, on exit
    uint32_t latency_probes = 0;  // against serve-test --latency
    bool probe_motion = false;    // keep the pointer moving while probing
};

std::string timing_text(const LatencyWindow& w) {
    if (!w.frames) return w.rtt >= 0 ? "rtt " + std::to_string(w.rtt) + " ms, no frame timing" : "no timing yet";
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "rtt %.2f (mean %.2f max %.2f) | server: queue %.2f encode %.2f wait %.2f | net %.2f | dwait %.2f "
                  "decode %.2f | take %.2f | age %.1f (max %.1f) ms, %.1f kB/frame (max %.1f kB, net %.2f ms)",
                  w.rtt, w.rtt_mean, w.rtt_max, w.queue, w.encode, w.wait, w.net, w.dwait, w.decode, w.present, w.age,
                  w.max_age, w.kbytes, w.max_kbytes, w.max_net);
    return buf;
}

class ProbeRun {
public:
    explicit ProbeRun(ProbeOptions o) : opt_(std::move(o)) {
        if (!opt_.session.log) {
            opt_.session.log = [](const std::string& line) { std::fprintf(stderr, "broremote probe: %s\n", line.c_str()); };
        }
    }

    int run() {
        session_ = std::make_unique<ViewerSession>([this] {
            std::lock_guard<std::mutex> lk(m_);
            woken_ = true;
            cv_.notify_one();
        });
        start_ = window_start_ = Clock::now();
        session_->start(opt_.session);
        // Probing wakes often to send each probe on time; otherwise a
        // published picture or a status change wakes it.
        const auto tick = std::chrono::milliseconds(opt_.latency_probes ? 2 : 100);
        while (step()) {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait_for(lk, tick, [&] { return woken_; });
            woken_ = false;
        }
        return finish();
    }

private:
    bool step() {
        const ViewerStatus st = session_->status();
        if (st.state != shown_.state || st.config.stream_id != shown_.config.stream_id ||
            st.decoder != shown_.decoder) {
            if (st.state == ViewerState::Connected && st.have_config) {
                std::fprintf(stderr, "broremote probe: %s: %s %ux%u, %s (server %s, protocol 1.%u)\n",
                             opt_.session.target.describe().c_str(), codec_name(st.config.codec), st.config.width,
                             st.config.height, st.decoder.c_str(), st.server.c_str(), unsigned(st.server_minor));
            }
        }
        shown_ = st;
        if (session_->take_frame(frame_, info_)) {
            have_frame_ = true;
            ++taken_;
            const auto now = Clock::now();
            taken_ms_.push_back(std::chrono::duration<double, std::milli>(now - info_.received).count());
            session_->note_presented(info_.frame_id, now);
        }
        const auto now = Clock::now();
        if (now - window_start_ >= std::chrono::seconds(1)) per_second(now);
        run_probes(now);

        if (g_quit) {
            quit_ = true;
            return false;
        }
        if (st.state == ViewerState::Closed) return false;
        if (opt_.latency_probes &&
            session_->latency().probes().size() + session_->latency().probes_lost() >= opt_.latency_probes) {
            return false;
        }
        if (opt_.frames && taken_ >= opt_.frames) return false;
        const double elapsed = std::chrono::duration<double>(now - start_).count();
        if (opt_.seconds > 0 && elapsed >= opt_.seconds) return false;
        if (opt_.timeout_s > 0 && elapsed > opt_.timeout_s) {
            timed_out_ = true;
            return false;
        }
        return true;
    }

    void per_second(Clock::time_point now) {
        const double since = std::chrono::duration<double>(now - window_start_).count();
        const LatencyWindow w = session_->latency().take_window();
        if (w.frames) windows_.push_back(w);
        if (opt_.stats && shown_.state == ViewerState::Connected) {
            std::fprintf(stderr, "broremote probe: %.1f fps, %s\n", double(taken_ - window_taken_) / since,
                         timing_text(w).c_str());
        }
        AudioSession::Stats a;
        if (session_->audio_stats(a) && a.connected) {
            if (a.host_status != host_status_) {
                host_status_ = a.host_status;
                std::fprintf(stderr, "broremote probe: host audio: %s\n",
                             host_status_.empty() ? "ok" : host_status_.c_str());
            }
            if (a.mic && a.mic_latency_ms >= 0) audio_up_ms_.push_back(a.mic_latency_ms);
            if (a.playback && a.playback_latency_ms >= 0) audio_down_ms_.push_back(a.playback_latency_ms);
            if (opt_.audio_stats) {
                // A negative latency is "not measured yet".
                auto ms = [](double v) {
                    char buf[32];
                    if (v < 0) return std::string("(not playing yet)");
                    std::snprintf(buf, sizeof buf, "%.1f ms", v);
                    return std::string(buf);
                };
                std::fprintf(stderr,
                             "broremote probe: audio: mic -> host %s (host buffer %.1f ms, %llu underruns), "
                             "host -> here %s (buffer %.1f ms, %llu underruns, %llu dropped), rtt %.2f ms, "
                             "%llu up / %llu down packets\n",
                             ms(a.mic_latency_ms).c_str(), a.host_mic_buffer_ms,
                             static_cast<unsigned long long>(a.host_mic_underruns), ms(a.playback_latency_ms).c_str(),
                             a.playback_buffer_ms, static_cast<unsigned long long>(a.playback_underruns),
                             static_cast<unsigned long long>(a.playback_dropped), a.rtt_ms,
                             static_cast<unsigned long long>(a.mic_packets),
                             static_cast<unsigned long long>(a.playback_packets));
            }
        }
        window_start_ = now;
        window_taken_ = taken_;
    }

    // One probe at a time, a quarter second or so apart (jittered so the
    // probes do not lock to the frame rate), once pictures are flowing.
    void run_probes(Clock::time_point now) {
        if (!opt_.latency_probes || probes_sent_ >= opt_.latency_probes || taken_ < 2) return;
        if (opt_.probe_motion) {
            // A pointer in motion: a small message every step, as a moving mouse sends.
            const float t = float(std::chrono::duration<double>(now - start_).count());
            session_->send_input(InputEvent::motion(100.0f + 50.0f * std::sin(t * 3.0f), 100.0f));
        }
        if (now < next_probe_ || session_->latency().probe_open(now)) return;
        if (session_->probe()) {
            ++probes_sent_;
            next_probe_ = now + std::chrono::milliseconds(200 + int(probes_sent_ * 37 % 67));
        } else {
            next_probe_ = now + std::chrono::milliseconds(100);
        }
    }

    void audio_report() {
        AudioSession::Stats a;
        if (!session_->audio_stats(a)) return;
        auto range = [](const std::vector<double>& v) {
            if (v.empty()) return std::string("not measured");
            char buf[96];
            std::snprintf(buf, sizeof buf, "mean %.1f, min %.1f, max %.1f ms (%zu samples)", mean(v),
                          *std::min_element(v.begin(), v.end()), *std::max_element(v.begin(), v.end()), v.size());
            return std::string(buf);
        };
        std::fprintf(stderr,
                     "broremote probe: audio: mic %s (%s%s), host audio %s (%s)\n"
                     "  one-way latency: mic -> host node %s\n"
                     "                   host -> speakers %s\n"
                     "  %llu mic packets sent (%llu frames dropped before sending); %llu host packets received\n"
                     "  host mic buffer: %llu underruns, %llu frames dropped; speakers: %llu underruns, %llu dropped\n",
                     a.mic ? "on" : "off", a.mic_device.c_str(), a.echo_cancel ? ", echo cancelled" : "",
                     a.playback ? "on" : "off", a.speaker_device.c_str(), range(audio_up_ms_).c_str(),
                     range(audio_down_ms_).c_str(), static_cast<unsigned long long>(a.mic_packets),
                     static_cast<unsigned long long>(a.mic_overflow),
                     static_cast<unsigned long long>(a.playback_packets),
                     static_cast<unsigned long long>(a.host_mic_underruns),
                     static_cast<unsigned long long>(a.host_mic_dropped),
                     static_cast<unsigned long long>(a.playback_underruns),
                     static_cast<unsigned long long>(a.playback_dropped));
        if (!a.notes.empty()) std::fprintf(stderr, "  %s\n", a.notes.c_str());
        std::vector<float> rec;
        uint32_t rate = 0, ch = 0;
        session_->recorded_audio(rec, rate, ch);
        if (!rate || rec.empty()) return;
        // The last second the speakers played: what it was.
        const size_t n = std::min<size_t>(rec.size(), rate);
        const audio::ToneAnalysis t = audio::analyze(rec.data() + (rec.size() - n), n, 1, rate);
        std::fprintf(stderr, "  host audio heard: %.2f s; the last second %.1f dBFS, peak %.3f, dominant %.1f Hz\n",
                     double(rec.size()) / rate, t.rms_dbfs, t.peak, t.frequency_hz);
        if (opt_.record_audio.empty()) return;
        audio::WavData w;
        w.rate = rate;
        w.channels = 1;
        w.frames = std::move(rec);
        std::string err;
        if (audio::write_wav(opt_.record_audio, w, &err)) std::fprintf(stderr, "  wrote %s\n", opt_.record_audio.c_str());
        else std::fprintf(stderr, "  --record-audio: %s\n", err.c_str());
    }

    void timing_report() {
        if (windows_.empty()) return;
        // The per-second means, averaged (weighted by frames).
        LatencyWindow all;
        double n = 0;
        for (const LatencyWindow& w : windows_) {
            const double k = double(w.frames);
            n += k;
            all.queue += w.queue * k;
            all.encode += w.encode * k;
            all.wait += w.wait * k;
            all.net += w.net * k;
            all.dwait += w.dwait * k;
            all.decode += w.decode * k;
            all.present += w.present * k;
            all.age += w.age * k;
            all.kbytes += w.kbytes * k;
            all.max_age = std::max(all.max_age, w.max_age);
            all.max_kbytes = std::max(all.max_kbytes, w.max_kbytes);
            all.max_net = std::max(all.max_net, w.max_net);
            all.rtt_mean += w.rtt_mean * double(w.pongs);
            all.pongs += w.pongs;
            all.rtt_max = std::max(all.rtt_max, w.rtt_max);
        }
        if (all.pongs) all.rtt_mean /= double(all.pongs);
        for (double* v : {&all.queue, &all.encode, &all.wait, &all.net, &all.dwait, &all.decode, &all.present,
                          &all.age, &all.kbytes})
            *v /= n;
        all.frames = uint64_t(n);
        all.rtt = session_->latency().rtt_ms();
        std::fprintf(stderr, "  timing over %llu frames: %s\n", static_cast<unsigned long long>(all.frames),
                     timing_text(all).c_str());
    }

    // False when no probe was answered.
    bool latency_report() {
        const std::vector<broremote::Probe> ps = session_->latency().probes();
        const uint64_t lost = session_->latency().probes_lost();
        if (ps.empty()) {
            std::fprintf(stderr, "broremote probe: no latency probe was answered (%llu lost); is the server "
                                 "`broremote serve-test --latency`?\n",
                         static_cast<unsigned long long>(lost));
            return false;
        }
        std::vector<double> dec, taken;
        broremote::Probe m;
        for (const broremote::Probe& p : ps) {
            dec.push_back(p.total_decoded);
            taken.push_back(p.total_presented);
            m.uplink += p.uplink;
            m.queue += p.queue;
            m.encode += p.encode;
            m.wait += p.wait;
            m.net += p.net;
            m.dwait += p.dwait;
            m.decode += p.decode;
            m.present += p.present;
            m.rtt += p.rtt;
        }
        const double k = double(ps.size());
        std::fprintf(stderr,
                     "broremote probe: %zu latency probes (%llu lost)\n"
                     "  input -> decoded: mean %.2f, p50 %.2f, p90 %.2f, max %.2f ms\n"
                     "  input -> taken:   mean %.2f, p50 %.2f, p90 %.2f, max %.2f ms (no display after it)\n"
                     "  mean parts: uplink+react %.2f | queue %.2f encode %.2f wait %.2f | net %.2f | "
                     "dwait %.2f decode %.2f | take %.2f ms (rtt %.2f)\n",
                     ps.size(), static_cast<unsigned long long>(lost), mean(dec), percentile(dec, 0.5),
                     percentile(dec, 0.9), percentile(dec, 1.0), mean(taken), percentile(taken, 0.5),
                     percentile(taken, 0.9), percentile(taken, 1.0), m.uplink / k, m.queue / k, m.encode / k,
                     m.wait / k, m.net / k, m.dwait / k, m.decode / k, m.present / k, m.rtt / k);
        return true;
    }

    // --dump-png and --check-pattern on the last picture. False on a failure.
    bool picture_checks() {
        if (opt_.dump_png.empty() && !opt_.check_pattern) return true;
        if (!have_frame_) {
            std::fprintf(stderr, "broremote probe: no picture arrived\n");
            return false;
        }
        bool ok = true;
        const std::vector<uint8_t> rgba = to_rgba(frame_);
        if (!opt_.dump_png.empty()) {
            if (write_png(opt_.dump_png, rgba.data(), frame_.width, frame_.height)) {
                std::fprintf(stderr, "broremote probe: wrote frame %llu (%ux%u) to %s\n",
                             static_cast<unsigned long long>(info_.frame_id), frame_.width, frame_.height,
                             opt_.dump_png.c_str());
            } else {
                std::fprintf(stderr, "broremote probe: cannot write %s\n", opt_.dump_png.c_str());
                ok = false;
            }
        }
        if (opt_.check_pattern) {
            const int64_t n = read_test_pattern_counter(rgba.data(), frame_.width, frame_.height, frame_.width * 4);
            std::vector<uint8_t> source(rgba.size());
            draw_test_pattern(source.data(), frame_.width, frame_.height, uint64_t(n < 0 ? 0 : n));
            const double rgb = psnr_rgba(rgba.data(), source.data(), frame_.width, frame_.height);
            const double luma = frame_.format == PixelFormat::NV12 ? psnr_luma(frame_, source.data()) : rgb;
            const bool good = n >= 0 && luma >= 35.0;
            std::fprintf(stderr, "broremote probe: pattern frame %lld: luma PSNR %.1f dB, RGB PSNR %.1f dB: %s\n",
                         static_cast<long long>(n), luma, rgb, good ? "match" : "MISMATCH");
            ok = ok && good;
        }
        return ok;
    }

    int finish() {
        int rc = 0;
        const ViewerStatus st = session_->status();
        const ViewerStats stats = session_->stats();
        if (st.state == ViewerState::Closed && !(opt_.frames && taken_ >= opt_.frames)) {
            std::fprintf(stderr, "broremote probe: %s\n", st.message.c_str());
            if (st.failed) rc = 1;
        }
        if (timed_out_) {
            std::fprintf(stderr, "broremote probe: gave up after %.0f s with %llu of %u pictures\n", opt_.timeout_s,
                         static_cast<unsigned long long>(taken_), opt_.frames);
            rc = 1;
        }
        audio_report();
        if (stats.decoded > 0) {
            const double span = std::chrono::duration<double>(stats.last_decoded - stats.first_decoded).count();
            const double fps = span > 0 ? double(stats.decoded - 1) / span : 0;
            const double mbps = span > 0 ? double(stats.bytes) * 8 / span / 1e6 : 0;
            std::fprintf(stderr,
                         "broremote probe: %s %ux%u, %s\n"
                         "  %llu pictures decoded (%llu failed, %llu keyframe requests), %llu taken, %.1f fps "
                         "decoded, %.1f Mbit/s\n"
                         "  decode: mean %.2f, p50 %.2f, p99 %.2f ms; packet received to taken: mean %.1f, p50 "
                         "%.1f, p99 %.1f ms\n",
                         codec_name(st.config.codec), st.config.width, st.config.height, st.decoder.c_str(),
                         static_cast<unsigned long long>(stats.decoded), static_cast<unsigned long long>(stats.failed),
                         static_cast<unsigned long long>(stats.keyframe_requests),
                         static_cast<unsigned long long>(taken_), fps, mbps, mean(stats.decode_ms),
                         percentile(stats.decode_ms, 0.5), percentile(stats.decode_ms, 0.99), mean(taken_ms_),
                         percentile(taken_ms_, 0.5), percentile(taken_ms_, 0.99));
        }
        timing_report();
        if (opt_.latency_probes && !latency_report()) rc = 1;
        if (opt_.frames && taken_ < opt_.frames && !timed_out_ && !quit_) rc = 1;
        if (!picture_checks()) rc = 1;
        return rc;
    }

    ProbeOptions opt_;
    // Before session_, so its threads' wake calls find them until it is gone.
    std::mutex m_;
    std::condition_variable cv_;
    bool woken_ = false;
    std::unique_ptr<ViewerSession> session_;

    ViewerStatus shown_;
    DecodedFrame frame_;
    ViewerFrameInfo info_;
    bool have_frame_ = false;
    bool quit_ = false, timed_out_ = false;
    uint64_t taken_ = 0, window_taken_ = 0;
    std::vector<double> taken_ms_;  // packet received -> taken
    Clock::time_point start_, window_start_, next_probe_{};
    uint64_t probes_sent_ = 0;
    std::vector<LatencyWindow> windows_;
    std::vector<double> audio_up_ms_, audio_down_ms_;
    std::string host_status_;  // the host's audio status last printed
};

}  // namespace

int cmd_probe(int argc, char** argv, int (*usage)()) {
    ProbeOptions o;
    o.session.client_name = "broremote probe";
    // No audio unless asked: a probe should not take this machine's mic and
    // speakers.
    o.session.audio.enabled = false;
    for (int i = 0; i < argc; ++i) {
        const char* a = argv[i];
        const bool has = i + 1 < argc;
        if (parse_connect_arg(argc, argv, i, o.session.target)) continue;
        if (!std::strcmp(a, "--any-codec")) {
            o.session.negotiate = false;
        } else if (!std::strcmp(a, "--frames") && has) {
            if (!parse_uint(argv[++i], o.frames) || o.frames == 0) return usage();
        } else if (!std::strcmp(a, "--timeout") && has) {
            uint32_t s = 0;
            if (!parse_uint(argv[++i], s)) return usage();
            o.timeout_s = s;
        } else if (!std::strcmp(a, "--seconds") && has) {
            uint32_t s = 0;
            if (!parse_uint(argv[++i], s) || s == 0) return usage();
            o.seconds = s;
        } else if (!std::strcmp(a, "--dump-png") && has) {
            o.dump_png = argv[++i];
        } else if (!std::strcmp(a, "--check-pattern")) {
            o.check_pattern = true;
        } else if (!std::strcmp(a, "--stats")) {
            o.stats = true;
        } else if (!std::strcmp(a, "--latency-test") && has) {
            if (!parse_uint(argv[++i], o.latency_probes) || o.latency_probes == 0) return usage();
        } else if (!std::strcmp(a, "--latency-motion")) {
            o.probe_motion = true;
        } else if (!std::strcmp(a, "--audio")) {
            o.session.audio.enabled = true;
        } else if (!std::strcmp(a, "--no-mic")) {
            o.session.audio.mic = false;
        } else if (!std::strcmp(a, "--no-audio-playback")) {
            o.session.audio.playback = false;
        } else if (!std::strcmp(a, "--audio-stats")) {
            o.audio_stats = true;
        } else if (!std::strcmp(a, "--mic-tone") && has) {
            o.session.audio.mic_tone_hz = std::atof(argv[++i]);
            if (!(o.session.audio.mic_tone_hz > 0 && o.session.audio.mic_tone_hz < 20000)) return usage();
        } else if (!std::strcmp(a, "--mic-file") && has) {
            o.session.audio.mic_file = argv[++i];
        } else if (!std::strcmp(a, "--mic-device") && has) {
            o.session.audio.mic_device = argv[++i];
        } else if (!std::strcmp(a, "--speaker-device") && has) {
            o.session.audio.speaker_device = argv[++i];
        } else if (!std::strcmp(a, "--record-audio") && has) {
            o.record_audio = argv[++i];
            o.session.audio.record_seconds = 600;
        } else if (!std::strcmp(a, "--audio-buffer") && has) {
            if (!parse_uint(argv[++i], o.session.audio.jitter_ms) || o.session.audio.jitter_ms > 1000) return usage();
        } else {
            return usage();
        }
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    return ProbeRun(std::move(o)).run();
}

}  // namespace broremote::tools
