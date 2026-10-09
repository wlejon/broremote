# broremote wire protocol, version 1.3

A viewer and a broremote server talk over byte streams: a control connection, and from 1.2 optionally further connections (lanes, below) that join its session. Locally each is a connection to the server's local address: an AF_UNIX stream socket on Linux and macOS, a named pipe on Windows. Remotely each is the stdio of its own `ssh host broremote proxy`, which relays bytes to the remote host's local address without reading them. The protocol is the same in every case, and nothing in it depends on the transport. The transport (local listener and connector, peer checks, spawned streams, the proxy relay, lane grants) is the brolink sibling's.

The protocol is hand-rolled and length-prefixed. Every field is bounds-checked when read, and a peer never trusts a length it receives. Structs are never copied onto the wire. The framing and primitives are brolink's (`brolink/wire.h`, wrapped by `include/broremote/wire.h`), the messages are in `include/broremote/protocol.h`; this document and those headers describe the same format.

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

- The client's first message must be `Hello` (or, from 1.2, `Join` on a lane connection). Any other message first gets `Error(HelloRequired)` and the connection is closed.
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

## Lanes (1.2)

A session can spread over several connections, so that one kind of traffic never waits behind another: input written while a 2 MB keyframe is queued on the control connection would otherwise sit behind it, in the server's queue and in every buffer of the ssh hop. Each connection is a lane; the control connection (the one that sent `Hello`) is the session.

```
control connection                  server
Hello            ------------------>
                 <------------------ Welcome (+ grant: session id, token)
...
second connection
Join(session, token, "input") ----->
                 <------------------ Joined     (or Error(JoinRefused), closed)
Input, Ping      ------------------>
                 <------------------ Pong
```

- **The grant.** A 1.2 server ends `Welcome` with `varint session` and a 32-byte `token` from the OS CSPRNG (`getrandom` / `BCryptGenRandom`). It is the only way to join the session; a server that could not get random bytes sends no grant, and its client has no lanes.
- **Join.** A new connection's first message is `Join` instead of `Hello`. The server compares the token in constant time. A wrong token, an unknown session, a lane name the server does not have, or a lane already joined is `Error(JoinRefused)` (the message says which) and the connection is closed. Each lane name joins once per session. `Join` on a connection that sent `Hello` is `Error(BadMessage)`.
- **The input lane.** The only lane is `input` (`kInputLane`). It carries `Input`, and `Ping` so the client can time it (answered with `Pong` on the lane). Any other message on it gets `Error(UnknownMessage)` and the lane stays; `Hello` or a second `Join` is `Error(BadMessage)`. Nothing else is sent on it: video, cursor and errors for the session go on the control connection. Input from either connection goes to the host in the order each connection delivered it; a client that has an input lane sends all its input there.
- **Lifetime.** The session ends with its control connection, which closes its lanes. A lane closing leaves the session as it was; the client then sends input on the control connection again.
- **Transport.** A lane reaches the server any way the control connection can: another local connection, or another `ssh host broremote proxy`. Each is its own TCP connection over ssh, so input never queues behind video anywhere.
- A 1.0 / 1.1 server sends no grant, so a client never sends `Join` to one.

## The audio lane (1.3)

A second lane, `audio` (`kAudioLane`), joined exactly like the input lane, carries raw PCM both ways: the host's audio (what that machine plays) to the viewer, and the viewer's microphone to the host, where it is a microphone of its own. A viewer joins it only to a server whose `Welcome` says minor >= 3; a 1.2 server refuses the lane name (`JoinRefused`).

```
audio lane connection               server
Join(session, token, "audio") ---->
                 <------------------ Joined
AudioStart       ------------------>
                 <------------------ AudioStarted
AudioUp, AudioUp ...  ------------->                (the viewer's mic)
                 <------------------ AudioDown, AudioDown ...   (the host's audio)
                 <------------------ AudioStats     (about twice a second, while the mic flows)
AudioControl     ------------------>                (mute toggles, any time)
Ping             ------------------>
                 <------------------ Pong
```

- **Formats.** `AudioFormat := varint rate (8000..192000), u8 channels (1..8), u8 sample (1 = s16le, 2 = f32le)`; anything else is malformed. `AudioStart` says which directions the viewer wants and in what format; the host answers with the formats it will use (1.3 hosts use the ones asked for, converting in the audio system) or turns a direction off. Raw PCM only: a codec, when one comes, is a new sample-format value. At the defaults (48 kHz s16, stereo down, mono up) the lane carries 1.5 Mbit/s down and 0.77 up.
- **Packets.** `AudioUp` / `AudioDown` each carry whole interleaved frames of their direction's format, with a sequence number counting that direction's packets from 1 and the sender's monotonic clock (`u64` microseconds; the host's is the clock `Pong` reports) when the first frame was captured. A packet is whatever one device period delivered (a few milliseconds): nothing is batched. A sender whose lane backs up drops its oldest unsent audio rather than queueing it; a gap in the sequence shows it.
- **Jitter buffers.** Each receiver plays through a jitter buffer that fills to a target depth (20 ms by default) before playing, plays silence and refills after running dry, and drops its oldest audio down to the target when it holds more than its bound (80 ms by default), so a stall or a faster sender clock never builds latency.
- **Latency.** The viewer keeps the offset between its clock and the host's from `Ping` / `Pong` on the lane (the sample with the smallest round trip). Downlink: the playout pairs each frame's host capture stamp with when it is heard here. Uplink: `AudioStats` returns the latest viewer capture stamp the host's mic node played and the host time it played it. Each is one subtraction once the offset is known.
- **The host's side.** For the mic the host makes a microphone node named for the viewer (`AudioStarted.mic_node`, e.g. "broremote: laptop mic") that exists while the lane does; optionally it is the default source meanwhile, and the previous default is restored. The host's audio is the default output's monitor.
- **Lifetime and failure.** The lane ends with its connection or with the session's control connection. Its failing, or the host lacking audio devices (`AudioStarted` with both directions off and `message` saying why), changes nothing on the control connection or the input lane. Any other message on the lane gets `Error(UnknownMessage)` and the lane stays; `AudioUp` or `AudioControl` before `AudioStart`, a second `AudioStart`, an `AudioUp` that is not whole frames, or `Hello` / `Join` are `Error(BadMessage)` and close the lane.

