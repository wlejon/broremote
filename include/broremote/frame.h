#pragma once
// The data that moves through broremote: frames the host submits, the packets
// an encoder makes of them and the pictures a decoder makes of those (all
// brovideo's types), the input that comes back, and the pointer state.

#include <brovideo/frame.h>

#include <cstdint>
#include <string>
#include <vector>

namespace broremote {

// ---- frames, packets, pictures (brovideo) -----------------------------------------

// One composited frame: a dmabuf with its acquire fence, or CPU RGBA8 /
// BGRA8 rows. Nothing in it is owned: the fds and the memory belong to the
// host, which keeps them valid until the server calls the frame's release
// callback.
using brovideo::DmabufPlane;
using brovideo::Frame;
using brovideo::PixelFormat;
// One encoded picture; a keyframe carries its parameter sets in band.
using EncodedPacket = brovideo::Packet;
// A decoded picture (the viewer asks for CPU memory).
using DecodedFrame = brovideo::Picture;

// ---- input -------------------------------------------------------------------------

// Input in the server's terms, so the host can inject it as if it came from
// its own devices.
enum class InputKind : uint8_t {
    Key = 1,            // code: Linux evdev KEY_*; pressed
    PointerMotion = 2,  // x, y: absolute position in stream pixels
    Button = 3,         // code: evdev BTN_*; pressed
    Wheel = 4,          // wheel_x, wheel_y: 120ths of a detent; +x right, +y down (libinput's sense)
};

struct InputEvent {
    InputKind kind = InputKind::Key;
    uint32_t code = 0;
    bool pressed = false;
    float x = 0.0f;
    float y = 0.0f;
    int32_t wheel_x = 0;
    int32_t wheel_y = 0;

    static InputEvent key(uint32_t code, bool pressed) {
        InputEvent e;
        e.kind = InputKind::Key;
        e.code = code;
        e.pressed = pressed;
        return e;
    }
    static InputEvent motion(float x, float y) {
        InputEvent e;
        e.kind = InputKind::PointerMotion;
        e.x = x;
        e.y = y;
        return e;
    }
    static InputEvent button(uint32_t code, bool pressed) {
        InputEvent e;
        e.kind = InputKind::Button;
        e.code = code;
        e.pressed = pressed;
        return e;
    }
    static InputEvent wheel(int32_t dx, int32_t dy) {
        InputEvent e;
        e.kind = InputKind::Wheel;
        e.wheel_x = dx;
        e.wheel_y = dy;
        return e;
    }
    bool operator==(const InputEvent&) const = default;
};

// ---- pointer -----------------------------------------------------------------------

struct CursorState {
    bool visible = true;
    int32_t x = 0;  // pointer position in stream pixels
    int32_t y = 0;
    uint32_t hotspot_x = 0;
    uint32_t hotspot_y = 0;
    std::string shape = "default";  // CSS cursor name; the image itself is a later minor
    bool operator==(const CursorState&) const = default;
};

}  // namespace broremote
