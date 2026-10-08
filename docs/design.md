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
  frame.h       Frame (dmabuf or CPU), EncodedPacket, DecodedFrame, InputEvent, CursorState
  codec.h       Codec, EncoderConfig, Encoder / Decoder interfaces and the factory
  server.h      Server: the host submits frames and drains input
  client.h      Client: connects, receives packets, sends input
src/            implementation; src/posix, src/win for platform pieces
src/codec_factory.cpp, src/codec_backends.h
                the factory: the one place a backend registers
src/vaapi/      VA-API encoder (Linux)
src/mf/         Media Foundation decoder (Windows)
tools/          broremote (CLI: proxy, serve-test, codecs), broremote-view (viewer)
tests/          ctest suite
```

CMake target `broremote` (alias `broremote::broremote`). Options:
`BROREMOTE_BUILD_TESTS`, `BROREMOTE_BUILD_TOOLS`, `BROREMOTE_BUILD_VIEWER`
(on when SDL3 is found), `BROREMOTE_WITH_VAAPI` (auto on Linux when libva and
libva-drm are found), `BROREMOTE_WITH_MF` (on for Windows). A disabled backend
is simply absent from the codec factory; nothing else changes. A backend adds
its sources and defines `BROREMOTE_HAVE_VAAPI` / `BROREMOTE_HAVE_MF` on the
`broremote` target; `src/codec_backends.h` declares its entry points
(`vaapi_encoders()` + `create_vaapi_encoder()`, `mf_decoders()` +
`create_mf_decoder()`) and `src/codec_factory.cpp` calls them behind those
`#if`s. The `*_encoders()` / `*_decoders()` probes report what the machine can
actually do, so `available_encoders()` is honest on a box without the hardware.

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
LEB128, `svarint` zigzag LEB128; `f32` is an IEEE single's bit pattern as a
u32; `str`/`bytes` are a varint length then the bytes. Readers never trust
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
| `Ack` | highest frame id the client has finished with: decoded, or abandoned after a decode error |
| `RequestKeyframe` | (empty) — the decoder lost sync |
| `Input` | one `InputEvent` (below) |
| `SetCodec` | preferred codec list, max bitrate kbps (optional; the server picks) |

A viewer must ack frames it fails to decode too (and then request a
keyframe): an unacked frame holds the ack window shut, and with encoding
paused the keyframe it asked for would never come.

There is one encoded stream for all clients, so `SetCodec` narrows a shared
choice: the server takes the first codec of its own list that every client
which sent `SetCodec` listed, and closes a client whose list leaves none
(`Error(NoCommonCodec)`). The bitrate is the server's, lowered to the smallest
nonzero client maximum.

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
  libinput high-resolution unit), signed: +x right, +y down (libinput's
  sense; a viewer on Windows negates `WM_MOUSEWHEEL`).

