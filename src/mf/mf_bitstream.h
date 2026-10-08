#pragma once
// Just enough bitstream parsing for the decoder to know whether a packet can
// start decoding: Annex B NAL types for H.264 / HEVC, OBU and frame headers
// for AV1. The MFTs conceal a predicted frame with no reference instead of
// failing it, so the decoder checks this itself to report a lost sync.

#include "broremote/codec.h"

#include <cstdint>
#include <span>

namespace broremote::mf {

// True when the packet holds a random access point the decoder can start at:
// an H.264 IDR slice, an HEVC IRAP picture (IDR, CRA, BLA), or an AV1
// temporal unit with a sequence header and a key frame.
bool is_keyframe(Codec codec, std::span<const uint8_t> packet);

}  // namespace broremote::mf
