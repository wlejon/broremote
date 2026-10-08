#pragma once
// broremote's codecs are brovideo's (../brovideo): every encoder and decoder
// comes from there, and broremote keeps only the remoting policy around them
// (which codec to use, when to send a keyframe, when to encode at all). The
// names below are brought into broremote so the protocol, the server and the
// viewer speak them directly.
//
// On the wire a codec is its brovideo value as a u8 (docs/protocol.md);
// codec_known() says which values this protocol version defines.

#include "broremote/frame.h"

#include <brovideo/brovideo.h>

#include <cstdint>

namespace broremote {

using brovideo::Codec;
using brovideo::codec_name;
using brovideo::parse_codec;
using brovideo::Decoder;
using brovideo::DecoderConfig;
using brovideo::Encoder;
using brovideo::EncoderConfig;

[[nodiscard]] inline bool codec_known(uint8_t value) noexcept { return value <= uint8_t(Codec::AV1); }

}  // namespace broremote
