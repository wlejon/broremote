# broremote design

broremote streams a whole desktop session from one machine to another: the
composited output goes out as hardware-encoded video, and keyboard and pointer
input come back. It is a C++20 library with no dependency on bro or bronze.
In the bro ecosystem it remotes helm: bro, running helm under DRM on Linux,
hosts a broremote server; a viewer on another machine shows the session.

This page is the contract between the parts. Each part is built and tested on
its own against the interfaces here.

## What it is not

- **Not per-window.** It streams the composited output, the frame that went to
  scanout, so the shell and every client window are in it. Streaming
  individual Wayland windows is a different product and is out of scope.
- **No crypto, no network listener.** The server listens only on a local
  socket. A remote viewer reaches it through `ssh host broremote proxy`, which
  relays bytes to that socket without reading them, exactly as bromux does.
  Authentication and encryption are ssh's.
- **No GPL.** Codecs are reached through platform APIs (VA-API on Linux, Media
  Foundation on Windows), never through ffmpeg/x264. ffmpeg/ffprobe are used
  only by tests, as an oracle, when present.

## Parts

```
include/broremote/
  wire.h        primitives: little-endian ints, LEB128 varints, bounds-checked Reader
  protocol.h    message types and their encode/decode
  stream.h      blocking byte streams: local socket, child-process stdio (ssh), stdio
  frame.h       Frame (dmabuf or CPU), EncodedPacket, DecodedFrame, InputEvent
  codec.h       Encoder / Decoder interfaces and the factory
  server.h      Server: the host submits frames and drains input
  client.h      Client: connects, receives packets, sends input
src/            implementation; src/posix, src/win for platform pieces
src/vaapi/      VA-API encoder (Linux)
src/mf/         Media Foundation decoder (Windows)
tools/          broremote (CLI: proxy, serve-test), broremote-view (viewer)
tests/          ctest suite
```

CMake target `broremote` (alias `broremote::broremote`). Options:
`BROREMOTE_BUILD_TESTS`, `BROREMOTE_BUILD_TOOLS`, `BROREMOTE_BUILD_VIEWER`
(on when SDL3 is found), `BROREMOTE_WITH_VAAPI` (auto on Linux when libva and
libva-drm are found), `BROREMOTE_WITH_MF` (on for Windows). A disabled backend
is simply absent from the codec factory; nothing else changes.

Dependencies resolve by the ecosystem convention (existing target, then
`../<name>`, then `third_party/<name>`), though the core needs none. SDL3 for
the viewer: an existing `SDL3::SDL3` target, else `find_package(SDL3)`.

## Wire format

Same shape as bromux's (docs/protocol.md there):

```
message := u32 length   -- little endian; bytes that follow (type + body), >= 2
           u16 type     -- little endian
           body
```

A length below 2 or above `kMaxMessage` (64 MiB) is a framing error and closes
the stream. Fixed-width integers are little endian; `varint` is unsigned
LEB128; `str`/`bytes` are a varint length then the bytes. Readers never trust
a length: a read past the end marks the Reader failed. Structs are never
memcpy'd onto the wire.

