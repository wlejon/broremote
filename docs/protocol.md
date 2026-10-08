# broremote wire protocol, version 1.1

A viewer and a broremote server talk over one byte stream. Locally that is an AF_UNIX stream socket (on Linux and, for tests and development, on Windows). Remotely it is the stdio of `ssh host broremote proxy`, which relays bytes to the remote host's local socket without reading them. The protocol is the same in every case, and nothing in it depends on the transport.

The protocol is hand-rolled and length-prefixed. Every field is bounds-checked when read, and a peer never trusts a length it receives. Structs are never copied onto the wire. The code lives in `include/broremote/wire.h` (primitives) and `include/broremote/protocol.h` (messages); this document and those headers describe the same format.

## Framing

```
message := u32 length   -- little endian; the bytes that follow (type + body), >= 2
           u16 type     -- little endian; MsgType
           body         -- type-specific
```

A length below 2, or above 64 MiB (`kMaxMessage`), is a framing error. The stream cannot be resynchronised after one, so it is closed (a server first sends `Error(BadMessage)`).

## Primitives

| Name | Encoding |
|------|----------|
| `u8` `u16` `u32` `u64` | fixed width, little endian |
| `bool` | `u8`: 0 or 1 (anything else is malformed) |
| `varint` | unsigned LEB128, at most 10 bytes |
| `svarint` | zigzag (`(v << 1) ^ (v >> 63)`), then varint |
| `f32` | IEEE-754 single precision, its bit pattern as `u32` |
| `str` | varint length, then that many bytes (UTF-8) |
| `bytes` | varint length, then that many bytes |

Readers reject a length or count that the remaining bytes could not hold, so a hostile length never causes an allocation. Where a field below is narrower than its encoding (a `varint` that must fit 32 bits, say), a larger value is malformed.

## Versioning

