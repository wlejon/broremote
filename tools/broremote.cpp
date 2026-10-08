// broremote: the command-line tool.
//   broremote proxy [--socket NAME]
//       Relay stdin/stdout to the local server's socket (what a remote viewer
//       runs as `ssh host broremote proxy`).
//   broremote serve-test [--socket NAME] [--size WxH] [--codec C] [--fps N] [--seconds N]
//       A server fed a moving test pattern (CPU frames), so a viewer can be
//       tested with no bro. Input events received are printed to stderr.
//   broremote codecs
//       The codecs this build can encode and decode here.
#include "broremote/protocol.h"
#include "broremote/server.h"
#include "broremote/stream.h"
#include "test_pattern.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace broremote;

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

int usage() {
    std::fprintf(stderr,
                 "usage: broremote proxy [--socket NAME]\n"
                 "       broremote serve-test [--socket NAME] [--size WxH] [--codec raw|h264|hevc|av1] [--fps N]\n"
                 "                            [--seconds N]\n"
                 "       broremote codecs\n"
                 "       broremote --version\n");
    return 2;
}

bool parse_uint(const char* s, uint32_t& out) {
    char* end = nullptr;
    const unsigned long v = std::strtoul(s, &end, 10);
    if (!end || *end || end == s || v > 0xFFFFFFFFul) return false;
    out = uint32_t(v);
    return true;
}

bool parse_size(const char* s, uint32_t& w, uint32_t& h) {
    const char* x = std::strchr(s, 'x');
    if (!x) return false;
    const std::string a(s, x);
    return parse_uint(a.c_str(), w) && parse_uint(x + 1, h) && w > 0 && h > 0 && w <= kMaxDimension &&
           h <= kMaxDimension;
}

int cmd_proxy(int argc, char** argv) {
    std::string name(kDefaultSocketName);
    for (int i = 0; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--socket") && i + 1 < argc) name = argv[++i];
        else return usage();
    }
    std::string err;
    const int rc = run_proxy(name, &err);
    if (rc != 0) std::fprintf(stderr, "broremote proxy: %s\n", err.c_str());
    return rc;
}

const char* kind_name(InputKind k) {
    switch (k) {
        case InputKind::Key: return "key";
        case InputKind::PointerMotion: return "motion";
        case InputKind::Button: return "button";
        case InputKind::Wheel: return "wheel";
    }
    return "?";
}

int cmd_serve_test(int argc, char** argv) {
    ServerConfig cfg;
    cfg.name = "broremote serve-test";
    cfg.codecs = {Codec::Raw};
    uint32_t width = 1280, height = 720, seconds = 0;
    for (int i = 0; i < argc; ++i) {
        const char* a = argv[i];
        const bool has = i + 1 < argc;
        if (!std::strcmp(a, "--socket") && has) {
            cfg.socket_name = argv[++i];
        } else if (!std::strcmp(a, "--size") && has) {
            if (!parse_size(argv[++i], width, height)) return usage();
        } else if (!std::strcmp(a, "--codec") && has) {
            auto c = parse_codec(argv[++i]);
            if (!c) {
                std::fprintf(stderr, "broremote serve-test: unknown codec '%s'\n", argv[i]);
                return 2;
            }
            cfg.codecs = {*c};
        } else if (!std::strcmp(a, "--fps") && has) {
            if (!parse_uint(argv[++i], cfg.fps) || cfg.fps == 0 || cfg.fps > 1000) return usage();
        } else if (!std::strcmp(a, "--seconds") && has) {
            if (!parse_uint(argv[++i], seconds)) return usage();
        } else {
            return usage();
        }
    }
    std::string err;
    std::unique_ptr<Server> server = Server::create(cfg, &err);
    if (!server) {
        std::fprintf(stderr, "broremote serve-test: %s\n", err.c_str());
        return 1;
    }
    std::fprintf(stderr, "broremote serve-test: %s, %s %ux%u at %u fps\n", server->socket_path().c_str(),
                 codec_name(cfg.codecs.front()), width, height, cfg.fps);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // A small pool of frame buffers: the server holds a frame until it calls
    // its release, so the next one is drawn into a free buffer.
    struct Buffer {
        std::vector<uint8_t> pixels;
        std::atomic<bool> busy{false};
    };
    constexpr int kBuffers = 3;
    std::unique_ptr<Buffer[]> pool(new Buffer[kBuffers]);
    for (int i = 0; i < kBuffers; ++i) pool[i].pixels.resize(size_t(width) * height * 4);

    const auto start = std::chrono::steady_clock::now();
    const auto period = std::chrono::nanoseconds(1000000000ll / cfg.fps);
    auto next = start;
    uint64_t n = 0;
    CursorState cursor;
    std::vector<InputEvent> input;
    while (!g_stop) {
        if (seconds && std::chrono::steady_clock::now() - start >= std::chrono::seconds(seconds)) break;
        input.clear();
        server->drain_input(input);
        for (const InputEvent& e : input) {
            std::fprintf(stderr, "input %s code=%u pressed=%d x=%.1f y=%.1f wheel=%d,%d\n", kind_name(e.kind), e.code,
                         int(e.pressed), double(e.x), double(e.y), e.wheel_x, e.wheel_y);
            if (e.kind == InputKind::PointerMotion) {
                cursor.x = int32_t(e.x);
                cursor.y = int32_t(e.y);
                server->set_cursor(cursor);
            }
        }
        if (server->wants_frames()) {
            Buffer* free_buf = nullptr;
            for (int i = 0; i < kBuffers && !free_buf; ++i) {
                if (!pool[i].busy.load()) free_buf = &pool[i];
            }
            if (free_buf) {
                free_buf->busy = true;
                tools::draw_test_pattern(free_buf->pixels.data(), width, height, n);
                Frame f;
                f.width = width;
                f.height = height;
                f.cpu = free_buf->pixels.data();
                f.cpu_stride = width * 4;
                f.pts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                               .count();
                server->submit(f, [free_buf] { free_buf->busy = false; });
                ++n;
            }
        }
        next += period;
        const auto now = std::chrono::steady_clock::now();
        if (next < now) next = now;
        std::this_thread::sleep_until(next);
    }
    server.reset();
    std::fprintf(stderr, "broremote serve-test: stopped after %llu frames\n", static_cast<unsigned long long>(n));
    return 0;
}

int cmd_codecs() {
    std::printf("encode:");
    for (Codec c : available_encoders()) std::printf(" %s", codec_name(c));
    std::printf("\ndecode:");
    for (Codec c : available_decoders()) std::printf(" %s", codec_name(c));
    std::printf("\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string cmd = argv[1];
    if (cmd == "proxy") return cmd_proxy(argc - 2, argv + 2);
    if (cmd == "serve-test") return cmd_serve_test(argc - 2, argv + 2);
    if (cmd == "codecs") return cmd_codecs();
    if (cmd == "--version") {
        std::printf("broremote %s, protocol %u.%u\n", "0.1.0", unsigned(kProtocolMajor), unsigned(kProtocolMinor));
        return 0;
    }
    return usage();
}
