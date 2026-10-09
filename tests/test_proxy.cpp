// End to end through the broremote executable: a viewer reaching an
// in-process server through `broremote proxy` run as a child stream (what
// `ssh host broremote proxy` does remotely), `broremote serve-test`, and
// `broremote probe` against it.
//   test_proxy <path to broremote>
#include "broremote/protocol.h"
#include "check.h"
#include "test_pattern.h"
#include "viewer.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <random>
#include <thread>

#if !defined(_WIN32)
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace broremote;
using testkit::FrameSource;
using testkit::Viewer;

namespace {

std::string g_exe;

std::string unique_name(const char* what) {
    static std::random_device rd;
    return std::string("t-") + what + "-" + std::to_string(rd() % 1000000);
}

// How the viewer reaches the proxy: plain pipes (ssh -T), pipes with the
// --pty handshake, or (POSIX) a real terminal in its default cooked mode,
// as ssh -tt gives the proxy, which --pty must make binary-clean.
enum class Via { Pipe, PipePty, Terminal };
Via g_via = Via::Pipe;

#if !defined(_WIN32)
// The master side of a terminal whose slave runs the proxy.
class TerminalStream final : public Stream {
public:
    TerminalStream(int master, pid_t child) : fd_(master), child_(child) {}
    ~TerminalStream() override {
        shutdown();
        ::waitpid(child_, nullptr, 0);
        ::close(fd_);
    }
    size_t read(char* buf, size_t n) override {
        for (;;) {
            const ssize_t r = ::read(fd_, buf, n);
            if (r > 0) return size_t(r);
            if (r < 0 && errno == EINTR) continue;
            return 0;  // EIO once the slave side is gone
        }
    }
    bool write(std::string_view d) override {
        std::lock_guard<std::mutex> lk(wm_);
        while (!d.empty()) {
            const ssize_t r = ::write(fd_, d.data(), d.size());
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) return false;
            d.remove_prefix(size_t(r));
        }
        return true;
    }
    void shutdown() override {
        if (!stopped_.exchange(true)) ::kill(child_, SIGTERM);
    }

private:
    int fd_;
    pid_t child_;
    std::mutex wm_;
    std::atomic<bool> stopped_{false};
};
#endif

std::unique_ptr<Stream> proxy_to(const std::string& name) {
    std::string err;
    std::unique_ptr<Stream> s;
    if (g_via == Via::Pipe) {
        s = spawn_stream({g_exe, "proxy", "--socket", name}, &err);
    } else if (g_via == Via::PipePty) {
        s = await_proxy_ready(spawn_stream({g_exe, "proxy", "--socket", name, "--pty"}, &err));
    } else {
#if !defined(_WIN32)
        int master = -1;
        const pid_t pid = ::forkpty(&master, nullptr, nullptr, nullptr);
        if (pid == 0) {
            ::execl(g_exe.c_str(), g_exe.c_str(), "proxy", "--socket", name.c_str(), "--pty", (char*)nullptr);
            ::_exit(127);
        }
        if (pid < 0) err = "forkpty failed";
        else s = await_proxy_ready(std::make_unique<TerminalStream>(master, pid));
#endif
    }
    if (!s) std::printf("   spawn_stream: %s\n", err.c_str());
    return s;
}

