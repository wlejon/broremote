#include "broremote/protocol.h"

#include <cmath>
#include <cstring>

namespace broremote {

namespace {

std::string message(MsgType type, wire::Writer& w) { return wire::make_message(uint16_t(type), w.data()); }

bool input_kind_known(uint8_t k) {
    return k >= uint8_t(InputKind::Key) && k <= uint8_t(InputKind::RelativeMotion);
}

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
        case ErrorCode::JoinRefused: return "join refused";
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
        case InputKind::RelativeMotion:
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
        case InputKind::RelativeMotion:
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
    if (grant) grant->write(w);
    return message(MsgType::Welcome, w);
}

bool WelcomeMsg::decode(std::string_view payload) {
    wire::Reader r(payload);
    major = r.u16();
    minor = r.u16();
    name = r.str_max(kMaxNameBytes);
    if (!r.ok()) return false;
    grant.reset();
    if (!r.at_end()) {
        brolink::lanes::Grant g;
        if (!g.read(r)) return false;
        grant = g;
    }
    return true;
}

std::string JoinMsg::encode() const {
    wire::Writer w;
    join.write(w);
    return message(MsgType::Join, w);
}

bool JoinMsg::decode(std::string_view payload) {
    wire::Reader r(payload);
    return join.read(r);
}

std::string JoinedMsg::encode() const { return wire::make_message(uint16_t(MsgType::Joined), {}); }

bool JoinedMsg::decode(std::string_view) { return true; }

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
                                        std::span<const uint8_t> data, const FrameTiming* timing) {
    wire::Writer w;
    w.data().reserve(data.size() + 48);
    w.varint(stream_id);
    w.varint(frame_id);
    w.svarint(pts_ns);
    w.u8(keyframe ? 1 : 0);
    w.bytes(data.data(), data.size());
    if (timing && timing->valid) {
        w.varint(timing->submit_us);
        w.varint(timing->queue_us);
        w.varint(timing->encode_us);
    }
    return message(MsgType::Video, w);
}

std::string VideoPacket::encode() const {
    return encode_message(stream_id, frame_id, pts_ns, keyframe, data, &timing);
}

bool VideoPacket::decode(std::string_view payload) {
    wire::Reader r(payload);
    stream_id = r.varint();
    frame_id = r.varint();
    pts_ns = r.svarint();
    const uint8_t flags = r.u8();  // bits other than 0 are a later minor's: ignored
    keyframe = (flags & 1) != 0;
    data = r.bytes();
    timing = FrameTiming{};
    // 1.1's timing: absent from a 1.0 server, whole when present.
    if (r.ok() && r.remaining() > 0) {
        timing.submit_us = r.varint();
        timing.queue_us = r.varint();
        timing.encode_us = r.varint();
        timing.valid = r.ok();
    }
    return r.ok();
}

std::string PingMsg::encode() const {
    wire::Writer w;
    w.u64(token);
    return message(MsgType::Ping, w);
}

bool PingMsg::decode(std::string_view payload) {
    wire::Reader r(payload);
    token = r.u64();
    return r.ok();
}

std::string PongMsg::encode() const {
    wire::Writer w;
    w.u64(token);
    w.u64(server_time_us);
    return message(MsgType::Pong, w);
}

bool PongMsg::decode(std::string_view payload) {
    wire::Reader r(payload);
    token = r.u64();
    server_time_us = r.u64();
    return r.ok();
}

std::string FrameSentMsg::encode() const {
    wire::Writer w;
    w.varint(frame_id);
    w.varint(wait_us);
    w.varint(write_us);
    return message(MsgType::FrameSent, w);
}

