// The viewer's session (ViewerSession, viewer.h) end to end in one process,
// with no window: an in-process Server (Raw codec) streams a 640x360
// picture to a ViewerSession over a real local connection. Pictures arrive
// whole and newest-first; input sent through the session reaches the server
// as exactly the events sent, in order, on the input lane; the host's
// cursor comes back; latency probes close against an input marker the
// feeder draws (what `broremote serve-test --latency` and `broremote probe
// --latency-test` do); a stream size change shows in the pictures and the
// status; closing from this side is a clean close.
#include "broremote/server.h"
#include "broremote/viewer.h"
#include "check.h"
#include "test_pattern.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

using namespace broremote;

namespace {

// Submits frames of the current size until stopped, from a small pool; with
// the input marker drawn (`presses`) as serve-test --latency does.
class Feeder {
public:
    explicit Feeder(Server& s) : server_(s) {
        thread_ = std::thread([this] { run(); });
    }
    ~Feeder() {
        stop_ = true;
        thread_.join();
    }
    void set_size(uint32_t w, uint32_t h) {
        w_ = w;
        h_ = h;
    }
    std::atomic<uint32_t> presses{0};

private:
    void run() {
        struct Buf {
            std::vector<uint8_t> px;
            std::atomic<bool> busy{false};
        };
        Buf pool[3];
        uint32_t n = 0;
        while (!stop_) {
            const uint32_t w = w_, h = h_;
            for (Buf& b : pool) {
                if (b.busy) continue;
                b.busy = true;
                b.px.assign(size_t(w) * h * 4, uint8_t(n * 7));
                tools::draw_input_marker(b.px.data(), w, h, presses);
                Frame f;
                f.width = w;
                f.height = h;
                f.cpu = b.px.data();
                server_.submit(f, [&b] { b.busy = false; });
                ++n;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        // The server releases what it holds when it goes; wait for that
        // before the pool does.
        for (Buf& b : pool) {
            while (b.busy && server_.client_count() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    Server& server_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<uint32_t> w_{640}, h_{360};
};

std::string show(const InputEvent& e) {
    char buf[96];
    switch (e.kind) {
        case InputKind::Key: std::snprintf(buf, sizeof buf, "key %u %s", e.code, e.pressed ? "down" : "up"); break;
        case InputKind::Button:
            std::snprintf(buf, sizeof buf, "button 0x%x %s", e.code, e.pressed ? "down" : "up");
            break;
        case InputKind::PointerMotion: std::snprintf(buf, sizeof buf, "motion %.2f,%.2f", e.x, e.y); break;
        case InputKind::Wheel: std::snprintf(buf, sizeof buf, "wheel %d,%d", e.wheel_x, e.wheel_y); break;
        case InputKind::RelativeMotion: std::snprintf(buf, sizeof buf, "relative %.2f,%.2f", e.x, e.y); break;
    }
    return buf;
}

// Drains the server until `n` events arrived (or 5 s pass), then a little
// longer for anything extra on its way.
std::vector<InputEvent> collect(Server& s, size_t n) {
    std::vector<InputEvent> got;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (got.size() < n && std::chrono::steady_clock::now() < until) {
        s.drain_input(got);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    s.drain_input(got);
    return got;
}

void expect(const std::vector<InputEvent>& got, const std::vector<InputEvent>& want) {
    CHECK_EQ(got.size(), want.size());
    for (size_t i = 0; i < std::max(got.size(), want.size()); ++i) {
        const std::string g = i < got.size() ? show(got[i]) : "(none)";
        const std::string w = i < want.size() ? show(want[i]) : "(none)";
        if (g != w) std::printf("   event %zu: got %s, want %s\n", i, g.c_str(), w.c_str());
        CHECK(g == w);
    }
}

// The session's wake, as a display waits on it.
struct Waker {
    std::mutex m;
    std::condition_variable cv;
    uint64_t wakes = 0;
    void wake() {
        std::lock_guard<std::mutex> lk(m);
        ++wakes;
        cv.notify_all();
    }
};

}  // namespace

int main() {
    check::watchdog(120);

    check::phase("server");
    ServerConfig cfg;
    cfg.socket_name = "test_viewer_" +
                      std::to_string(int64_t(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000));
    cfg.name = "test_viewer server";
    cfg.codecs = {Codec::Raw};
    cfg.fps = 100;
    cfg.audio.enabled = false;
    std::string err;
    auto server = Server::create(cfg, &err);
    CHECK(server != nullptr);
    if (!server) {
        std::printf("   %s\n", err.c_str());
        return check::finish();
    }
    Feeder feeder(*server);

    {
        Waker waker;
        ViewerSession session([&waker] { waker.wake(); });
        ViewerOptions o;
        o.target.socket = cfg.socket_name;
        o.target.socket_given = true;
        o.audio.enabled = false;
        o.client_name = "test_viewer";

        check::phase("the session shows the stream");
        session.start(o);
        DecodedFrame frame;
        ViewerFrameInfo info;
        uint64_t taken = 0, last_sequence = 0;
        bool ordered = true;
        auto take = [&] {
            if (!session.take_frame(frame, info)) return false;
            ordered = ordered && info.sequence > last_sequence;
            last_sequence = info.sequence;
            session.note_presented(info.frame_id, Clock::now());
            ++taken;
            return true;
        };
        WAIT((take(), taken >= 3), 10000);
        CHECK(ordered);
        CHECK(waker.wakes > 0);
        CHECK(frame.width == 640 && frame.height == 360);
        CHECK(frame.format == PixelFormat::RGBA8);
        CHECK(!session.take_frame(frame, info) || info.sequence > last_sequence);
        ViewerStatus st = session.status();
        CHECK(st.state == ViewerState::Connected);
        CHECK(st.have_config);
        CHECK_EQ(st.config.width, 640u);
        CHECK_EQ(st.config.height, 360u);
        CHECK_EQ(st.server, cfg.name);
        CHECK(st.input_lane);
        const ViewerStats vs = session.stats();
        CHECK(vs.decoded >= 3);
        CHECK_EQ(vs.failed, 0u);

        check::phase("input reaches the server exactly, in order");
        std::vector<InputEvent> scratch;
        server->drain_input(scratch);
        const std::vector<InputEvent> sent = {
            InputEvent::motion(320, 180),       InputEvent::button(0x110, true),  InputEvent::button(0x110, false),
            InputEvent::key(30, true),          InputEvent::key(30, false),       InputEvent::wheel(0, 120),
            InputEvent::wheel(-60, 0),          InputEvent::relative(-3.5f, 2.25f), InputEvent::motion(639, 359),
            InputEvent::key(97, true),          InputEvent::key(97, false)};
        for (const InputEvent& e : sent) session.send_input(e);
        expect(collect(*server, sent.size()), sent);

        check::phase("the host's cursor comes back");
        CursorState c;
        c.x = 12;
        c.y = 34;
        c.shape = "text";
        server->set_cursor(c);
        CursorState got;
        WAIT(session.cursor(got) > 0 && got.x == 12 && got.y == 34, 5000);
        CHECK_EQ(got.shape, std::string("text"));

        check::phase("latency probes close on the input marker");
        // The server's side of serve-test --latency: count the presses into
        // the marker the feeder draws.
        size_t probes = 0;
        const auto until = Clock::now() + std::chrono::seconds(20);
        while (probes < 5 && Clock::now() < until) {
            scratch.clear();
            server->drain_input(scratch);
            for (const InputEvent& e : scratch) {
                if (e.kind == InputKind::Key && e.pressed) ++feeder.presses;
            }
            take();
            if (!session.latency().probe_open(Clock::now())) session.probe();
            probes = session.latency().probes().size();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK_EQ(probes, size_t(5));
        CHECK_EQ(session.latency().probes_lost(), 0u);
        for (const Probe& p : session.latency().probes()) {
            CHECK(p.total_decoded > 0 && p.total_decoded < 2000);
            CHECK(p.total_presented >= p.total_decoded);
        }
        // Timing: once a Pong gave the clock offset, frames taken are placed.
        WAIT((take(), session.latency().rtt_ms() >= 0), 5000);
        session.latency().take_window();
        const uint64_t before = taken;
        WAIT((take(), taken >= before + 5), 5000);
        const LatencyWindow w = session.latency().take_window();
        CHECK(w.frames > 0);

        check::phase("a new stream size");
        feeder.set_size(800, 600);
        WAIT((take(), frame.width == 800), 10000);
        CHECK_EQ(frame.height, 600u);
        st = session.status();
        CHECK_EQ(st.config.width, 800u);
        CHECK_EQ(st.config.height, 600u);

        check::phase("closing from this side");
        session.close();
        WAIT(session.status().state == ViewerState::Closed, 5000);
        CHECK(!session.status().failed);
        WAIT(server->client_count() == 0, 5000);
    }
    server.reset();
    return check::finish();
}
