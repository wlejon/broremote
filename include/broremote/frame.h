#pragma once
// The data that moves through broremote: frames the host submits, the packets
// an encoder makes of them, the pictures a decoder makes of those, the input
// that comes back, and the pointer state.

#include <cstdint>
#include <string>
#include <vector>

namespace broremote {

// ---- host frames -----------------------------------------------------------------

struct DmabufPlane {
    int fd = -1;
    uint32_t offset = 0;
    uint32_t pitch = 0;
};

// One composited frame. Either a dmabuf (plane_count > 0: the GPU path) or a
// CPU frame (plane_count == 0): `cpu` points at `height` rows of RGBA8 pixels
// (bytes R, G, B, A in memory order), `cpu_stride` bytes apart (0: width * 4).
// Nothing here is owned: the fds and the memory belong to the host, which
// keeps them valid until the server calls the frame's release callback.
struct Frame {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t drm_format = 0;    // DRM fourcc of a dmabuf frame, e.g. XRGB8888 (ignored for CPU frames)
    uint64_t modifier = 0;      // DRM format modifier
    uint32_t plane_count = 0;   // 0 => CPU frame
    DmabufPlane planes[4];
    int acquire_fence_fd = -1;  // sync_file; the content is ready when it signals (-1: ready)
    const uint8_t* cpu = nullptr;
    uint32_t cpu_stride = 0;
    int64_t pts_ns = 0;

    [[nodiscard]] bool is_cpu() const noexcept { return plane_count == 0; }
    [[nodiscard]] uint32_t cpu_row_bytes() const noexcept { return cpu_stride ? cpu_stride : width * 4; }
};

// ---- codec output ------------------------------------------------------------------

// One encoded picture: Annex B for H.264/HEVC, a temporal unit of OBUs for
// AV1, the Raw format (docs/protocol.md) for Codec::Raw. A keyframe carries
// its parameter sets in band, so a decoder needs nothing but the packets.
struct EncodedPacket {
    std::vector<uint8_t> data;
    bool keyframe = false;
    int64_t pts_ns = 0;
};

enum class PixelFormat : uint8_t {
    RGBA8 = 0,  // `height` rows of `stride` bytes, R G B A
    NV12 = 1,   // Y: `height` rows of `stride` bytes; then at `uv_offset`,
                // (height + 1) / 2 rows of `stride` bytes of interleaved U V
};

// A decoded picture in CPU memory.
struct DecodedFrame {
    bool ready = false;  // false: the decoder consumed the input but has no picture yet
    uint32_t width = 0;
    uint32_t height = 0;
    PixelFormat format = PixelFormat::RGBA8;
    uint32_t stride = 0;
    uint32_t uv_offset = 0;  // NV12 only
    std::vector<uint8_t> data;
};

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
