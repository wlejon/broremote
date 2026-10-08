#include "broremote/protocol.h"

#include <cmath>
#include <cstring>

namespace broremote {

namespace {

std::string message(MsgType type, wire::Writer& w) { return wire::make_message(uint16_t(type), w.data()); }

bool input_kind_known(uint8_t k) { return k >= uint8_t(InputKind::Key) && k <= uint8_t(InputKind::Wheel); }

}  // namespace

const char* error_code_name(ErrorCode c) noexcept {
    switch (c) {
        case ErrorCode::None: return "none";
        case ErrorCode::BadMessage: return "bad message";
        case ErrorCode::UnknownMessage: return "unknown message";
        case ErrorCode::VersionMismatch: return "version mismatch";
        case ErrorCode::HelloRequired: return "hello required";
        case ErrorCode::NoCommonCodec: return "no common codec";
        case ErrorCode::EncoderFailed: return "encoder failed";
        case ErrorCode::ServerShutdown: return "server shutdown";
    }
    return "unknown error";
}

// ---- client -> server ---------------------------------------------------------------

std::string HelloMsg::encode() const {
    wire::Writer w;
    w.raw(std::string_view(kProtocolMagic, 4));
    w.u16(major);
    w.u16(minor);
    w.str(name);
    return message(MsgType::Hello, w);
}

bool HelloMsg::decode(std::string_view payload) {
    wire::Reader r(payload);
    std::string_view magic = r.raw(4);
    if (!r.ok() || std::memcmp(magic.data(), kProtocolMagic, 4) != 0) return false;
    major = r.u16();
    minor = r.u16();
    name = r.str_max(kMaxNameBytes);
    return r.ok();
}

std::string AckMsg::encode() const {
    wire::Writer w;
    w.varint(frame_id);
    return message(MsgType::Ack, w);
}

bool AckMsg::decode(std::string_view payload) {
    wire::Reader r(payload);
    frame_id = r.varint();
    return r.ok();
}

std::string RequestKeyframeMsg::encode() const { return wire::make_message(uint16_t(MsgType::RequestKeyframe), {}); }

bool RequestKeyframeMsg::decode(std::string_view) { return true; }

std::string InputMsg::encode() const {
    wire::Writer w;
    w.u8(uint8_t(event.kind));
    switch (event.kind) {
        case InputKind::Key:
        case InputKind::Button:
            w.varint(event.code);
            w.boolean(event.pressed);
            break;
        case InputKind::PointerMotion:
            w.f32(event.x);
            w.f32(event.y);
            break;
        case InputKind::Wheel:
            w.svarint(event.wheel_x);
            w.svarint(event.wheel_y);
            break;
    }
    return message(MsgType::Input, w);
}

bool InputMsg::decode(std::string_view payload, bool* unknown_kind) {
    if (unknown_kind) *unknown_kind = false;
    wire::Reader r(payload);
    const uint8_t kind = r.u8();
    if (!r.ok()) return false;
    if (!input_kind_known(kind)) {
        if (unknown_kind) *unknown_kind = true;
        return false;
    }
    event = InputEvent{};
    event.kind = InputKind(kind);
    switch (event.kind) {
        case InputKind::Key:
        case InputKind::Button:
            event.code = r.varint32();
            event.pressed = r.boolean();
            break;
        case InputKind::PointerMotion:
            event.x = r.f32();
            event.y = r.f32();
            if (!std::isfinite(event.x) || !std::isfinite(event.y)) r.fail();
            break;
        case InputKind::Wheel:
            event.wheel_x = r.svarint32();
            event.wheel_y = r.svarint32();
            break;
    }
    return r.ok();
}

std::string SetCodecMsg::encode() const {
    wire::Writer w;
    w.varint(codecs.size());
    for (Codec c : codecs) w.u8(uint8_t(c));
    w.varint(max_bitrate_kbps);
    return message(MsgType::SetCodec, w);
}

bool SetCodecMsg::decode(std::string_view payload) {
    wire::Reader r(payload);
    const size_t n = r.count(1);
    if (!r.ok() || n > kMaxCodecList) return false;
    codecs.clear();
    for (size_t i = 0; i < n; ++i) {
        const uint8_t c = r.u8();
        if (codec_known(c)) codecs.push_back(Codec(c));
    }
    max_bitrate_kbps = r.varint32();
    return r.ok();
}

// ---- server -> client ---------------------------------------------------------------

std::string WelcomeMsg::encode() const {
    wire::Writer w;
    w.u16(major);
    w.u16(minor);
    w.str(name);
    return message(MsgType::Welcome, w);
}

bool WelcomeMsg::decode(std::string_view payload) {
    wire::Reader r(payload);
    major = r.u16();
    minor = r.u16();
    name = r.str_max(kMaxNameBytes);
    return r.ok();
}

std::string StreamConfig::encode() const {
    wire::Writer w;
    w.varint(stream_id);
    w.u8(uint8_t(codec));
    w.varint(width);
    w.varint(height);
    w.varint(fps);
    return message(MsgType::StreamConfig, w);
}

bool StreamConfig::decode(std::string_view payload) {
    wire::Reader r(payload);
    stream_id = r.varint();
    codec = Codec(r.u8());  // an unknown codec decodes; the client reports it cannot play it
    width = uint32_t(r.varint_max(kMaxDimension));
    height = uint32_t(r.varint_max(kMaxDimension));
    fps = r.varint32();
    if (r.ok() && (width == 0 || height == 0)) r.fail();
    return r.ok();
}

std::string VideoPacket::encode_message(uint64_t stream_id, uint64_t frame_id, int64_t pts_ns, bool keyframe,
                                        std::span<const uint8_t> data) {
    wire::Writer w;
    w.data().reserve(data.size() + 32);
    w.varint(stream_id);
    w.varint(frame_id);
    w.svarint(pts_ns);
    w.u8(keyframe ? 1 : 0);
    w.bytes(data.data(), data.size());
    return message(MsgType::Video, w);
}

std::string VideoPacket::encode() const { return encode_message(stream_id, frame_id, pts_ns, keyframe, data); }

bool VideoPacket::decode(std::string_view payload) {
    wire::Reader r(payload);
    stream_id = r.varint();
    frame_id = r.varint();
    pts_ns = r.svarint();
    const uint8_t flags = r.u8();  // bits other than 0 are a later minor's: ignored
    keyframe = (flags & 1) != 0;
    data = r.bytes();
    return r.ok();
}

std::string CursorMsg::encode() const {
    wire::Writer w;
    w.boolean(state.visible);
    w.svarint(state.x);
    w.svarint(state.y);
    w.varint(state.hotspot_x);
    w.varint(state.hotspot_y);
    w.str(state.shape);
    return message(MsgType::Cursor, w);
}

bool CursorMsg::decode(std::string_view payload) {
    wire::Reader r(payload);
    state.visible = r.boolean();
    state.x = r.svarint32();
    state.y = r.svarint32();
    state.hotspot_x = r.varint32();
    state.hotspot_y = r.varint32();
    state.shape = r.str_max(kMaxShapeBytes);
    return r.ok();
}

std::string ErrorMsg::encode() const {
    wire::Writer w;
    w.u16(uint16_t(code));
    w.str(message.size() > kMaxErrorBytes ? std::string_view(message).substr(0, kMaxErrorBytes)
                                          : std::string_view(message));
    return broremote::message(MsgType::Error, w);
}

bool ErrorMsg::decode(std::string_view payload) {
    wire::Reader r(payload);
    code = ErrorCode(r.u16());  // unknown codes decode; they print as "unknown error"
    message = r.str_max(kMaxErrorBytes);
    return r.ok();
}

}  // namespace broremote
