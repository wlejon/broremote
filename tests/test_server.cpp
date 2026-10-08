// Server and clients in one process over a real local socket, Codec::Raw and
// CPU frames: config, exact pixels, acks and flow control, keyframes on join,
// request and resize, input, cursor, two clients, protocol errors, and the
// release callback called exactly once per frame on every path.
#include "broremote/protocol.h"
#include "check.h"
#include "viewer.h"

#include <algorithm>
#include <optional>
#include <random>
#include <thread>

using namespace broremote;
using testkit::FrameSource;
using testkit::Viewer;

namespace {

std::string unique_name(const char* what) {
    static std::random_device rd;
    return std::string("t-") + what + "-" + std::to_string(rd() % 1000000);
}

std::unique_ptr<Server> make_server(const std::string& name, uint32_t window = 2) {
    ServerConfig cfg;
    cfg.socket_name = name;
    cfg.codecs = {Codec::Raw};
    cfg.max_frames_in_flight = window;
    cfg.name = "test-server";
    std::string err;
    auto s = Server::create(cfg, &err);
    if (!s) std::printf("   Server::create: %s\n", err.c_str());
    return s;
}

std::unique_ptr<Stream> dial(const std::string& name) {
    std::string err;
    auto s = connect_local(name, &err);
    if (!s) std::printf("   connect_local: %s\n", err.c_str());
    return s;
}

void settle(int ms = 150) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ---------------------------------------------------------------------------------

void test_no_client() {
    check::phase("no client: frames released at once");
    auto server = make_server(unique_name("idle"));
    CHECK(server != nullptr);
    if (!server) return;
    CHECK(!server->wants_frames());
    FrameSource src;
    auto& s = src.submit(*server, 8, 8, 1);
    CHECK_EQ(s.releases.load(), 1);  // inside submit()
    CHECK_EQ(server->stats().unwatched, uint64_t(1));
    server.reset();
    CHECK(src.all_released_once());
}

void test_duplicate_and_codecs() {
    check::phase("create: duplicate socket, no codec");
    const std::string name = unique_name("dup");
    auto a = make_server(name);
    CHECK(a != nullptr);
    ServerConfig cfg;
    cfg.socket_name = name;
    cfg.codecs = {Codec::Raw};
    std::string err;
    auto b = Server::create(cfg, &err);
    CHECK(b == nullptr);
    CHECK(err.find("already") != std::string::npos);
    a.reset();
    // Once the first is gone the name is free again.
    auto c = Server::create(cfg, &err);
    CHECK(c != nullptr);
    c.reset();
    // A codec list with nothing this build can encode.
    std::vector<Codec> missing;
    const auto have = brovideo::codecs(brovideo::Direction::Encode);
    for (Codec k : {Codec::H264, Codec::HEVC, Codec::AV1}) {
        if (std::find(have.begin(), have.end(), k) == have.end()) missing.push_back(k);
    }
    if (!missing.empty()) {
        cfg.codecs = missing;
        auto d = Server::create(cfg, &err);
        CHECK(d == nullptr);
        CHECK(err.find("can be encoded") != std::string::npos);
    }
    CHECK(!valid_socket_name("../x"));
    CHECK(!valid_socket_name(""));
    CHECK(!valid_socket_name(".hidden"));
    CHECK(valid_socket_name("a.b_c-9"));
}

void test_basic_stream() {
    check::phase("config, frames, exact pixels");
    const std::string name = unique_name("basic");
    auto server = make_server(name);
    if (!server) {
        CHECK(false);
        return;
    }
    FrameSource src;
    {
        Viewer v;
        ClientOptions o;
        o.name = "viewer-1";
        CHECK(v.open(dial(name), o));
        CHECK_EQ(v.client().welcome().name, std::string("test-server"));
        CHECK_EQ(v.client().welcome().major, kProtocolMajor);
        WAIT(server->client_count() == 1, 5000);
        CHECK(server->wants_frames());
        CHECK_EQ(v.config_count(), size_t(0));  // no frame yet, so no stream
        CHECK(!server->stream().has_value());

        for (uint32_t i = 0; i < 5; ++i) {
            auto& s = src.submit(*server, 64, 48, i);
            WAIT(v.picture_count() == i + 1, 5000);
            auto p = v.last_picture();
            CHECK(p.pixels == s.pixels);
            CHECK_EQ(p.width, 64u);
            CHECK_EQ(p.keyframe, i == 0);
            CHECK_EQ(p.frame_id, uint64_t(i + 1));
        }
        CHECK_EQ(v.config_count(), size_t(1));
        auto sc = v.last_config();
        CHECK_EQ(sc.stream_id, uint64_t(1));
        CHECK_EQ(sc.codec, Codec::Raw);
        CHECK_EQ(sc.width, 64u);
        CHECK_EQ(sc.height, 48u);
        CHECK_EQ(sc.fps, 60u);
        const auto info = server->stream();
        CHECK(info.has_value());
        if (info) {
            CHECK_EQ(info->codec, Codec::Raw);
            CHECK_EQ(info->width, 64u);
            CHECK_EQ(info->height, 48u);
            CHECK_EQ(info->bitrate_kbps, 20000u);
        }
        CHECK(v.with([&] { return v.decode_errors.empty(); }));
        auto st = server->stats();
        CHECK_EQ(st.encoded, uint64_t(5));
        CHECK_EQ(st.keyframes, uint64_t(1));
        CHECK_EQ(st.streams, uint64_t(1));
    }
    // The viewer left: nobody is watching.
    WAIT(server->client_count() == 0, 5000);
    CHECK(!server->wants_frames());
    server.reset();
    CHECK(src.all_released_once());
}

void test_flow_control() {
    check::phase("flow control: no acks, no encoding, no backlog");
    const std::string name = unique_name("flow");
    auto server = make_server(name, 2);
    if (!server) {
        CHECK(false);
        return;
    }
    FrameSource src;
    Viewer v;
    v.auto_ack = false;
    CHECK(v.open(dial(name)));
    WAIT(server->client_count() == 1, 5000);
    src.submit(*server, 32, 32, 0);
    WAIT(v.picture_count() == 1, 5000);
    src.submit(*server, 32, 32, 1);
    WAIT(v.picture_count() == 2, 5000);
    // At the window: the next frames wait, each replacing the one before.
    FrameSource::Slot* newest = nullptr;
    for (uint32_t i = 2; i < 7; ++i) {
        newest = &src.submit(*server, 32, 32, i);
        settle(30);
    }
    settle(200);
    CHECK_EQ(v.picture_count(), size_t(2));
    auto st = server->stats();
    CHECK_EQ(st.encoded, uint64_t(2));
    CHECK_EQ(st.replaced, uint64_t(4));  // frames 2..5 replaced, 6 still pending
    // Acking the first frame opens one slot: exactly one frame, the newest.
    v.client().ack(1);
    WAIT(v.picture_count() == 3, 5000);
    settle(200);
    CHECK_EQ(v.picture_count(), size_t(3));
    auto last = v.last_picture();
    CHECK_EQ(last.frame_id, uint64_t(3));
    CHECK(newest && last.pixels == newest->pixels);  // frame 6, the last submitted
    // Ack everything: the stream flows again.
    v.client().ack(3);
    src.submit(*server, 32, 32, 7);
    WAIT(v.picture_count() == 4, 5000);
    // Acking out of order or beyond what was sent is harmless.
    v.client().ack(1);
    v.client().ack(1000);
    settle(150);  // let the acks land before the next frame, which they must not cover
    src.submit(*server, 32, 32, 8);  // frame id 5, unacked
    WAIT(v.picture_count() == 5, 5000);
    CHECK(v.with([&] { return v.decode_errors.empty(); }));

    check::phase("flow control: shutdown releases a held frame");
    src.submit(*server, 32, 32, 9);  // frame id 6: the window is full again
    WAIT(v.picture_count() == 6, 5000);
    auto& held = src.submit(*server, 32, 32, 10);
    settle(100);
    CHECK_EQ(held.releases.load(), 0);  // pending while paused
    server.reset();
    CHECK_EQ(held.releases.load(), 1);
    WAIT(v.is_closed(), 5000);
    CHECK(v.has_error(ErrorCode::ServerShutdown));
    CHECK(src.all_released_once());
}

void test_flow_pixels() {
    check::phase("flow control: the frame sent on resume is the newest");
    const std::string name = unique_name("newest");
    auto server = make_server(name, 1);
    if (!server) {
        CHECK(false);
        return;
    }
    FrameSource src;
    Viewer v;
    v.auto_ack = false;
    CHECK(v.open(dial(name)));
    WAIT(server->client_count() == 1, 5000);
    src.submit(*server, 16, 16, 0);
    WAIT(v.picture_count() == 1, 5000);
    src.submit(*server, 16, 16, 1);
    src.submit(*server, 16, 16, 2);
    auto& newest = src.submit(*server, 16, 16, 3);
    settle(100);
    CHECK_EQ(v.picture_count(), size_t(1));
    v.client().ack(1);
    WAIT(v.picture_count() == 2, 5000);
    CHECK(v.last_picture().pixels == newest.pixels);
    v.close();
    server.reset();
    CHECK(src.all_released_once());
}

void test_keyframes_and_two_clients() {
    check::phase("keyframe on join, two clients");
    const std::string name = unique_name("join");
    auto server = make_server(name, 2);
    if (!server) {
        CHECK(false);
        return;
    }
    FrameSource src;
    Viewer a;
    CHECK(a.open(dial(name)));
    WAIT(server->client_count() == 1, 5000);
    for (uint32_t i = 0; i < 3; ++i) {
        src.submit(*server, 40, 30, i);
        WAIT(a.picture_count() == i + 1, 5000);
    }
    CHECK_EQ(server->stats().keyframes, uint64_t(1));

    Viewer b;
    CHECK(b.open(dial(name)));
    WAIT(server->client_count() == 2, 5000);
    // The joiner is told the current stream at once.
    WAIT(b.config_count() == 1, 5000);
    CHECK_EQ(b.last_config().stream_id, uint64_t(1));
    auto& s3 = src.submit(*server, 40, 30, 3);
    WAIT(a.picture_count() == 4 && b.picture_count() == 1, 5000);
    CHECK(b.picture(0).keyframe);
    CHECK(a.last_picture().keyframe);
    CHECK(b.picture(0).pixels == s3.pixels);
    CHECK(a.last_picture().pixels == s3.pixels);
    CHECK_EQ(server->stats().keyframes, uint64_t(2));
    // Both get the following predicted frames.
    auto& s4 = src.submit(*server, 40, 30, 4);
    WAIT(a.picture_count() == 5 && b.picture_count() == 2, 5000);
    CHECK(!b.last_picture().keyframe);
    CHECK(b.last_picture().pixels == s4.pixels);

    check::phase("keyframe on request");
    b.client().request_keyframe();
    settle(50);
    src.submit(*server, 40, 30, 5);
    WAIT(a.picture_count() == 6 && b.picture_count() == 3, 5000);
    CHECK(b.last_picture().keyframe);
    CHECK_EQ(server->stats().keyframes, uint64_t(3));

    check::phase("two clients: one slow client pauses both; its leaving resumes");
    b.auto_ack = false;
    src.submit(*server, 40, 30, 6);
    WAIT(a.picture_count() == 7 && b.picture_count() == 4, 5000);
    src.submit(*server, 40, 30, 7);
    WAIT(a.picture_count() == 8 && b.picture_count() == 5, 5000);
    src.submit(*server, 40, 30, 8);  // b is at its window: waits
    settle(200);
    CHECK_EQ(a.picture_count(), size_t(8));
    b.close();  // the slow client leaves
    WAIT(server->client_count() == 1, 5000);
    WAIT(a.picture_count() == 9, 5000);
    CHECK(a.with([&] { return a.decode_errors.empty(); }));
    CHECK(b.with([&] { return b.decode_errors.empty(); }));
    a.close();
    server.reset();
    CHECK(src.all_released_once());
}

void test_resize() {
    check::phase("resize: new StreamConfig and a keyframe");
    const std::string name = unique_name("resize");
    auto server = make_server(name);
    if (!server) {
        CHECK(false);
        return;
    }
    FrameSource src;
    Viewer v;
    CHECK(v.open(dial(name)));
    WAIT(server->client_count() == 1, 5000);
    src.submit(*server, 64, 48, 0);
    WAIT(v.picture_count() == 1, 5000);
    src.submit(*server, 64, 48, 1);
    WAIT(v.picture_count() == 2, 5000);
    auto& big = src.submit(*server, 80, 40, 2);
    WAIT(v.picture_count() == 3, 5000);
    CHECK_EQ(v.config_count(), size_t(2));
    auto sc = v.last_config();
    CHECK_EQ(sc.stream_id, uint64_t(2));
    CHECK_EQ(sc.width, 80u);
    CHECK_EQ(sc.height, 40u);
    auto p = v.last_picture();
    CHECK(p.keyframe);
    CHECK_EQ(p.stream_id, uint64_t(2));
    CHECK_EQ(p.width, 80u);
    CHECK(p.pixels == big.pixels);
    auto& next = src.submit(*server, 80, 40, 3);
    WAIT(v.picture_count() == 4, 5000);
    CHECK(!v.last_picture().keyframe);
    CHECK(v.last_picture().pixels == next.pixels);
    CHECK_EQ(server->stats().streams, uint64_t(2));
    v.close();
    server.reset();
    CHECK(src.all_released_once());
}

void test_input_and_cursor() {
    check::phase("input round trip");
    const std::string name = unique_name("input");
    auto server = make_server(name);
    if (!server) {
        CHECK(false);
        return;
    }
    CursorState cur;
    cur.x = 10;
    cur.y = 20;
    cur.shape = "text";
    server->set_cursor(cur);
    Viewer v;
    CHECK(v.open(dial(name)));
    WAIT(server->client_count() == 1, 5000);
    const std::vector<InputEvent> sent = {InputEvent::key(30, true),        InputEvent::key(30, false),
                                          InputEvent::motion(100.5f, 7.25f), InputEvent::button(0x110, true),
                                          InputEvent::button(0x110, false),  InputEvent::wheel(0, -240)};
    for (const auto& e : sent) v.client().send_input(e);
    std::vector<InputEvent> got;
    CHECK(check::wait_for([&] {
        server->drain_input(got);
        return got.size() >= sent.size();
    }));
    CHECK(got == sent);
    got.clear();
    server->drain_input(got);
    CHECK(got.empty());

    check::phase("cursor: on join, on change, not repeated");
    WAIT(v.with([&] { return v.cursors.size(); }) == 1, 5000);
    CHECK(v.with([&] { return v.cursors[0] == cur; }));
    cur.x = 11;
    server->set_cursor(cur);
    server->set_cursor(cur);  // unchanged: not sent again
    cur.visible = false;
    server->set_cursor(cur);
    WAIT(v.with([&] { return v.cursors.size(); }) == 3, 5000);
    settle(100);
    CHECK_EQ(v.with([&] { return v.cursors.size(); }), size_t(3));
    CHECK(v.with([&] { return v.cursors.back() == cur; }));
    v.close();
}

void test_codec_negotiation() {
    check::phase("SetCodec with no common codec");
    const std::string name = unique_name("codec");
    auto server = make_server(name);
    if (!server) {
        CHECK(false);
        return;
    }
    {
        Viewer ok;
        ClientOptions o;
        o.codecs = {Codec::H264, Codec::Raw};
        CHECK(ok.open(dial(name), o));
        WAIT(server->client_count() == 1, 5000);
        FrameSource src;
        src.submit(*server, 8, 8, 0);
        WAIT(ok.picture_count() == 1, 5000);
        CHECK_EQ(ok.last_config().codec, Codec::Raw);

        Viewer bad;
        ClientOptions ob;
        ob.codecs = {Codec::AV1};
        CHECK(bad.open(dial(name), ob));
        WAIT(bad.is_closed(), 5000);
        CHECK(bad.has_error(ErrorCode::NoCommonCodec));
        WAIT(server->client_count() == 1, 5000);
        // The good viewer is unaffected.
        src.submit(*server, 8, 8, 1);
        WAIT(ok.picture_count() == 2, 5000);
        ok.close();
        server.reset();
        CHECK(src.all_released_once());
    }
}

// A raw protocol peer, for what a well-behaved Client never sends.
struct RawPeer {
    std::unique_ptr<Stream> s;
    std::thread reader;
    std::mutex m;
    std::vector<std::pair<uint16_t, std::string>> msgs;
    bool eof = false;

