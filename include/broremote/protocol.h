#pragma once
// The broremote wire protocol: message types and their bodies.
//
// Versioning. The client opens with Hello (magic "BRRM", major, minor, name);
// the server answers Welcome or Error(VersionMismatch) and closes. Peers with
// the same major interoperate: a newer minor only ever appends fields to the
// end of a body (every decoder ignores trailing bytes) or adds message types
// (a server answers an unknown type with Error(UnknownMessage) and stays
// connected; a client ignores it). Anything else bumps the major.
// docs/protocol.md documents every body.
//
// Each message struct has encode(), which returns the whole framed message
// (length + type + body), and decode(payload), which parses a body and
// returns false when it is malformed.

#include "broremote/codec.h"
#include "broremote/frame.h"
#include "broremote/wire.h"

#include <brolink/lanes.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace broremote {

inline constexpr char kProtocolMagic[4] = {'B', 'R', 'R', 'M'};
inline constexpr uint16_t kProtocolMajor = 1;
// 1.1 added Ping / Pong, FrameSent and the timing fields at the end of Video.
// 1.2 added lanes: Welcome ends with a lane grant, and a further connection
// opens with Join instead of Hello to become the session's input lane.
inline constexpr uint16_t kProtocolMinor = 2;

// The lanes a server accepts (Join's lane name).
inline constexpr std::string_view kInputLane = "input";

// Limits a decoder enforces on what a peer sends.
inline constexpr size_t kMaxNameBytes = 256;        // Hello / Welcome names
inline constexpr size_t kMaxErrorBytes = 4096;      // Error message text
inline constexpr size_t kMaxShapeBytes = 64;        // Cursor shape name
inline constexpr size_t kMaxCodecList = 16;         // SetCodec entries
inline constexpr uint32_t kMaxDimension = 16384;    // StreamConfig width / height

enum class MsgType : uint16_t {
    // client -> server
    Hello = 0x0101,
    Ack = 0x0102,
    RequestKeyframe = 0x0103,
    Input = 0x0104,
    SetCodec = 0x0105,
    Ping = 0x0106,       // 1.1
    Join = 0x0107,       // 1.2: a lane connection's first message
    // server -> client
    Welcome = 0x0201,
    StreamConfig = 0x0202,
    Video = 0x0203,
    Cursor = 0x0204,
    Error = 0x0205,
    Pong = 0x0206,       // 1.1
    FrameSent = 0x0207,  // 1.1
    Joined = 0x0208,     // 1.2: the answer to Join
};

enum class ErrorCode : uint16_t {
    None = 0,
    BadMessage = 1,       // a body did not decode, or the framing broke; closed
    UnknownMessage = 2,   // a type this server does not know; stays connected
    VersionMismatch = 3,  // Hello with another major; closed
    HelloRequired = 4,    // a message before Hello; closed
    NoCommonCodec = 5,    // SetCodec named no codec the server can encode for every client; closed
    EncoderFailed = 6,    // the encoder could not be created or failed; closed
    ServerShutdown = 7,   // the host is shutting the server down; closed
    JoinRefused = 8,      // 1.2: a Join named no live session, the wrong token, or a lane not on offer; closed
};

[[nodiscard]] const char* error_code_name(ErrorCode) noexcept;

// ---- client -> server ---------------------------------------------------------------

struct HelloMsg {
    uint16_t major = kProtocolMajor;
    uint16_t minor = kProtocolMinor;
    std::string name;
    [[nodiscard]] std::string encode() const;
    bool decode(std::string_view payload);  // false also on a wrong magic
};

struct AckMsg {
    uint64_t frame_id = 0;  // highest frame id the client has decoded
    [[nodiscard]] std::string encode() const;
    bool decode(std::string_view payload);
};

struct RequestKeyframeMsg {
    [[nodiscard]] std::string encode() const;
    bool decode(std::string_view payload);
};

struct InputMsg {
    InputEvent event;
    [[nodiscard]] std::string encode() const;
    // False on a malformed body. A body whose kind this version does not
    // know (a newer minor's) decodes to false with *unknown_kind set: the
    // server ignores it rather than treating it as an error.
    bool decode(std::string_view payload, bool* unknown_kind = nullptr);
};

