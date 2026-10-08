#include "input_map.h"

#include "keymap.h"

#include <algorithm>
#include <cmath>

namespace broremote::view {

Rect letterbox(float out_w, float out_h, uint32_t stream_w, uint32_t stream_h) {
    Rect r;
    if (out_w <= 0 || out_h <= 0 || stream_w == 0 || stream_h == 0) return r;
    const float scale = std::min(out_w / float(stream_w), out_h / float(stream_h));
    r.w = float(stream_w) * scale;
    r.h = float(stream_h) * scale;
    r.x = (out_w - r.w) / 2;
    r.y = (out_h - r.h) / 2;
    return r;
}

bool InputMapper::to_stream(float wx, float wy, const ViewGeometry& g, float& sx, float& sy) {
    const Rect r = letterbox(g.window_w, g.window_h, g.stream_w, g.stream_h);
    if (r.w <= 0 || r.h <= 0) return false;
    sx = std::clamp((wx - r.x) * float(g.stream_w) / r.w, 0.0f, float(g.stream_w - 1));
    sy = std::clamp((wy - r.y) * float(g.stream_h) / r.h, 0.0f, float(g.stream_h - 1));
    return true;
}

void InputMapper::motion(float wx, float wy, const ViewGeometry& g, std::vector<InputEvent>& out) {
    float sx = 0, sy = 0;
    if (!to_stream(wx, wy, g, sx, sy)) return;
    if (have_pos_ && sx == last_x_ && sy == last_y_) return;
    have_pos_ = true;
    last_x_ = sx;
    last_y_ = sy;
    out.push_back(InputEvent::motion(sx, sy));
}

void InputMapper::handle(const SDL_Event& e, const ViewGeometry& g, std::vector<InputEvent>& out) {
    switch (e.type) {
        case SDL_EVENT_KEY_DOWN: {
            if (e.key.repeat) return;  // the server repeats keys itself
            const uint32_t code = evdev_key(e.key.scancode);
            if (code && keys_.insert(code).second) out.push_back(InputEvent::key(code, true));
            return;
        }
        case SDL_EVENT_KEY_UP: {
            const uint32_t code = evdev_key(e.key.scancode);
            // Only what was pressed here: a key held when the window got
            // focus was never sent down.
            if (code && keys_.erase(code)) out.push_back(InputEvent::key(code, false));
            return;
        }
        case SDL_EVENT_MOUSE_MOTION: motion(e.motion.x, e.motion.y, g, out); return;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            const uint32_t code = evdev_button(e.button.button);
            if (!code || g.stream_w == 0) return;
            const bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            // The click lands where it happened, even if no motion said so.
            motion(e.button.x, e.button.y, g, out);
            if (down ? buttons_.insert(code).second : buttons_.erase(code) > 0) {
                out.push_back(InputEvent::button(code, down));
            }
            return;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            if (g.stream_w == 0) return;
            motion(e.wheel.mouse_x, e.wheel.mouse_y, g, out);
            // SDL: +x right, +y away from the user (scroll up). The protocol
            // is libinput's: +y down. Fractions (precise touchpads) add up.
            wheel_x_ += e.wheel.x * 120.0f;
            wheel_y_ += -e.wheel.y * 120.0f;
            const float dx = std::trunc(wheel_x_), dy = std::trunc(wheel_y_);
            wheel_x_ -= dx;
            wheel_y_ -= dy;
            if (dx != 0 || dy != 0) out.push_back(InputEvent::wheel(int32_t(dx), int32_t(dy)));
            return;
        }
        case SDL_EVENT_WINDOW_FOCUS_LOST: release_all(out); return;
        default: return;
    }
}

void InputMapper::release_all(std::vector<InputEvent>& out) {
    for (uint32_t k : keys_) out.push_back(InputEvent::key(k, false));
    for (uint32_t b : buttons_) out.push_back(InputEvent::button(b, false));
    keys_.clear();
    buttons_.clear();
    wheel_x_ = wheel_y_ = 0;
}

}  // namespace broremote::view