    explicit RawPeer(std::unique_ptr<Stream> st) : s(std::move(st)) {
        reader = std::thread([this] {
            wire::MessageSplitter sp;
            char buf[4096];
            for (;;) {
                size_t n = s->read(buf, sizeof buf);
                if (n == 0) break;
                sp.feed(buf, n);
                wire::MessageSplitter::Message msg;
                std::lock_guard<std::mutex> lk(m);
                while (sp.next(msg)) msgs.emplace_back(msg.type, std::string(msg.payload));
            }
            std::lock_guard<std::mutex> lk(m);
            eof = true;
        });
    }
    ~RawPeer() {
        s->shutdown();
        reader.join();
    }
    bool closed() {
        std::lock_guard<std::mutex> lk(m);
        return eof;
    }
    std::optional<ErrorCode> error() {
        std::lock_guard<std::mutex> lk(m);
        for (auto& [t, p] : msgs) {
            if (MsgType(t) == MsgType::Error) {
                ErrorMsg e;
                if (e.decode(p)) return e.code;
            }
        }
        return std::nullopt;
    }
    bool got(MsgType type) {
        std::lock_guard<std::mutex> lk(m);
        for (auto& [t, p] : msgs) {
            if (MsgType(t) == type) return true;
        }
        return false;
    }
    size_t count(MsgType type) {
        std::lock_guard<std::mutex> lk(m);
        size_t n = 0;
        for (auto& [t, p] : msgs) n += MsgType(t) == type ? 1 : 0;
        return n;
    }
};

void test_protocol_errors() {
    const std::string name = unique_name("proto");
    auto server = make_server(name);
    if (!server) {
        CHECK(false);
        return;
    }
    check::phase("protocol: message before Hello");
    {
        RawPeer p(dial(name));
        p.s->write(AckMsg{1}.encode());
        WAIT(p.closed(), 5000);
        CHECK(p.error() == ErrorCode::HelloRequired);
    }
    check::phase("protocol: other major");
    {
        RawPeer p(dial(name));
        HelloMsg h;
        h.major = kProtocolMajor + 1;
        p.s->write(h.encode());
        WAIT(p.closed(), 5000);
        CHECK(p.error() == ErrorCode::VersionMismatch);
    }
    check::phase("protocol: bad magic");
    {
        RawPeer p(dial(name));
        p.s->write(wire::make_message(uint16_t(MsgType::Hello), std::string("XXXX\x01\x00\x00\x00\x00", 9)));
        WAIT(p.closed(), 5000);
        CHECK(p.error() == ErrorCode::BadMessage);
    }
    check::phase("protocol: framing error");
    {
        RawPeer p(dial(name));
        p.s->write(std::string("\x01\x00\x00\x00\x05", 5));
        WAIT(p.closed(), 5000);
        CHECK(p.error() == ErrorCode::BadMessage);
    }
    check::phase("protocol: huge length");
    {
        RawPeer p(dial(name));
        p.s->write(std::string("\xFF\xFF\xFF\x7F\x01\x01", 6));
        WAIT(p.closed(), 5000);
        CHECK(p.error() == ErrorCode::BadMessage);
    }
    check::phase("protocol: unknown type answered, connection kept");
    {
        RawPeer p(dial(name));
        p.s->write(HelloMsg{}.encode());
        WAIT(p.got(MsgType::Welcome), 5000);
        p.s->write(wire::make_message(0x01FE, "?"));
        WAIT(p.error().has_value(), 5000);
        CHECK(p.error() == ErrorCode::UnknownMessage);
        // An input kind from the future is ignored; a malformed Ack closes.
        p.s->write(wire::make_message(uint16_t(MsgType::Input), std::string("\x63\x01", 2)));
        settle(100);
        CHECK(!p.closed());
        CHECK_EQ(server->client_count(), size_t(1));
        p.s->write(wire::make_message(uint16_t(MsgType::Ack), std::string("\xFF", 1)));
        WAIT(p.closed(), 5000);
    }
    WAIT(server->client_count() == 0, 5000);
    check::phase("protocol: abrupt disconnects");
    for (int i = 0; i < 5; ++i) {
        auto s = dial(name);
        if (s) s->write(std::string(HelloMsg{}.encode()).substr(0, 7));  // half a Hello, then gone
    }
    settle(100);
    CHECK_EQ(server->client_count(), size_t(0));
    // Still serving.
    Viewer v;
    CHECK(v.open(dial(name)));
    v.close();
}

void test_timing() {
    const std::string name = unique_name("timing");
    auto server = make_server(name);
    if (!server) {
        CHECK(false);
        return;
    }
    FrameSource src;
    check::phase("1.1: ping / pong, frame timing, FrameSent");
    {
        Viewer v;
        CHECK(v.open(dial(name)));
        CHECK_EQ(v.client().welcome().minor, kProtocolMinor);
        WAIT(server->client_count() == 1, 5000);
        CHECK(v.client().ping());
        WAIT(v.with([&] { return v.pongs.size(); }) == 1, 5000);
        const auto pong = v.with([&] { return v.pongs.front(); });
        CHECK(pong.sent <= pong.received);
        CHECK(pong.server_us > 0);
        for (uint32_t i = 0; i < 3; ++i) {
            src.submit(*server, 32, 16, i);
            WAIT(v.with([&] { return v.frames_sent.size(); }) == i + 1, 5000);
        }
        CHECK(v.client().ping());
        WAIT(v.with([&] { return v.pongs.size(); }) == 2, 5000);
        const uint64_t later_us = v.with([&] { return v.pongs.back().server_us; });
        v.with([&] {
            CHECK_EQ(v.timings.size(), size_t(3));
            for (size_t i = 0; i < v.timings.size() && i < v.frames_sent.size(); ++i) {
                const FrameTiming& t = v.timings[i];
                CHECK(t.valid);
                CHECK(t.submit_us >= pong.server_us && t.submit_us <= later_us);
                CHECK(t.queue_us < 5000000 && t.encode_us < 5000000);
                CHECK_EQ(v.frames_sent[i].frame_id, uint64_t(i + 1));
                CHECK(v.frames_sent[i].wait_us < 5000000);
            }
            return 0;
        });
    }
    check::phase("1.0 client: no FrameSent; a frame at the shut window waits and is counted");
    WAIT(server->client_count() == 0, 5000);
    {
        RawPeer p(dial(name));
        HelloMsg h;
        h.minor = 0;
        p.s->write(h.encode());
        WAIT(p.got(MsgType::Welcome), 5000);
        WAIT(server->client_count() == 1, 5000);
        const uint64_t waits = server->stats().window_waits;
        src.submit(*server, 32, 16, 7);
        WAIT(p.count(MsgType::Video) == 1, 5000);
        src.submit(*server, 32, 16, 8);
        WAIT(p.count(MsgType::Video) == 2, 5000);
        src.submit(*server, 32, 16, 9);  // two unacked: the window is shut
        settle(100);
        CHECK_EQ(p.count(MsgType::Video), size_t(2));
        CHECK_EQ(server->stats().window_waits, waits + 1);
        CHECK_EQ(p.count(MsgType::FrameSent), size_t(0));
        p.s->write(AckMsg{100}.encode());
        WAIT(p.count(MsgType::Video) == 3, 5000);
        CHECK_EQ(p.count(MsgType::FrameSent), size_t(0));
    }
    server.reset();
    CHECK(src.all_released_once());
}

void test_client_side() {
    check::phase("client: connect failures");
    std::string err;
    bool not_running = false;
    auto s = connect_local(unique_name("nobody"), &err, &not_running);
    CHECK(s == nullptr);
    CHECK(not_running);
    // A server that never answers: the handshake times out.
    const std::string name = unique_name("mute");
    auto server = make_server(name);
    if (!server) {
        CHECK(false);
        return;
    }
    // Fill the server's accept path with a raw peer is not needed: use a
    // stream that never answers instead (a pipe to a peer that reads nothing).
    struct Mute final : Stream {
        std::mutex m;
        std::condition_variable cv;
        bool stop = false;
        size_t read(char*, size_t) override {
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&] { return stop; });
            return 0;
        }
        bool write(std::string_view) override { return true; }
        void shutdown() override {
            std::lock_guard<std::mutex> lk(m);
            stop = true;
            cv.notify_all();
        }
    };
    ClientOptions o;
    o.connect_timeout_ms = 200;
    auto c = Client::connect(std::make_unique<Mute>(), {}, o, &err);
    CHECK(c == nullptr);
    CHECK(err.find("no answer") != std::string::npos);
    // on_closed runs once when the client is destroyed.
    int closed = 0;
    {
        ClientHandlers h;
        h.on_closed = [&](const std::string&) { ++closed; };
        auto cl = Client::connect(dial(name), std::move(h), &err);
        CHECK(cl != nullptr);
        if (cl) CHECK(cl->connected());
    }
    CHECK_EQ(closed, 1);
}

}  // namespace

int main() {
    check::watchdog(240);
    test_no_client();
    test_duplicate_and_codecs();
    test_basic_stream();
    test_flow_control();
    test_flow_pixels();
    test_keyframes_and_two_clients();
    test_resize();
    test_input_and_cursor();
    test_codec_negotiation();
    test_protocol_errors();
    test_timing();
    test_client_side();
    return check::finish();
}
