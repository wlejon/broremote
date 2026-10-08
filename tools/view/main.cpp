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
                 "  --any-codec         do not tell the server which codecs decode here\n"
                 "  --fullscreen        start fullscreen (Ctrl+Alt+Enter toggles)\n"
                 "  --size WxH          initial window size (default: fit the stream)\n"
                 "  --no-vsync          present as soon as a picture is decoded\n"
                 "  --frames N          exit after N pictures were shown\n"
                 "  --timeout S         with --frames: fail if they have not been shown in S seconds\n"
                 "  --dump-png FILE     write the last picture shown to FILE on exit\n"
                 "  --check-pattern     compare the last picture with broremote serve-test's pattern\n"
                 "  --hidden            no visible window\n");
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
        } else if (!std::strcmp(a, "--fullscreen")) {
            o.fullscreen = true;
        } else if (!std::strcmp(a, "--no-vsync")) {
            o.vsync = false;
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
