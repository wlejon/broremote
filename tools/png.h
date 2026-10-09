#pragma once
// A minimal PNG writer (RGBA8, stored deflate blocks: large files, no
// dependency) for `broremote probe --dump-png`.

#include <cstdint>
#include <string>

namespace broremote::tools {

// `rgba`: height rows of width * 4 bytes, tightly packed. False on an I/O error.
bool write_png(const std::string& path, const uint8_t* rgba, uint32_t width, uint32_t height);

}  // namespace broremote::tools