Versioning: the client's first message is `Hello{magic "BRRM", major, minor,
client name}`; the server answers `Welcome{major, minor, server name}` or
`Error(VersionMismatch)` and closes. Minor versions only append fields (every
decoder ignores trailing bytes) or add message types (unknown types are
ignored by clients, answered with `Error(UnknownMessage)` by servers). Anything
else bumps the major. The full catalogue lives in docs/protocol.md.

### Server to client

| Message | Body |
|---|---|
| `Welcome` | major, minor, server name |
| `StreamConfig` | stream id (varint, increments on every reconfigure), codec, width, height, fps hint |
| `Video` | stream id, frame id (varint), pts ns (svarint), flags (keyframe), bitstream bytes |
| `Cursor` | visible, x, y in stream pixels, hotspot, shape name (str); image later |
| `Error` | code, message |

A `Video` message always belongs to the latest `StreamConfig` the client was
sent; the first `Video` after a `StreamConfig` is a keyframe. Bitstreams are
Annex B for H.264/HEVC and a temporal unit of OBUs for AV1, with parameter
sets in-band on every keyframe, so a decoder needs nothing but the packets.

### Client to server

| Message | Body |
|---|---|
| `Hello` | magic, major, minor, client name |
| `Ack` | highest frame id the client has decoded |
| `RequestKeyframe` | (empty) — the decoder lost sync |
| `Input` | one `InputEvent` (below) |
| `SetCodec` | preferred codec list, max bitrate kbps (optional; the server picks) |

### Input

Input is in the *server's* terms, so the host can inject it as if it came from
its own devices:

- **Keys:** Linux evdev `KEY_*` code, pressed/released. The viewer maps its
  platform's scancodes to evdev. On focus loss the viewer releases every key it
  pressed, so nothing sticks down on the server.
- **Pointer motion:** absolute position in stream pixels (floats). The viewer
  scales from its window to the stream.
- **Buttons:** evdev `BTN_*` code, pressed/released.
- **Wheel:** horizontal and vertical, in 120ths of a detent (the Windows /
  libinput high-resolution unit), signed.

Text input/IME and clipboard are later minors.

## Library interfaces

### Frame

```cpp
struct DmabufPlane { int fd; uint32_t offset, pitch; };
struct Frame {
    uint32_t width, height;
    uint32_t drm_format;          // DRM fourcc, e.g. XRGB8888
    uint64_t modifier;            // DRM format modifier
    uint32_t plane_count;         // 0 => CPU frame
    DmabufPlane planes[4];
    int acquire_fence_fd = -1;    // sync_file; the content is ready when it signals (-1: ready)
    const uint8_t* cpu = nullptr; // CPU frame: tightly packed RGBA8 (plane_count == 0)
    uint32_t cpu_stride = 0;
    int64_t pts_ns = 0;
};
```

The fds belong to the host. The server never closes them; it holds the frame
until it calls the release callback.

### Server (host side)

```cpp
struct ServerConfig {
    std::string socket_name = "default";   // $XDG_RUNTIME_DIR/broremote/<name>.sock
    std::vector<Codec> codecs = {Codec::H264}; // preference order
    uint32_t bitrate_kbps = 20000;
    uint32_t fps = 60;
    uint32_t max_frames_in_flight = 2;     // unacked frames per client before encoding pauses
    uint32_t keyframe_interval_s = 0;      // 0: keyframes only on demand
    std::string name = "broremote";
};
class Server {
public:
    static std::unique_ptr<Server> create(const ServerConfig&, std::string* err);
    // Any thread. Returns at once. The server holds at most one pending frame:
    // a newer submit replaces it, and the replaced frame is released at once.
    // `release` runs on a server thread once the server no longer reads the
    // frame's memory (after the colour-conversion copy, well within a frame).
    void submit(const Frame&, std::function<void()> release);
    // Host thread, once per host frame. Appends every input event received
    // since the last call.
    void drain_input(std::vector<InputEvent>& out);
    void set_cursor(const CursorState&);
    size_t client_count() const;
    bool wants_frames() const;   // false when no client is attached: the host can skip submitting
};
```

Internally: an I/O thread runs the accept/read/write loop; an encode thread
takes the pending frame, converts and encodes it, and queues the packet to each
client. Encoding pauses (frames are released unencoded) while any client has
`max_frames_in_flight` unacked frames, so a slow link gets fewer frames, never
a backlog. Encoded packets are never dropped: with predicted frames, dropping
one corrupts every frame up to the next keyframe. A client joining, a
`RequestKeyframe`, or a size change forces a keyframe (and, for a size change,
a new `StreamConfig`).

The socket is created mode 0600 in a 0700 directory and the peer's uid is
checked (`SO_PEERCRED`), as in bromux.

### Client (viewer side)

```cpp
class Client {
public:
    static std::unique_ptr<Client> connect(std::unique_ptr<Stream>, std::string* err);
    // Callbacks run on the client's reader thread.
    std::function<void(const StreamConfig&)> on_config;
    std::function<void(const VideoPacket&)> on_video;
    std::function<void(const CursorState&)> on_cursor;
    std::function<void(const std::string&)> on_closed;
    void ack(uint64_t frame_id);
    void request_keyframe();
    void send_input(const InputEvent&);
};
```

### Codecs

```cpp
enum class Codec : uint8_t { Raw = 0, H264 = 1, HEVC = 2, AV1 = 3 };
class Encoder {
public:
    virtual ~Encoder() = default;
    // Converts and encodes one frame. Calls `release` as soon as the frame's
    // memory is no longer read (before the encode itself finishes).
    virtual bool encode(const Frame&, bool force_keyframe, const std::function<void()>& release,
                        EncodedPacket& out, std::string* err) = 0;
};
class Decoder {
public:
    virtual ~Decoder() = default;
    virtual bool decode(std::span<const uint8_t> bitstream, DecodedFrame& out, std::string* err) = 0;
};
std::unique_ptr<Encoder> create_encoder(Codec, const EncoderConfig&, std::string* err);
std::unique_ptr<Decoder> create_decoder(Codec, std::string* err);
std::vector<Codec> available_encoders();
std::vector<Codec> available_decoders();
```

`Codec::Raw` is built in on every platform: CPU frames, RGBA, run-length
compressed. It exists so the protocol and server/client run and are tested
everywhere (CI, Windows, a box with no VA-API), not for real use.

**VA-API encoder (Linux).** Imports the dmabuf as a VA surface (DRM PRIME 2,
with the modifier), waits on the acquire fence, runs a VideoProc pass to an
NV12 surface it owns (BT.709, limited range; the conversion also crops/pads to
the coded size), releases the frame, then encodes. H.264 High (Main/Constrained
Baseline when High is missing), low-latency: I and P frames only, no
B-frames, one reference, CBR at the configured bitrate, keyframe on demand,
parameter sets in-band on every IDR. HEVC and AV1 follow the same shape. CPU
frames are uploaded to a VA surface and take the same path.

**Media Foundation decoder (Windows).** The H.264 (and HEVC/AV1 where the
system has them) decoder MFT, hardware-accelerated through a D3D11 device
manager where available, low-latency mode on, NV12 out.

## Tools

- `broremote proxy [--socket NAME]`: relays stdin/stdout to the local server
  socket. This is what `ssh host broremote proxy` runs.
- `broremote serve-test [--socket NAME] [--size WxH] [--codec C]`: a server fed
  a moving test pattern (CPU frames), so a viewer can be tested with no bro.
- `broremote-view [--ssh HOST | --socket NAME] [--ssh-command CMD]`: SDL3
  window; decodes and shows the stream scaled to the window with the aspect
  kept; sends keys, pointer and wheel; requests a keyframe on decode error;
  releases held keys on focus loss.

## The bro side (not in this repo)

bro links broremote under `BRO_WITH_REMOTE`. A small host adapter in bro
submits each composited frame from the KMS presenter (the scanout buffer is a
GBM dmabuf already, with its render-done fence) while `wants_frames()` is true,
and feeds drained input to the engine's DRM input path, so remote input routes
to the shell or a client exactly as local input does. helm decides whether to
host, through broremote's JavaScript binding (`broremote_api`, optional,
gated by `BROREMOTE_ENABLE_API`, mounted by bro's `installSiblingApis`).

## Testing

- Wire and protocol: round-trip every message; hostile lengths and counts fail
  cleanly.
- Server and client in one process over a socket pair, `Codec::Raw`, a CPU
  frame source: config, frames, acks, flow control (a client that stops
  acking stops encoding without a backlog), keyframe on join and on request,
  resize, input round-trip, two clients.
- VA-API (Linux with a VA encoder): encode a known pattern, decode with
  ffmpeg (when present) and compare PSNR against the source; dmabuf import
  from a GBM buffer.
- Media Foundation (Windows): decode a fixture bitstream produced by ffmpeg
  and compare against the expected pattern.
- End to end: `serve-test` on the halo, `broremote-view --ssh halo` on
  Windows.
