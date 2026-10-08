#pragma once
// Decoded pictures as plain RGBA, for checking them against the test pattern
// and for writing them out: NV12 is converted with the BT.709 limited-range
// matrix the encoders signal (chroma taken from the 2x2 block a pixel is in).

#include "broremote/frame.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace broremote::tools {

// `f` as tightly packed RGBA (R G B A per pixel), whatever its format.
inline std::vector<uint8_t> to_rgba(const DecodedFrame& f) {
    std::vector<uint8_t> out(size_t(f.width) * f.height * 4);
    if (f.format == PixelFormat::RGBA8) {
        for (uint32_t y = 0; y < f.height; ++y) {
            std::copy_n(f.data.data() + size_t(y) * f.stride, size_t(f.width) * 4, out.data() + size_t(y) * f.width * 4);
        }
        return out;
    }
    const auto clamp = [](double v) { return uint8_t(std::clamp(v + 0.5, 0.0, 255.0)); };
    for (uint32_t y = 0; y < f.height; ++y) {
        const uint8_t* yrow = f.data.data() + size_t(y) * f.stride;
        const uint8_t* uvrow = f.data.data() + f.uv_offset + size_t(y / 2) * f.stride;
        uint8_t* o = out.data() + size_t(y) * f.width * 4;
        for (uint32_t x = 0; x < f.width; ++x) {
            const double l = 1.164383 * (double(yrow[x]) - 16.0);
            const double u = double(uvrow[(x & ~1u)]) - 128.0;
            const double v = double(uvrow[(x & ~1u) + 1]) - 128.0;
            o[x * 4 + 0] = clamp(l + 1.792741 * v);
            o[x * 4 + 1] = clamp(l - 0.213249 * u - 0.532909 * v);
            o[x * 4 + 2] = clamp(l + 2.112402 * u);
            o[x * 4 + 3] = 255;
        }
    }
    return out;
}

// PSNR in dB between two RGBA pictures of the same size, over R, G and B.
inline double psnr_rgba(const uint8_t* a, const uint8_t* b, uint32_t width, uint32_t height) {
    double se = 0;
    const size_t n = size_t(width) * height;
    for (size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            const double d = double(a[i * 4 + c]) - double(b[i * 4 + c]);
            se += d * d;
        }
    }
    const double mse = se / double(n * 3);
    return mse <= 1e-10 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

// PSNR in dB of an NV12 picture's luma against the ideal BT.709
// limited-range luma of an RGBA source of the same size.
inline double psnr_luma(const DecodedFrame& f, const uint8_t* rgba) {
    if (f.format != PixelFormat::NV12) return 0;
    double se = 0;
    for (uint32_t y = 0; y < f.height; ++y) {
        const uint8_t* row = f.data.data() + size_t(y) * f.stride;
        const uint8_t* src = rgba + size_t(y) * f.width * 4;
        for (uint32_t x = 0; x < f.width; ++x) {
            const double ideal =
                16.0 + 219.0 * (0.2126 * src[x * 4] + 0.7152 * src[x * 4 + 1] + 0.0722 * src[x * 4 + 2]) / 255.0;
            const double d = double(row[x]) - ideal;
            se += d * d;
        }
    }
    const double mse = se / (double(f.width) * f.height);
    return mse <= 1e-10 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

}  // namespace broremote::tools