## Messages: client to server (0x01xx)

| Type | Name | Body |
|------|------|------|
| 0x0101 | Hello | `u8[4] magic` ("BRRM"), `u16 major`, `u16 minor`, `str client_name` (at most 256 bytes) |
| 0x0102 | Ack | `varint frame_id` — the highest frame id finished with |
| 0x0103 | RequestKeyframe | (empty) — the decoder lost sync; the next frame is a keyframe |
| 0x0104 | Input | `InputEvent` (below) |
| 0x0105 | SetCodec | `varint n` (at most 16), `u8 codec` × n (preference order; unknown values are skipped), `varint max_bitrate_kbps` (32-bit; 0 = no limit) |
| 0x0106 | Ping (1.1) | `u64 token` — answered at once with `Pong`. A client sends it only to a server whose `Welcome` says minor >= 1 |
| 0x0107 | Join (1.2) | `varint session`, `u8[32] token` (both from the `Welcome` grant), `str lane` (at most 64 bytes; `input`, or from 1.3 `audio`) — the first message of a lane connection |
| 0x0108 | AudioStart (1.3) | `str source` (the viewer's machine, at most 256 bytes), `bool playback`, `AudioFormat playback_format`, `bool mic`, `AudioFormat mic_format` — the audio lane's first message after `Joined` |
| 0x0109 | AudioUp (1.3) | `varint seq`, `u64 capture_us` (the viewer's clock), `bytes pcm` (at most 1 MiB; whole frames of the mic format) |
| 0x010A | AudioControl (1.3) | `bool playback_muted` (the host stops sending `AudioDown`), `bool mic_muted` (the host's mic node plays silence) |

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
| 0x0201 | Welcome | `u16 major`, `u16 minor`, `str server_name` (at most 256 bytes); 1.2 appends the lane grant: `varint session`, `u8[32] token` |
| 0x0202 | StreamConfig | `varint stream_id`, `u8 codec`, `varint width`, `varint height` (1..16384 each), `varint fps` (a hint) |
| 0x0203 | Video | `varint stream_id`, `varint frame_id`, `svarint pts_ns`, `u8 flags` (bit 0 keyframe; other bits ignored), `bytes bitstream`; 1.1 appends `varint submit_us`, `varint queue_us`, `varint encode_us` (below) |
| 0x0204 | Cursor | `bool visible`, `svarint x`, `svarint y` (32-bit, stream pixels), `varint hotspot_x`, `varint hotspot_y`, `str shape` (a CSS cursor name, at most 64 bytes) |
| 0x0205 | Error | `u16 code`, `str message` (at most 4096 bytes) |
| 0x0206 | Pong (1.1) | `u64 token` (the Ping's), `u64 server_time_us` (the server's monotonic clock when it answered) |
| 0x0207 | FrameSent (1.1) | `varint frame_id`, `varint wait_us`, `varint write_us` |
| 0x0208 | Joined (1.2) | (empty) — the `Join` was accepted; the connection is now that lane |
| 0x0209 | AudioStarted (1.3) | `bool playback`, `AudioFormat playback_format`, `bool mic`, `AudioFormat mic_format`, `str mic_node` (at most 256 bytes), `str message` (why a direction is off; at most 4096 bytes) |
| 0x020A | AudioDown (1.3) | as `AudioUp`, the host's clock and the playback format |
| 0x020B | AudioStats (1.3) | `bool mic_valid`, `u64 mic_capture_us` (the viewer's clock), `u64 mic_out_us` (the host's: when its mic node played that frame), `varint mic_buffer_us`, `varint mic_underruns`, `varint mic_dropped_frames`, `varint playback_dropped` (`AudioDown` packets the host dropped) |

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
| 8 | JoinRefused (1.2) | closed: a `Join` was refused (bad token, no such session or lane, lane already joined) |

## The Raw codec

`Codec::Raw` is brovideo's: RGBA pixels, run-length coded, with XOR-delta predicted frames between keyframes so that keyframe handling is exercised exactly as with a real codec. Its packet format is specified in brovideo's docs/raw.md; a `Video` message carries one such packet unchanged.