Text input/IME and clipboard are later minors. The server ignores an input
kind it does not know (a newer minor's) instead of treating it as an error.

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
    const uint8_t* cpu = nullptr; // CPU frame (plane_count == 0): RGBA8 rows, bytes R G B A
    uint32_t cpu_stride = 0;      // bytes between CPU rows; 0 => width * 4
    int64_t pts_ns = 0;
};
struct EncodedPacket { std::vector<uint8_t> data; bool keyframe; int64_t pts_ns; };
enum class PixelFormat : uint8_t { RGBA8, NV12 };
struct DecodedFrame {
    bool ready;                   // false: the decoder took the input but has no picture yet
    uint32_t width, height;
    PixelFormat format;           // Raw gives RGBA8; MF gives NV12
    uint32_t stride, uv_offset;   // NV12: Y rows, then (height+1)/2 interleaved UV rows at uv_offset
    std::vector<uint8_t> data;
};
struct CursorState { bool visible; int32_t x, y; uint32_t hotspot_x, hotspot_y; std::string shape; };
```

The fds belong to the host. The server never closes them; it holds the frame
until it calls the release callback. `drm_format` is ignored for CPU frames.
`DecodedFrame` is CPU memory; a zero-copy D3D11 texture path for the viewer
can be added beside it when the MF decoder needs one.

### Server (host side)

```cpp
struct ServerConfig {
    std::string socket_name = "default";   // $XDG_RUNTIME_DIR/broremote/<name>.sock
    std::vector<Codec> codecs = {Codec::H264}; // preference order; ones this build cannot encode are skipped
    uint32_t bitrate_kbps = 20000;
    uint32_t fps = 60;
    uint32_t max_frames_in_flight = 2;     // unacked frames per client before encoding pauses
    uint32_t keyframe_interval_s = 0;      // 0: keyframes only on demand
    std::string name = "broremote";
};
class Server {
public:
    // Null when no configured codec can be encoded here, or the socket cannot
    // be made (including when a live server already listens on it; a stale
    // socket file is replaced).
    static std::unique_ptr<Server> create(const ServerConfig&, std::string* err);
    ~Server();   // joins the threads, releases a held frame, closes every client
    // Any thread. Returns at once. The server holds at most one pending frame:
    // a newer submit replaces it, and the replaced frame is released at once.
    // `release` is called exactly once per submit: inside submit() itself (on
    // the caller's thread) for a replaced frame or when no client is attached,
    // otherwise on the encode thread once the encoder no longer reads the
    // frame's memory (after the colour-conversion copy, well within a frame).
    // It must not call back into the Server.
    void submit(const Frame&, std::function<void()> release);
    // Host thread, once per host frame. Appends every input event received
    // since the last call (queued up to 65536; beyond that new ones drop).
    void drain_input(std::vector<InputEvent>& out);
    void set_cursor(const CursorState&);  // sent on change and to each joining client
    size_t client_count() const;
    bool wants_frames() const;   // false when no client is attached: the host can skip submitting
    const std::string& socket_path() const;
    Stats stats() const;         // submitted / encoded / keyframes / replaced / unwatched / failed / streams
};
```

Internally: an I/O thread runs the accept/read/write loop (non-blocking
sockets around one poll, woken by a waker when the encode thread queues
output); an encode thread takes the pending frame, converts and encodes it,
and queues the packet to each client. Encoding pauses while any client has
`max_frames_in_flight` unacked frames: the pending frame waits (a newer submit
still replaces and releases it), so a slow link gets fewer frames, never a
backlog, and the frame encoded when an ack opens the window is the newest
one. Holding it rather than releasing it matters for a host that submits only
on damage: a released frame would leave the viewer on a stale picture until
the next change. Encoded packets are never dropped: with predicted frames,
dropping one corrupts every frame up to the next keyframe. A client joining, a
`RequestKeyframe`, or a size change forces a keyframe (and, for a size,
codec or bitrate change, a new encoder and a new `StreamConfig`). An encoder
serves one size; the server makes a new one per stream. A client that has not
yet had a keyframe of the current stream is sent no predicted frames. Frame
ids count every packet from 1 across streams. If the encoder cannot be made
or fails, every client gets `Error(EncoderFailed)` and is closed.

The socket is created mode 0600 in a 0700 directory and the peer's uid is
checked (`SO_PEERCRED`) on both sides, as in bromux. Without
`$XDG_RUNTIME_DIR` the directory is `/tmp/broremote-<uid>/`. On Windows (tests
and development only; the real host is bro on Linux) the socket is AF_UNIX
too, in `%LOCALAPPDATA%\broremote\`, inside the user's profile, and the client
checks the server process runs as the same user
(`SIO_AF_UNIX_GETPEERPID` + the token's SID). A blocked `recv()` on a Windows
AF_UNIX socket is not reliably woken when the peer closes, so the Windows
client stream waits in `WSAPoll` with a short timeout instead.

### Client (viewer side)

```cpp
struct ClientOptions {
    std::string name = "broremote";
    std::vector<Codec> codecs;          // sent as SetCodec after the handshake when not empty
    uint32_t max_bitrate_kbps = 0;
    uint32_t connect_timeout_ms = 10000;
};
struct ClientHandlers {                 // run on the client's reader thread, in message order
    std::function<void(const StreamConfig&)> on_config;
    std::function<void(const VideoPacket&)> on_video;
    std::function<void(const CursorState&)> on_cursor;
    std::function<void(ErrorCode, const std::string&)> on_error;
    std::function<void(const std::string&)> on_closed;  // once, whatever ended it
};
class Client {
public:
    // Sends Hello, waits for Welcome (or the refusal, or the timeout).
    static std::unique_ptr<Client> connect(std::unique_ptr<Stream>, ClientHandlers,
                                           const ClientOptions&, std::string* err);
    ~Client();                          // closes and joins the reader (on_closed runs first)
    const WelcomeMsg& welcome() const;
    void ack(uint64_t frame_id);        // any thread
    void request_keyframe();
    void send_input(const InputEvent&);
    void set_codecs(const std::vector<Codec>&, uint32_t max_bitrate_kbps = 0);
    bool connected() const;
    void close();
};
```

The handlers are fixed at `connect()` rather than assigned afterwards: the
reader thread starts inside `connect()`, and a `StreamConfig` or the first
`Video` can arrive before `connect()` returns, so callbacks assigned later
could miss them. For the same reason a handler that needs the `Client` (to
ack, say) must not assume `connect()` has returned yet.

### Codecs

```cpp
enum class Codec : uint8_t { Raw = 0, H264 = 1, HEVC = 2, AV1 = 3 };
struct EncoderConfig { uint32_t width, height, fps = 60, bitrate_kbps = 20000; };
class Encoder {
public:
    virtual ~Encoder() = default;
    // Converts and encodes one frame of the configured size. Calls `release`
    // exactly once, as soon as the frame's memory is no longer read (before
    // the encode itself finishes), on failure too. The first packet and every
    // forced one are keyframes.
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
compressed, with XOR-delta predicted frames between keyframes so keyframe
handling is exercised as with a real codec (a delta with no reference is a
lost sync). Its format is in docs/protocol.md. It exists so the protocol and
server/client run and are tested everywhere (CI, Windows, a box with no
VA-API), not for real use.

`Decoder::decode` returns false when the bitstream is bad or the decoder lost
sync; the viewer then acks the frame anyway and requests a keyframe.

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
- `broremote serve-test [--socket NAME] [--size WxH] [--codec C] [--fps N]
  [--seconds N]`: a server fed a moving test pattern (CPU frames), so a viewer
  can be tested with no bro. The pattern (tools/test_pattern.h) is a
  scrolling gradient, a sweeping white bar, pure red/green/blue swatches and
  the frame counter as 32 one-bit blocks, so motion, channel order and
  dropped frames all show. With a codec this machine cannot encode it says
  so and exits 1. Input it receives is printed to stderr; pointer motion
  moves the cursor it reports.
- `broremote codecs`: what this build can encode and decode here.
- `broremote-view [--ssh HOST | --socket NAME] [--ssh-command CMD]`: SDL3
  window; decodes and shows the stream scaled to the window with the aspect
  kept; sends keys, pointer and wheel; requests a keyframe on decode error;
  releases held keys on focus loss.

## The bro side (not in this repo)

bro links broremote under `BRO_WITH_REMOTE`. A small host adapter in bro
submits each composited frame from the KMS presenter (the scanout buffer is a
GBM dmabuf already, with its render-done fence) while `wants_frames()` is true,
submits the current frame when `wants_frames()` turns true even if nothing
was redrawn (a joining viewer needs a keyframe, and the server keeps no
frame of its own), and feeds drained input to the engine's DRM input path, so remote input routes
to the shell or a client exactly as local input does. helm decides whether to
host, through broremote's JavaScript binding (`broremote_api`, optional,
gated by `BROREMOTE_ENABLE_API`, mounted by bro's `installSiblingApis`).

## Testing

- Wire and protocol: round-trip every message; hostile lengths and counts fail
  cleanly; every truncation of every body fails; random bodies into every
  decoder (tests/test_wire.cpp).
- Server and client in one process over a real local socket, `Codec::Raw`, a
  CPU frame source: config, frames decoded to the exact pixels, acks, flow
  control (a client that stops acking stops encoding without a backlog, and
  the newest frame goes out on resume), keyframe on join and on request,
  resize, input round-trip, cursor, two clients, codec negotiation, protocol
  errors, and every frame released exactly once on every path
  (tests/test_server.cpp).
- `broremote proxy` as a child stream relaying a whole session, and
  `serve-test` end to end (tests/test_proxy.cpp).
- VA-API (Linux with a VA encoder): encode a known pattern, decode with
  ffmpeg (when present) and compare PSNR against the source; dmabuf import
  from a GBM buffer.
- Media Foundation (Windows): decode a fixture bitstream produced by ffmpeg
  and compare against the expected pattern.
- End to end: `serve-test` on the halo, `broremote-view --ssh halo` on
  Windows.