- The client's first message must be `Hello`. Any other message first gets `Error(HelloRequired)` and the connection is closed.
- `Hello` carries the magic bytes `BRRM`, `major` and `minor`. A wrong magic is `Error(BadMessage)`; another major is `Error(VersionMismatch)`; both close the connection. Otherwise the server answers `Welcome`.
- **Minor versions only add.** A newer minor may append fields to the end of a body; every decoder ignores trailing bytes. A newer minor may also add message types:
  - A server answers an unknown type with `Error(UnknownMessage)` and stays connected.
  - A client ignores unknown types.
  - An `Input` whose kind the server does not know is ignored (it is a newer minor's).
- Any other change bumps the major.

## Session

```
client                              server
Hello            ------------------>
                 <------------------ Welcome
SetCodec (opt.)  ------------------>
                 <------------------ StreamConfig   (when a stream exists, at once; else with the first frame)
                 <------------------ Cursor         (when the host has set one)
                 <------------------ Video (keyframe), Video, Video ...
Ack              ------------------>
Input            ------------------>
RequestKeyframe  ------------------>
                 <------------------ StreamConfig   (on a reconfigure), Video (keyframe) ...
```

- **One stream for everyone.** The server encodes each frame once and sends the packet to every attached client. `Video` belongs to the latest `StreamConfig` the client was sent; the first `Video` a client gets after a `StreamConfig` is a keyframe.
- **Reconfigure.** A change of frame size, codec or bitrate makes a new stream: a `StreamConfig` with the next `stream_id`, then a keyframe.
- **Joining.** A client that completes the handshake is sent the current `StreamConfig` (if any) and the cursor, and the next frame is a keyframe. Until then it is sent no predicted frames.
- **Frame ids** count every encoded packet, from 1, across streams.
- **Flow control.** A client acks the highest frame id it has finished with: decoded, or abandoned after a decode error (it then sends `RequestKeyframe`). While any client has `max_frames_in_flight` (default 2) unacked frames, the server encodes nothing: the newest submitted frame waits and is encoded as soon as an ack opens the window, and older ones are dropped before encoding. So a slow link gets fewer frames, never a backlog, and encoded packets are never dropped (dropping a predicted frame would corrupt every frame up to the next keyframe). A client that never acks stalls the stream for everyone, which is the point: the server serves the slowest viewer.
- **Codec choice.** The server's own preference list (`ServerConfig::codecs`, minus what this build cannot encode) is the order. A client may narrow it with `SetCodec`; the server picks the first of its codecs that every client which sent `SetCodec` listed. If there is none, the client whose `SetCodec` made it impossible gets `Error(NoCommonCodec)` and is closed. The bitrate is the server's, lowered to the smallest nonzero `max_bitrate_kbps` any client sent.

## Messages: client to server (0x01xx)

| Type | Name | Body |
|------|------|------|
| 0x0101 | Hello | `u8[4] magic` ("BRRM"), `u16 major`, `u16 minor`, `str client_name` (at most 256 bytes) |
| 0x0102 | Ack | `varint frame_id` — the highest frame id finished with |
| 0x0103 | RequestKeyframe | (empty) — the decoder lost sync; the next frame is a keyframe |
| 0x0104 | Input | `InputEvent` (below) |
| 0x0105 | SetCodec | `varint n` (at most 16), `u8 codec` × n (preference order; unknown values are skipped), `varint max_bitrate_kbps` (32-bit; 0 = no limit) |
| 0x0106 | Ping (1.1) | `u64 token` — answered at once with `Pong`. A client sends it only to a server whose `Welcome` says minor >= 1 |

### InputEvent

```
InputEvent := u8 kind, then by kind:
  1 Key            varint code (32-bit; Linux evdev KEY_*), bool pressed
  2 PointerMotion  f32 x, f32 y   -- absolute, in stream pixels; must be finite
  3 Button         varint code (32-bit; evdev BTN_*), bool pressed
  4 Wheel          svarint dx, svarint dy (32-bit each) -- 120ths of a detent; +x right, +y down
```

Input is in the server's terms, so the host injects it as if it came from its own devices: the viewer maps its platform's keys to evdev and scales its window to stream pixels. On focus loss the viewer releases every key and button it pressed.

## Messages: server to client (0x02xx)

| Type | Name | Body |
|------|------|------|
| 0x0201 | Welcome | `u16 major`, `u16 minor`, `str server_name` (at most 256 bytes) |
| 0x0202 | StreamConfig | `varint stream_id`, `u8 codec`, `varint width`, `varint height` (1..16384 each), `varint fps` (a hint) |
| 0x0203 | Video | `varint stream_id`, `varint frame_id`, `svarint pts_ns`, `u8 flags` (bit 0 keyframe; other bits ignored), `bytes bitstream`; 1.1 appends `varint submit_us`, `varint queue_us`, `varint encode_us` (below) |
| 0x0204 | Cursor | `bool visible`, `svarint x`, `svarint y` (32-bit, stream pixels), `varint hotspot_x`, `varint hotspot_y`, `str shape` (a CSS cursor name, at most 64 bytes) |
| 0x0205 | Error | `u16 code`, `str message` (at most 4096 bytes) |
| 0x0206 | Pong (1.1) | `u64 token` (the Ping's), `u64 server_time_us` (the server's monotonic clock when it answered) |
| 0x0207 | FrameSent (1.1) | `varint frame_id`, `varint wait_us`, `varint write_us` |

### Timing (1.1)

So a viewer can tell where a frame's age comes from:

- **Ping / Pong.** The server answers a `Ping` on its I/O thread as soon as it reads it, and queues the `Pong` ahead of any queued video (after a message already partly written, which must finish first), so the round trip is the transport's. The token is the client's (a viewer sends its own clock). With the send and receive times, `server_time_us` gives the offset between the two clocks; the sample with the smallest round trip is the least skewed.
- **Video timing.** After the bitstream: `submit_us`, the server clock when the host submitted the frame; `queue_us`, from then to the encode starting (the encoder busy, or the ack window shut); `encode_us`, the conversion and encode. A 1.0 server sends none (the body ends at the bitstream); when present the three are whole, and a body that ends partway through them is malformed.
- **FrameSent.** Sent to a client whose `Hello` said minor >= 1, once the last byte of a `Video` was written to that client's socket: `wait_us` from the packet being queued for the client to its first byte being written, `write_us` from the first byte to the last (socket backpressure). It follows the `Video` it describes.

Codecs (the `u8 codec` is brovideo's `Codec` value, which brovideo keeps stable):

| Value | Codec | Bitstream in `Video` |
|-------|-------|----------------------|
| 0 | Raw | brovideo's Raw format (below; built in everywhere; for tests, not real use) |
| 1 | H264 | Annex B, parameter sets in band on every keyframe |
| 2 | HEVC | Annex B, parameter sets in band on every keyframe |
| 3 | AV1 | one temporal unit of OBUs, sequence header on every keyframe |

A decoder needs nothing but the packets: no extradata travels outside them.

Error codes:

| Code | Name | Then |
|------|------|------|
| 1 | BadMessage | closed: a body did not decode, the framing broke, or Hello came twice |
| 2 | UnknownMessage | stays connected |
| 3 | VersionMismatch | closed |
| 4 | HelloRequired | closed |
| 5 | NoCommonCodec | closed |
| 6 | EncoderFailed | closed (every client): the encoder could not be created or failed |
| 7 | ServerShutdown | closed (every client): the host destroyed the server |

## The Raw codec

`Codec::Raw` is brovideo's: RGBA pixels, run-length coded, with XOR-delta predicted frames between keyframes so that keyframe handling is exercised exactly as with a real codec. Its packet format is specified in brovideo's docs/raw.md; a `Video` message carries one such packet unchanged.
