#pragma once
// The codec backends the factory (codec_factory.cpp) knows. Each optional
// backend is compiled in behind its BROREMOTE_HAVE_* definition and declares
// its entry points here; the factory is the only caller.

#include "broremote/codec.h"

namespace broremote::detail {

// Built in everywhere: CPU RGBA frames, run-length coded, with predicted
// (XOR-delta) frames between keyframes. Format: docs/protocol.md.
std::unique_ptr<Encoder> create_raw_encoder(const EncoderConfig&, std::string* err);
std::unique_ptr<Decoder> create_raw_decoder(std::string* err);

#if defined(BROREMOTE_HAVE_VAAPI)
// src/vaapi: the codecs this machine's VA driver can encode, and an encoder.
std::vector<Codec> vaapi_encoders();
std::unique_ptr<Encoder> create_vaapi_encoder(Codec, const EncoderConfig&, std::string* err);
#endif

#if defined(BROREMOTE_HAVE_MF)
// src/mf: the codecs this machine's Media Foundation can decode, and a decoder.
std::vector<Codec> mf_decoders();
std::unique_ptr<Decoder> create_mf_decoder(Codec, std::string* err);
// Why the probe found a codec unusable (empty when it is usable).
std::string mf_unavailable_reason(Codec);
#endif

}  // namespace broremote::detail