bool FrameSentMsg::decode(std::string_view payload) {
    wire::Reader r(payload);
    frame_id = r.varint();
    wait_us = r.varint();
    write_us = r.varint();
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
    w.boolean(state.locked);  // 1.4
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
    state.locked = false;
    if (r.ok() && !r.at_end()) state.locked = r.boolean();  // 1.4
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

// ---- the audio lane -------------------------------------------------------------------

namespace {

void write_format(wire::Writer& w, const audio::Format& f) {
    w.varint(f.rate);
    w.u8(uint8_t(f.channels));
    w.u8(uint8_t(f.sample));
}

audio::Format read_format(wire::Reader& r) {
    audio::Format f;
    f.rate = r.varint32();
    f.channels = r.u8();
    f.sample = audio::SampleFormat(r.u8());
    if (r.ok() && !f.valid()) r.fail();
    return f;
}

std::string_view clip(const std::string& s, size_t limit) {
    return s.size() > limit ? std::string_view(s).substr(0, limit) : std::string_view(s);
}

}  // namespace

std::string AudioStartMsg::encode() const {
    wire::Writer w;
    w.str(clip(source, kMaxNameBytes));
    w.boolean(playback);
    write_format(w, playback_format);
    w.boolean(mic);
    write_format(w, mic_format);
    return message(MsgType::AudioStart, w);
}

bool AudioStartMsg::decode(std::string_view payload) {
    wire::Reader r(payload);
    source = r.str_max(kMaxNameBytes);
    playback = r.boolean();
    playback_format = read_format(r);
    mic = r.boolean();
    mic_format = read_format(r);
    return r.ok();
}

std::string AudioStartedMsg::encode() const {
    wire::Writer w;
    w.boolean(playback);
    write_format(w, playback_format);
    w.boolean(mic);
    write_format(w, mic_format);
    w.str(clip(mic_node, kMaxNameBytes));
    w.str(clip(message, kMaxErrorBytes));
    return broremote::message(MsgType::AudioStarted, w);
}

bool AudioStartedMsg::decode(std::string_view payload) {
    wire::Reader r(payload);
    playback = r.boolean();
    playback_format = read_format(r);
    mic = r.boolean();
    mic_format = read_format(r);
    mic_node = r.str_max(kMaxNameBytes);
    message = r.str_max(kMaxErrorBytes);
    return r.ok();
}

std::string AudioDataMsg::encode_message(MsgType type, uint64_t seq, uint64_t capture_us, std::string_view pcm) {
    wire::Writer w;
    w.data().reserve(pcm.size() + 24);
    w.varint(seq);
    w.u64(capture_us);
    w.str(pcm);
    return message(type, w);
}

std::string AudioDataMsg::encode() const { return encode_message(type, seq, capture_us, pcm); }

bool AudioDataMsg::decode(std::string_view payload, MsgType as) {
    wire::Reader r(payload);
    type = as;
    seq = r.varint();
    capture_us = r.u64();
    pcm = r.str_max(kMaxAudioBytes);
    return r.ok();
}

std::string AudioControlMsg::encode() const {
    wire::Writer w;
    w.boolean(playback_muted);
    w.boolean(mic_muted);
    return message(MsgType::AudioControl, w);
}

bool AudioControlMsg::decode(std::string_view payload) {
    wire::Reader r(payload);
    playback_muted = r.boolean();
    mic_muted = r.boolean();
    return r.ok();
}

std::string AudioStatsMsg::encode() const {
    wire::Writer w;
    w.boolean(mic_valid);
    w.u64(mic_capture_us);
    w.u64(mic_out_us);
    w.varint(mic_buffer_us);
    w.varint(mic_underruns);
    w.varint(mic_dropped_frames);
    w.varint(playback_dropped);
    return message(MsgType::AudioStats, w);
}

bool AudioStatsMsg::decode(std::string_view payload) {
    wire::Reader r(payload);
    mic_valid = r.boolean();
    mic_capture_us = r.u64();
    mic_out_us = r.u64();
    mic_buffer_us = r.varint32();
    mic_underruns = r.varint();
    mic_dropped_frames = r.varint();
    playback_dropped = r.varint();
    return r.ok();
}

}  // namespace broremote
