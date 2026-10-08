// End to end through the broremote executable: a viewer reaching an
// in-process server through `broremote proxy` run as a child stream (what
// `ssh host broremote proxy` does remotely), and `broremote serve-test`.
//   test_proxy <path to broremote>
#include "broremote/protocol.h"
#include "check.h"
#include "test_pattern.h"
#include "viewer.h"

#include <algorithm>
#include <filesystem>
#include <random>
#include <thread>

using namespace broremote;
using testkit::FrameSource;
using testkit::Viewer;

namespace {

std::string g_exe;

std::string unique_name(const char* what) {
    static std::random_device rd;
    return std::string("t-") + what + "-" + std::to_string(rd() % 1000000);
}

std::unique_ptr<Stream> proxy_to(const std::string& name) {
    std::string err;
    auto s = spawn_stream({g_exe, "proxy", "--socket", name}, &err);
    if (!s) std::printf("   spawn_stream: %s\n", err.c_str());
    return s;
}

void test_proxy_relay() {
    check::phase("proxy relays a whole session");
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
        Viewer v;
        CHECK(v.open(proxy_to(name)));
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
    const auto have = available_encoders();
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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: test_proxy <path to broremote>\n");
        return 2;
    }
    g_exe = argv[1];
    check::watchdog(240);
    test_proxy_relay();
    test_serve_test_unavailable_codec();
    test_serve_test_pattern();
    return check::finish();
}