void test_proxy_relay() {
    check::phase(g_via == Via::Pipe       ? "proxy relays a whole session"
                 : g_via == Via::PipePty ? "proxy --pty over pipes relays a whole session"
                                         : "proxy --pty on a terminal relays a whole session, binary-clean");
    const std::string name = unique_name("proxy");
    ServerConfig cfg;
    cfg.socket_name = name;
    cfg.codecs = {Codec::Raw};
    std::string err;
    auto server = Server::create(cfg, &err);
    CHECK(server != nullptr);
    if (!server) return;
    FrameSource src;
    {
        // Input on its own lane: a second proxy, the same way as the first.
        Viewer v;
        ClientOptions o;
        o.open_input_lane = [&](std::string*) { return proxy_to(name); };
        CHECK(v.open(proxy_to(name), o));
        CHECK(v.client().input_lane());
        if (!v.client().input_lane()) std::printf("   input lane: %s\n", v.client().input_lane_error().c_str());
        WAIT(server->client_count() == 1, 10000);
        // Big enough frames that a packet spans many reads and writes.
        for (uint32_t i = 0; i < 4; ++i) {
            auto& s = src.submit(*server, 640, 360, i);
            WAIT(v.picture_count() == i + 1, 10000);
            CHECK(v.last_picture().pixels == s.pixels);
        }
        auto& r = src.submit(*server, 320, 200, 9);
        WAIT(v.picture_count() == 5, 10000);
        CHECK(v.last_picture().keyframe);
        CHECK(v.last_picture().pixels == r.pixels);
        CHECK_EQ(v.config_count(), size_t(2));
        v.client().send_input(InputEvent::key(42, true));
        v.client().send_input(InputEvent::motion(1.5f, 2.5f));
        std::vector<InputEvent> got;
        CHECK(check::wait_for([&] {
            server->drain_input(got);
            return got.size() >= 2;
        }));
        CHECK(got.size() == 2 && got[0] == InputEvent::key(42, true) && got[1] == InputEvent::motion(1.5f, 2.5f));
        CHECK_EQ(server->stats().lane_inputs, uint64_t(2));
        CHECK(v.with([&] { return v.decode_errors.empty(); }));
    }
    // The viewer closing its end of the proxy ends the proxy and its connection.
    WAIT(server->client_count() == 0, 10000);

    check::phase("proxy: the server going away ends the viewer");
    {
        Viewer v;
        CHECK(v.open(proxy_to(name)));
        WAIT(server->client_count() == 1, 10000);
        server.reset();
        WAIT(v.is_closed(), 10000);
        CHECK(v.has_error(ErrorCode::ServerShutdown));
    }
    CHECK(src.all_released_once());

    check::phase("proxy: no server");
    {
        Viewer v;
        CHECK(!v.open(proxy_to(unique_name("nobody"))));
        CHECK(v.connect_error.find("no server") != std::string::npos);
        std::printf("   (%s)\n", v.connect_error.c_str());
    }
}

void test_serve_test_unavailable_codec() {
    check::phase("serve-test: an unavailable codec exits nonzero");
    const auto have = brovideo::codecs(brovideo::Direction::Encode);
    const char* missing = nullptr;
    for (Codec c : {Codec::AV1, Codec::HEVC, Codec::H264}) {
        if (std::find(have.begin(), have.end(), c) == have.end()) missing = codec_name(c);
    }
    if (!missing) {
        std::printf("   (every codec is available here; skipped)\n");
        return;
    }
    std::string err;
    auto p = Process::spawn({g_exe, "serve-test", "--codec", missing, "--socket", unique_name("nocodec")}, &err);
    CHECK(p != nullptr);
    if (!p) return;
    int code = 0;
    CHECK(p->wait_for(std::chrono::seconds(20), &code));
    CHECK(code != 0);
}

void test_serve_test_pattern() {
    check::phase("serve-test: the moving pattern");
    const std::string name = unique_name("pattern");
    std::string err;
    auto p = Process::spawn(
        {g_exe, "serve-test", "--socket", name, "--size", "640x360", "--fps", "30", "--seconds", "3"}, &err);
    CHECK(p != nullptr);
    if (!p) return;
    std::unique_ptr<Stream> s;
    CHECK(check::wait_for(
        [&] {
            s = connect_local(name);
            return s != nullptr;
        },
        10000));
    if (!s) return;
    Viewer v;
    CHECK(v.open(std::move(s)));
    WAIT(v.picture_count() >= 10, 20000);
    auto first = v.picture(0);
    auto later = v.last_picture();
    CHECK(first.keyframe);
    CHECK_EQ(first.width, 640u);
    const int64_t c0 = tools::read_test_pattern_counter(first.pixels.data(), 640, 360, 640 * 4);
    const int64_t c1 = tools::read_test_pattern_counter(later.pixels.data(), 640, 360, 640 * 4);
    CHECK(c0 >= 0 && c1 > c0);
    // The swatches: pure red, green, blue at the top left.
    const uint8_t* px = later.pixels.data();
    const size_t row = 640 * 4, mid = 8 * row;
    CHECK(px[mid + 8 * 4] == 255 && px[mid + 8 * 4 + 1] == 0 && px[mid + 8 * 4 + 2] == 0);
    CHECK(px[mid + 24 * 4] == 0 && px[mid + 24 * 4 + 1] == 255 && px[mid + 24 * 4 + 2] == 0);
    CHECK(px[mid + 40 * 4] == 0 && px[mid + 40 * 4 + 1] == 0 && px[mid + 40 * 4 + 2] == 255);
    // The pattern drawn here for the same counter matches the decoded picture exactly.
    std::vector<uint8_t> want(size_t(640) * 360 * 4);
    tools::draw_test_pattern(want.data(), 640, 360, uint64_t(c1));
    CHECK(want == later.pixels);
    v.client().send_input(InputEvent::motion(5, 6));
    // serve-test stops by itself (--seconds) while the viewer is attached:
    // the viewer is told, and the socket file is removed.
    WAIT(v.is_closed(), 20000);
    CHECK(v.has_error(ErrorCode::ServerShutdown));
    int code = -1;
    CHECK(p->wait_for(std::chrono::seconds(20), &code));
    CHECK_EQ(code, 0);
    bool not_running = false;
    CHECK(connect_local(name, nullptr, &not_running) == nullptr);
    CHECK(not_running);
    std::string path = socket_path(name);
    CHECK(!path.empty());
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::symlink_status(std::filesystem::path(path), ec))) {
        check::fail(__FILE__, __LINE__, "the socket file outlived serve-test: " + path);
    }
    v.close();
}

