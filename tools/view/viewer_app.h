#pragma once
// broremote-view's window: an SDL3 window and renderer showing the newest
// decoded picture scaled into the window with its aspect kept (letterboxed),
// sending keyboard, pointer and wheel input back through the Session. The
// render thread only uploads and draws; connecting and decoding happen on
// the Session's threads.

#include "input_map.h"
#include "session.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace broremote::view {

struct ViewerOptions {
    SessionOptions session;
    uint32_t frames = 0;        // exit after this many pictures were displayed (0: until closed)
    double timeout_s = 0;       // with frames: give up (exit 1) after this long (0: never)
    std::string dump_png;       // write the last displayed picture here on exit
    bool check_pattern = false; // compare the last picture with broremote serve-test's pattern
    bool vsync = true;
    bool fullscreen = false;
    bool hidden = false;        // no visible window (tests)
    int width = 0, height = 0;  // the initial window size (0: fit the stream once it is known)
};

class Viewer {
public:
    explicit Viewer(ViewerOptions options);
    ~Viewer();
    Viewer(const Viewer&) = delete;
    Viewer& operator=(const Viewer&) = delete;

    // Opens the window and starts connecting. False with *err when SDL fails.
    bool init(std::string* err);
    // Handles pending events (waiting up to timeout_ms for one), takes and
    // draws the newest picture. False once the viewer should exit.
    bool step(int timeout_ms);
    // The closing report (stderr), --dump-png and --check-pattern. The exit code.
    int finish();

    // For tests.
    [[nodiscard]] SDL_Window* window() const { return window_; }
    [[nodiscard]] Session& session() { return *session_; }
    [[nodiscard]] uint64_t displayed() const { return displayed_; }
    [[nodiscard]] const InputMapper& mapper() const { return mapper_; }
    [[nodiscard]] const DecodedFrame* frame() const { return have_frame_ ? &frame_ : nullptr; }

private:
    void handle(const SDL_Event& e);
    void upload();
    void draw();
    void update_title(bool force);
    void fit_window_to_stream(uint32_t w, uint32_t h);
    void toggle_fullscreen();
    bool check_screen();
    ViewGeometry geometry() const;

    ViewerOptions opt_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    uint32_t tex_w_ = 0, tex_h_ = 0;
    PixelFormat tex_format_ = PixelFormat::RGBA8;
    uint32_t wake_type_ = 0;
    std::atomic<bool> wake_pending_{false};
    std::unique_ptr<Session> session_;
    InputMapper mapper_;
    std::vector<InputEvent> events_;

    DecodedFrame frame_;  // the picture on screen
    FrameInfo info_;
    bool have_frame_ = false;
    bool need_draw_ = true;
    bool quit_ = false;
    bool timed_out_ = false;
    bool fitted_ = false;
    bool swallow_enter_ = false;
    uint64_t displayed_ = 0;
    std::vector<double> latency_ms_;  // packet received -> presented

    SessionStatus shown_status_;
    std::string title_;
    Clock::time_point start_, title_time_;
    uint64_t title_displayed_ = 0;
};

// The whole program after argument parsing: returns the exit code.
int run_viewer(const ViewerOptions& options);

}  // namespace broremote::view
