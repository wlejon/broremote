// broremote-view's input path, end to end in one process: an in-process
// Server (Raw codec) streams a 640x360 picture to the real Viewer (SDL's
// offscreen video driver, a 1000x1000 window, so the picture is letterboxed
// with bars above and below), SDL events are injected with SDL_PushEvent,
// and the InputEvents the server drains must be exactly the expected ones:
// evdev key and button codes, pointer positions mapped from the window to
// stream pixels (clamped at the picture's edges), wheel steps in 120ths with
// +y down and fractions carried, no auto-repeat, the fullscreen hotkey not
// forwarded, and every held key and button released on focus loss. Then the
// stream changes size and the mapping follows it.
#include "broremote/server.h"
#include "check.h"
#include "keymap.h"
#include "viewer_app.h"

#include <atomic>
#include <memory>
#include <thread>

using namespace broremote;

namespace {

// Submits frames of the current size until stopped, from a small pool.
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
                Frame f;
                f.width = w;
                f.height = h;
                f.cpu = b.px.data();
                server_.submit(f, [&b] { b.busy = false; });
                ++n;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
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

SDL_Window* g_window = nullptr;

void push(SDL_Event e) {
    SDL_PushEvent(&e);
}

SDL_Event motion(float x, float y) {
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = SDL_GetWindowID(g_window);
    e.motion.x = x;
    e.motion.y = y;
    return e;
}

SDL_Event button(uint8_t b, bool down, float x, float y) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.windowID = SDL_GetWindowID(g_window);
    e.button.button = b;
    e.button.down = down;
    e.button.x = x;
    e.button.y = y;
    return e;
}

SDL_Event wheel(float dx, float dy, float x, float y) {
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.windowID = SDL_GetWindowID(g_window);
    e.wheel.x = dx;
    e.wheel.y = dy;
    e.wheel.mouse_x = x;
    e.wheel.mouse_y = y;
    return e;
}

SDL_Event key(SDL_Scancode sc, bool down, SDL_Keymod mod = SDL_KMOD_NONE, bool repeat = false) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.windowID = SDL_GetWindowID(g_window);
    e.key.scancode = sc;
    e.key.down = down;
    e.key.repeat = repeat;
    e.key.mod = mod;
    return e;
}

SDL_Event focus_lost() {
    SDL_Event e{};
    e.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    e.window.windowID = SDL_GetWindowID(g_window);
    return e;
}

std::string show(const InputEvent& e) {
    char buf[96];
    switch (e.kind) {
        case InputKind::Key: std::snprintf(buf, sizeof buf, "key %u %s", e.code, e.pressed ? "down" : "up"); break;
        case InputKind::Button:
            std::snprintf(buf, sizeof buf, "button 0x%x %s", e.code, e.pressed ? "down" : "up");
            break;
        case InputKind::PointerMotion: std::snprintf(buf, sizeof buf, "motion %.2f,%.2f", e.x, e.y); break;
        case InputKind::Wheel: std::snprintf(buf, sizeof buf, "wheel %d,%d", e.wheel_x, e.wheel_y); break;
    }
    return buf;
}

// Steps the viewer until the server has drained `n` events (or 5 s pass).
std::vector<InputEvent> collect(view::Viewer& v, Server& s, size_t n) {
    std::vector<InputEvent> got;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (got.size() < n && std::chrono::steady_clock::now() < until) {
        v.step(5);
        s.drain_input(got);
    }
    // Anything extra that is on its way.
    for (int i = 0; i < 20; ++i) {
        v.step(5);
        s.drain_input(got);
    }
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

}  // namespace

