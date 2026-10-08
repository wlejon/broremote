#pragma once
// SDL input events to broremote InputEvents: keys as evdev codes, the
// pointer in stream pixels through the letterboxed picture, buttons as evdev
// BTN_* codes, the wheel in 120ths of a detent (+y down). It remembers what
// it pressed, so focus loss releases exactly that.

#include "broremote/frame.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <set>
#include <vector>

namespace broremote::view {

// The picture's rectangle inside an area of out_w x out_h (aspect kept,
// centred, bars on the long sides).
struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
};
Rect letterbox(float out_w, float out_h, uint32_t stream_w, uint32_t stream_h);

// The window in the units SDL's mouse events use, and the stream's size.
struct ViewGeometry {
    float window_w = 0, window_h = 0;
    uint32_t stream_w = 0, stream_h = 0;
};

class InputMapper {
public:
    // Appends what `e` means to the server (nothing for most events).
    // Pointer events with no stream yet (stream size 0) are dropped.
    void handle(const SDL_Event& e, const ViewGeometry& g, std::vector<InputEvent>& out);
    // Releases every key and button this mapper pressed (focus loss).
    void release_all(std::vector<InputEvent>& out);

    // Window coordinates to stream pixels, clamped to the picture.
    static bool to_stream(float wx, float wy, const ViewGeometry& g, float& sx, float& sy);

    [[nodiscard]] size_t held_keys() const { return keys_.size(); }
    [[nodiscard]] size_t held_buttons() const { return buttons_.size(); }

private:
    void motion(float wx, float wy, const ViewGeometry& g, std::vector<InputEvent>& out);

    std::set<uint32_t> keys_;
    std::set<uint32_t> buttons_;
    float wheel_x_ = 0, wheel_y_ = 0;  // fractions of a 120th not yet sent
    bool have_pos_ = false;
    float last_x_ = 0, last_y_ = 0;
};

}  // namespace broremote::view
