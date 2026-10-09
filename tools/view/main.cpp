// broremote-view: watch and drive a broremote session.
//   broremote-view [--ssh HOST [--ssh-command CMD] | --socket NAME] [options]
// See usage() for the options.
#include "viewer_app.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace broremote;

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: broremote-view [--ssh HOST [--ssh-command CMD] | --socket NAME] [options]\n"
                 "  --ssh HOST          connect through `ssh -T HOST broremote proxy`\n"
                 "  --ssh-command CMD   run CMD on the host instead of `broremote proxy`\n"
                 "  --socket NAME       the server's socket name (local, or passed to the remote proxy)\n"
                 "  --ssh-program PATH  the ssh to run (default: $BROREMOTE_SSH, else Windows' own OpenSSH\n"
                 "                      where present, else ssh from PATH)\n"
                 "  --ssh-pty           run the proxy on a raw remote terminal (ssh -tt: OpenSSH turns\n"
                 "                      Nagle off); --ssh-no-pty is the default ssh -T pipe\n"
                 "  --no-input-lane     send input on the control connection, not on a second\n"
                 "                      connection (a second ssh with --ssh) of its own\n"
                 "  --no-audio-lane     no audio: neither the host's audio here nor this mic there\n"
                 "  --no-mic            hear the host, but send no mic\n"
                 "  --no-audio-playback send the mic, but do not play the host's audio\n"
                 "  --mic-device NAME   the mic whose name contains NAME (default: the communications\n"
                 "                      default; `broremote audio-devices` lists them)\n"
                 "  --speaker-device NAME  the speakers likewise (default: the default output)\n"
                 "  --mic-tone HZ       send a tone instead of the mic\n"
                 "  --mic-file WAV      send WAV (looped) instead of the mic\n"
                 "  --record-audio WAV  write what the speakers played (first channel) on exit\n"
                 "  --audio-buffer MS   the playback jitter buffer's target (default 20)\n"
                 "  --audio-stats       print the audio latencies and buffers every second\n"
                 "                      (Ctrl+Alt+M mutes the mic, Ctrl+Alt+A the host's audio)\n"
                 "  --any-codec         do not tell the server which codecs decode here\n"
                 "  --fullscreen        start fullscreen (Ctrl+Alt+Enter toggles)\n"
                 "  --size WxH          initial window size (default: fit the stream)\n"
                 "  --no-vsync          present as soon as a picture is decoded\n"
                 "  --frames N          exit after N pictures were shown\n"
                 "  --timeout S         with --frames: fail if they have not been shown in S seconds\n"
                 "  --dump-png FILE     write the last picture shown to FILE on exit\n"
                 "  --check-pattern     compare the last picture with broremote serve-test's pattern\n"
                 "  --hidden            no visible window\n"
                 "  --stats             print where each second's frames spent their time\n"
                 "  --latency-test N    against `broremote serve-test --latency`: send N key presses,\n"
                 "                      time each to the picture that answers it, report and exit\n"
                 "  --latency-motion    while probing, keep sending pointer motion too\n");
    return 2;
}

bool parse_uint(const char* s, uint32_t& out) {
    char* end = nullptr;
    const unsigned long v = std::strtoul(s, &end, 10);
    if (!end || *end || end == s || v > 0xFFFFFFFFul) return false;
    out = uint32_t(v);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    view::ViewerOptions o;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const bool has = i + 1 < argc;
        if (tools::parse_connect_arg(argc, argv, i, o.session.target)) continue;
        if (!std::strcmp(a, "--any-codec")) {
            o.session.negotiate = false;
        } else if (!std::strcmp(a, "--no-audio-lane")) {
            o.session.audio.enabled = false;
        } else if (!std::strcmp(a, "--no-mic")) {
            o.session.audio.mic = false;
        } else if (!std::strcmp(a, "--no-audio-playback")) {
            o.session.audio.playback = false;
        } else if (!std::strcmp(a, "--audio-stats")) {
            o.audio_stats = true;
        } else if (!std::strcmp(a, "--mic-tone") && has) {
            o.session.audio.mic_tone_hz = std::atof(argv[++i]);
            if (!(o.session.audio.mic_tone_hz > 0 && o.session.audio.mic_tone_hz < 20000)) return usage();
        } else if (!std::strcmp(a, "--mic-device") && has) {
            o.session.audio.mic_device = argv[++i];
        } else if (!std::strcmp(a, "--speaker-device") && has) {
            o.session.audio.speaker_device = argv[++i];
        } else if (!std::strcmp(a, "--mic-file") && has) {
            o.session.audio.mic_file = argv[++i];
        } else if (!std::strcmp(a, "--record-audio") && has) {
            o.record_audio = argv[++i];
            o.session.audio.record_seconds = 600;
        } else if (!std::strcmp(a, "--audio-buffer") && has) {
            if (!parse_uint(argv[++i], o.session.audio.jitter_ms) || o.session.audio.jitter_ms > 1000) return usage();
        } else if (!std::strcmp(a, "--fullscreen")) {
            o.fullscreen = true;
        } else if (!std::strcmp(a, "--no-vsync")) {
            o.vsync = false;
        } else if (!std::strcmp(a, "--latency-motion")) {
            o.probe_motion = true;
        } else if (!std::strcmp(a, "--stats")) {
            o.stats = true;
        } else if (!std::strcmp(a, "--latency-test") && has) {
            if (!parse_uint(argv[++i], o.latency_probes) || o.latency_probes == 0) return usage();
        } else if (!std::strcmp(a, "--hidden")) {
            o.hidden = true;
        } else if (!std::strcmp(a, "--check-pattern")) {
            o.check_pattern = true;
        } else if (!std::strcmp(a, "--frames") && has) {
            if (!parse_uint(argv[++i], o.frames) || o.frames == 0) return usage();
        } else if (!std::strcmp(a, "--timeout") && has) {
            uint32_t s = 0;
            if (!parse_uint(argv[++i], s)) return usage();
            o.timeout_s = s;
        } else if (!std::strcmp(a, "--dump-png") && has) {
            o.dump_png = argv[++i];
        } else if (!std::strcmp(a, "--size") && has) {
            unsigned w = 0, h = 0;
            if (std::sscanf(argv[++i], "%ux%u", &w, &h) != 2 || !w || !h) return usage();
            o.width = int(w);
            o.height = int(h);
        } else if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) {
            usage();
            return 0;
        } else {
            return usage();
        }
    }
    if (o.session.target.remote() && o.session.target.ssh_host.empty()) return usage();
    return view::run_viewer(o);
}
