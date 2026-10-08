#pragma once
// Encoders and decoders, and the factory that is the single place backends
// register. Codec::Raw is built in everywhere; the VA-API encoder (Linux) and
// the Media Foundation decoder (Windows) join the factory behind their build
// options, and nothing else changes when one is absent.

#include "broremote/frame.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace broremote {

enum class Codec : uint8_t { Raw = 0, H264 = 1, HEVC = 2, AV1 = 3 };

[[nodiscard]] const char* codec_name(Codec) noexcept;  // "raw", "h264", "hevc", "av1"
[[nodiscard]] std::optional<Codec> parse_codec(std::string_view name) noexcept;
[[nodiscard]] bool codec_known(uint8_t value) noexcept;

// What an encoder is created for. An encoder serves one size: the server
// makes a new one when the frame size (or the codec, or the bitrate) changes.
struct EncoderConfig {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 60;
    uint32_t bitrate_kbps = 20000;
};

class Encoder {
public:
    virtual ~Encoder() = default;
    // Converts and encodes one frame of the configured size. Calls `release`
    // exactly once, as soon as the frame's memory is no longer read (before
    // the encode itself finishes), on success and on failure alike. The first
    // packet of an encoder, and every packet with `force_keyframe`, is a
    // keyframe. False (with *err) on failure.
    virtual bool encode(const Frame& frame, bool force_keyframe, const std::function<void()>& release,
                        EncodedPacket& out, std::string* err) = 0;
};

class Decoder {
public:
    virtual ~Decoder() = default;
    // Decodes one packet. False (with *err) when the bitstream is bad or the
    // decoder lost sync (a predicted frame with no reference): the viewer
    // then requests a keyframe. `out.ready` is false when the decoder needs
    // more input before it has a picture.
    virtual bool decode(std::span<const uint8_t> bitstream, DecodedFrame& out, std::string* err) = 0;
};

std::unique_ptr<Encoder> create_encoder(Codec, const EncoderConfig&, std::string* err);
std::unique_ptr<Decoder> create_decoder(Codec, std::string* err);
// The codecs this build (and this machine) can encode / decode, Raw included.
std::vector<Codec> available_encoders();
std::vector<Codec> available_decoders();

}  // namespace broremote