// `broremote probe` against `broremote serve-test`, both as processes: the
// scripted checks (pictures, the pattern, a PNG; then latency probes).
bool wait_for_server(const std::string& name) {
    return check::wait_for([&] { return connect_local(name) != nullptr; }, 10000);
}

void test_probe() {
    check::phase("probe: frames, the pattern, a PNG");
    std::string err;
    {
        const std::string name = unique_name("probe");
        auto server = Process::spawn(
            {g_exe, "serve-test", "--socket", name, "--size", "640x360", "--fps", "60", "--seconds", "30", "--no-audio"},
            &err);
        CHECK(server != nullptr);
        if (!server) return;
        CHECK(wait_for_server(name));
        const std::filesystem::path png = std::filesystem::temp_directory_path() / (name + ".png");
        auto probe = Process::spawn({g_exe, "probe", "--socket", name, "--frames", "30", "--timeout", "20",
                                     "--check-pattern", "--dump-png", png.string()},
                                    &err);
        CHECK(probe != nullptr);
        int code = -1;
        if (probe) {
            CHECK(probe->wait_for(std::chrono::seconds(30), &code));
            CHECK_EQ(code, 0);
        }
        std::error_code ec;
        CHECK(std::filesystem::exists(png, ec) && std::filesystem::file_size(png, ec) > size_t(640) * 360 * 4);
        std::filesystem::remove(png, ec);
        server->kill();
        server->wait_for(std::chrono::seconds(10));
    }

    check::phase("probe: latency probes against serve-test --latency");
    {
        const std::string name = unique_name("probelat");
        auto server = Process::spawn({g_exe, "serve-test", "--socket", name, "--size", "640x360", "--fps", "60",
                                      "--seconds", "30", "--latency", "--no-audio"},
                                     &err);
        CHECK(server != nullptr);
        if (!server) return;
        CHECK(wait_for_server(name));
        auto probe = Process::spawn({g_exe, "probe", "--socket", name, "--latency-test", "5", "--stats"}, &err);
        CHECK(probe != nullptr);
        int code = -1;
        if (probe) {
            CHECK(probe->wait_for(std::chrono::seconds(30), &code));
            CHECK_EQ(code, 0);
        }
        server->kill();
        server->wait_for(std::chrono::seconds(10));
    }

    check::phase("probe: no server fails");
    {
        auto probe = Process::spawn({g_exe, "probe", "--socket", unique_name("none"), "--frames", "1"}, &err);
        CHECK(probe != nullptr);
        int code = 0;
        if (probe) {
            CHECK(probe->wait_for(std::chrono::seconds(20), &code));
            CHECK(code != 0);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: test_proxy <path to broremote>\n");
        return 2;
    }
    g_exe = argv[1];
    check::watchdog(240);
    test_proxy_relay();
    g_via = Via::PipePty;
    test_proxy_relay();
#if !defined(_WIN32)
    g_via = Via::Terminal;
    test_proxy_relay();
    g_via = Via::Pipe;
#endif
    test_serve_test_unavailable_codec();
    test_serve_test_pattern();
    test_probe();
    return check::finish();
}
