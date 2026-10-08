// The codec factory: the single place backends register. A backend that is
// not compiled in (or finds no hardware) is simply absent from the lists.
#include "codec_backends.h"

#include <algorithm>

namespace broremote {

const char* codec_name(Codec c) noexcept {
    switch (c) {
        case Codec::Raw: return "raw";
        case Codec::H264: return "h264";
        case Codec::HEVC: return "hevc";
        case Codec::AV1: return "av1";
    }
    return "unknown";
}

std::optional<Codec> parse_codec(std::string_view name) noexcept {
    for (Codec c : {Codec::Raw, Codec::H264, Codec::HEVC, Codec::AV1}) {
        if (name == codec_name(c)) return c;
    }
    if (name == "h265") return Codec::HEVC;
    return std::nullopt;
}

bool codec_known(uint8_t value) noexcept { return value <= uint8_t(Codec::AV1); }

namespace {

[[maybe_unused]] bool contains(const std::vector<Codec>& v, Codec c) { return std::find(v.begin(), v.end(), c) != v.end(); }

std::string unavailable(Codec c, const char* what) {
    return std::string(codec_name(c)) + " " + what + " is not available in this build or on this machine";
}

}  // namespace

std::vector<Codec> available_encoders() {
    std::vector<Codec> out{Codec::Raw};
#if defined(BROREMOTE_HAVE_VAAPI)
    for (Codec c : detail::vaapi_encoders()) out.push_back(c);
#endif
    return out;
}

std::vector<Codec> available_decoders() {
    std::vector<Codec> out{Codec::Raw};
#if defined(BROREMOTE_HAVE_MF)
    for (Codec c : detail::mf_decoders()) out.push_back(c);
#endif
    return out;
}

std::unique_ptr<Encoder> create_encoder(Codec c, const EncoderConfig& config, std::string* err) {
    if (config.width == 0 || config.height == 0) {
        if (err) *err = "encoder size is empty";
        return nullptr;
    }
    if (c == Codec::Raw) return detail::create_raw_encoder(config, err);
#if defined(BROREMOTE_HAVE_VAAPI)
    if (contains(detail::vaapi_encoders(), c)) return detail::create_vaapi_encoder(c, config, err);
#endif
    if (err) *err = unavailable(c, "encoding");
    return nullptr;
}

std::unique_ptr<Decoder> create_decoder(Codec c, std::string* err) {
    if (c == Codec::Raw) return detail::create_raw_decoder(err);
#if defined(BROREMOTE_HAVE_MF)
    if (contains(detail::mf_decoders(), c)) return detail::create_mf_decoder(c, err);
    if (err) {
        const std::string why = detail::mf_unavailable_reason(c);
        *err = unavailable(c, "decoding") + (why.empty() ? "" : ": " + why);
        return nullptr;
    }
#endif
    if (err) *err = unavailable(c, "decoding");
    return nullptr;
}

}  // namespace broremote