int main() {
    check::watchdog(120);
    // A window with no display: deterministic size, no real input mixed in.
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");

    check::phase("server");
    ServerConfig cfg;
    cfg.socket_name = "test_view_" + std::to_string(int64_t(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000));
    cfg.codecs = {Codec::Raw};
    cfg.fps = 100;
    std::string err;
    auto server = Server::create(cfg, &err);
    CHECK(server != nullptr);
    if (!server) {
        std::printf("   %s\n", err.c_str());
        return check::finish();
    }
    Feeder feeder(*server);

    check::phase("viewer shows the stream");
    view::ViewerOptions o;
    o.session.target.socket = cfg.socket_name;
    o.hidden = true;
    o.vsync = false;
    o.width = 1000;
    o.height = 1000;
    {
        view::Viewer v(o);
        CHECK(v.init(&err));
        if (!v.window()) {
            std::printf("   %s\n", err.c_str());
            return check::finish();
        }
        g_window = v.window();
        int ww = 0, wh = 0;
        SDL_GetWindowSize(g_window, &ww, &wh);
        CHECK_EQ(ww, 1000);
        CHECK_EQ(wh, 1000);
        WAIT((v.step(5), v.displayed() >= 2), 10000);
        CHECK(v.frame() && v.frame()->width == 640 && v.frame()->height == 360);
        std::vector<InputEvent> scratch;
        server->drain_input(scratch);

        // 640x360 in 1000x1000: scale 1.5625, the picture 1000x562.5 at y 218.75.
        check::phase("pointer: window to stream pixels");
        push(motion(500, 500));          // the centre
        push(motion(0, 218.75f));        // the top-left corner
        push(motion(1000, 781.25f));     // past the bottom-right: clamped
        push(motion(500, 100));          // in the bar above: clamped to the top row
        push(motion(500, 100));          // the same place again: nothing new
        push(motion(250.0f, 296.875f));  // a quarter across, a quarter down (sub-pixel exact)
        expect(collect(v, *server, 5), {InputEvent::motion(320, 180), InputEvent::motion(0, 0),
                                        InputEvent::motion(639, 359), InputEvent::motion(320, 0),
                                        InputEvent::motion(160, 50)});

        check::phase("buttons: evdev codes, the click where it happened");
        push(button(SDL_BUTTON_LEFT, true, 250.0f, 296.875f));  // already there: no motion first
        push(button(SDL_BUTTON_RIGHT, true, 500, 500));         // moves first
        push(button(SDL_BUTTON_RIGHT, false, 500, 500));
        push(button(SDL_BUTTON_X1, true, 500, 500));
        push(button(SDL_BUTTON_X1, false, 500, 500));
        push(button(SDL_BUTTON_MIDDLE, false, 500, 500));  // never pressed here: not sent
        expect(collect(v, *server, 6),
               {InputEvent::button(view::kBtnLeft, true), InputEvent::motion(320, 180),
                InputEvent::button(view::kBtnRight, true), InputEvent::button(view::kBtnRight, false),
                InputEvent::button(view::kBtnSide, true), InputEvent::button(view::kBtnSide, false)});

        check::phase("wheel: 120ths, +y down, fractions carried");
        push(wheel(0, 1, 500, 500));       // one detent up
        push(wheel(0, -2, 500, 500));      // two down
        push(wheel(-0.5f, 0, 500, 500));   // half a detent left, twice
        push(wheel(-0.5f, 0, 500, 500));
        push(wheel(0, 0.004f, 500, 500));  // under a 120th: nothing yet
        push(wheel(0, 0.005f, 500, 500));  // together just over one 120th (up)
        expect(collect(v, *server, 5), {InputEvent::wheel(0, -120), InputEvent::wheel(0, 240),
                                        InputEvent::wheel(-60, 0), InputEvent::wheel(-60, 0),
                                        InputEvent::wheel(0, -1)});

        check::phase("keys: evdev codes, no auto-repeat, the hotkey kept local");
        push(key(SDL_SCANCODE_A, true));
        push(key(SDL_SCANCODE_A, true, SDL_KMOD_NONE, true));  // auto-repeat: dropped
        push(key(SDL_SCANCODE_LSHIFT, true));
        push(key(SDL_SCANCODE_F5, true));
        push(key(SDL_SCANCODE_KP_ENTER, true));
        push(key(SDL_SCANCODE_A, false));
        push(key(SDL_SCANCODE_SLASH, false));  // never pressed here: not sent
        // Ctrl+Alt+Enter toggles fullscreen; the Enter goes nowhere (the
        // modifiers themselves are ordinary keys and do go).
        push(key(SDL_SCANCODE_RETURN, true, SDL_Keymod(SDL_KMOD_LCTRL | SDL_KMOD_LALT)));
        push(key(SDL_SCANCODE_RETURN, false, SDL_Keymod(SDL_KMOD_LCTRL | SDL_KMOD_LALT)));
        push(key(SDL_SCANCODE_RETURN, true, SDL_Keymod(SDL_KMOD_LCTRL | SDL_KMOD_LALT)));  // and back
        push(key(SDL_SCANCODE_RETURN, false, SDL_Keymod(SDL_KMOD_LCTRL | SDL_KMOD_LALT)));
        push(key(SDL_SCANCODE_RETURN, true));
        push(key(SDL_SCANCODE_RETURN, false));
        expect(collect(v, *server, 7),
               {InputEvent::key(30, true), InputEvent::key(42, true), InputEvent::key(63, true),
                InputEvent::key(96, true), InputEvent::key(30, false), InputEvent::key(28, true),
                InputEvent::key(28, false)});

        check::phase("focus loss releases what is held");
        push(key(SDL_SCANCODE_RCTRL, true));
        push(focus_lost());
        push(key(SDL_SCANCODE_RCTRL, false));  // released already: not again
        expect(collect(v, *server, 6),
               {InputEvent::key(97, true), InputEvent::key(42, false), InputEvent::key(63, false),
                InputEvent::key(96, false), InputEvent::key(97, false), InputEvent::button(view::kBtnLeft, false)});
        CHECK_EQ(v.mapper().held_keys(), size_t(0));
        CHECK_EQ(v.mapper().held_buttons(), size_t(0));

        check::phase("a new stream size: the mapping follows");
        feeder.set_size(800, 600);
        WAIT((v.step(5), v.frame() && v.frame()->width == 800), 10000);
        CHECK(v.frame() && v.frame()->height == 600);
        server->drain_input(scratch);
        // 800x600 in 1000x1000: scale 1.25, the picture at y 125.
        push(motion(500, 500));
        push(motion(1000, 875));
        expect(collect(v, *server, 2), {InputEvent::motion(400, 300), InputEvent::motion(799, 599)});

        check::phase("closing");
        CHECK_EQ(v.finish(), 0);
    }
    server.reset();
    return check::finish();
}