struct SetCodecMsg {
    std::vector<Codec> codecs;       // preference order; values this version does not know are dropped
    uint32_t max_bitrate_kbps = 0;   // 0: no limit
    [[nodiscard]] std::string encode() const;
    bool decode(std::string_view payload);
};

// 1.1. The server answers at once, on its I/O thread, ahead of any queued
// video, so the round trip is the transport's own.
struct PingMsg {
    uint64_t token = 0;  // echoed in the Pong (a viewer sends its clock)
    [[nodiscard]] std::string encode() const;
    bool decode(std::string_view payload);
};

// 1.2. The first message of a further connection to the server: it joins the
// session of the client whose Welcome carried the grant, as the named lane
// (kInputLane). The server answers Joined, or Error(JoinRefused) and closes.
// An input lane carries Input (and Ping); everything else stays on the
// control connection, which ends the session, and its lanes, when it closes.
struct JoinMsg {
    brolink::lanes::Join join;
    [[nodiscard]] std::string encode() const;
    bool decode(std::string_view payload);
};

// ---- server -> client ---------------------------------------------------------------

// 1.2: the answer to a Join that succeeded. Empty body.
struct JoinedMsg {
    [[nodiscard]] std::string encode() const;
    bool decode(std::string_view payload);
};

struct PongMsg {
    uint64_t token = 0;           // the Ping's
    uint64_t server_time_us = 0;  // the server's monotonic clock when it answered
    [[nodiscard]] std::string encode() const;
    bool decode(std::string_view payload);
};

// 1.1, to clients that said minor >= 1 in Hello: sent once the last byte of
// a Video message was handed to that client's socket.
struct FrameSentMsg {
    uint64_t frame_id = 0;
    uint64_t wait_us = 0;   // the packet was queued for this client -> its first byte was written
    uint64_t write_us = 0;  // first byte written -> last byte written (socket backpressure)
    [[nodiscard]] std::string encode() const;
    bool decode(std::string_view payload);
};

struct WelcomeMsg {
    uint16_t major = kProtocolMajor;
    uint16_t minor = kProtocolMinor;
    std::string name;
    // 1.2: what a further connection presents in Join to become one of this
    // client's lanes (a session id and a token from the OS CSPRNG). Absent
    // from a 1.0 / 1.1 server.
    std::optional<brolink::lanes::Grant> grant;
    [[nodiscard]] std::string encode() const;
    bool decode(std::string_view payload);
};

// Every Video message belongs to the latest StreamConfig the client was
// sent, and the first Video after a StreamConfig is a keyframe.
struct StreamConfig {
    uint64_t stream_id = 0;  // increments on every reconfigure
    Codec codec = Codec::Raw;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;  // a hint
    [[nodiscard]] std::string encode() const;
    bool decode(std::string_view payload);
    bool operator==(const StreamConfig&) const = default;
};

// Where a frame spent its time on the server (1.1, after the bitstream).
// Times are microseconds; submit_us is on the server's monotonic clock (the
// one Pong reports), so a viewer that knows the clock offset can place the
// frame on its own timeline.
struct FrameTiming {
    bool valid = false;      // false: a 1.0 server sent no timing
    uint64_t submit_us = 0;  // Server::submit() was called
    uint64_t queue_us = 0;   // submit -> encode start (waiting for the encoder or the ack window)
    uint64_t encode_us = 0;  // encode start -> packet ready (conversion and encode)
    bool operator==(const FrameTiming&) const = default;
};

struct VideoPacket {
    uint64_t stream_id = 0;
    uint64_t frame_id = 0;  // increments per packet across streams; the client acks it
    int64_t pts_ns = 0;
    bool keyframe = false;
    std::vector<uint8_t> data;
    FrameTiming timing;
    [[nodiscard]] std::string encode() const;
    bool decode(std::string_view payload);
    // Encode straight from a bitstream, without building a VideoPacket.
    static std::string encode_message(uint64_t stream_id, uint64_t frame_id, int64_t pts_ns, bool keyframe,
                                      std::span<const uint8_t> data, const FrameTiming* timing = nullptr);
};

struct CursorMsg {
    CursorState state;
    [[nodiscard]] std::string encode() const;
    bool decode(std::string_view payload);
};

struct ErrorMsg {
    ErrorCode code = ErrorCode::None;
    std::string message;
    [[nodiscard]] std::string encode() const;
    bool decode(std::string_view payload);
};

}  // namespace broremote
