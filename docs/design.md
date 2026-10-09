# broremote design

broremote streams a whole desktop session from one machine to another: the
composited output goes out as hardware-encoded video, and keyboard and pointer
input come back. Audio goes both ways: the viewer hears the host's audio, and
the viewer's microphone is a microphone on the host. It is a C++20 library
with no dependency on bro or bronze.
In the bro ecosystem it remotes helm: bro, running helm under DRM on Linux,
hosts a broremote server; a viewer on another machine shows the session.

This page is the contract between the parts. Each part is built and tested on
its own against the interfaces here.

## What it is not

- **Not per-window.** It streams the composited output, the frame that went to
  scanout, so the shell and every client window are in it. Streaming
  individual Wayland windows is a different product and is out of scope.
- **No crypto, no network listener.** The server listens only on a local
  address (an AF_UNIX socket on Linux and macOS, a named pipe on Windows). A
  remote viewer reaches it through `ssh host broremote proxy`, which relays
  bytes to that address without reading them, exactly as bromux does.
  Authentication and encryption are ssh's.
- **Not a transport library.** The local listener and connector, their peer
  checks, the event loop, spawned and stdio streams, the proxy relay, the
  framing and the lane grants are brolink's (../brolink), shared with bromux.
  broremote decides what goes on which connection.
- **No GPL.** Codecs are reached through platform APIs (VA-API on Linux, Media
  Foundation on Windows), never through ffmpeg/x264. ffmpeg/ffprobe are used
  only by tests, as an oracle, when present.
- **Not a codec library.** Every encoder and decoder is brovideo's
  (../brovideo; its docs/design.md has the VA-API and Media Foundation
  details and the driver workarounds). broremote decides which codec a
  session uses, when a keyframe is needed and when to encode at all.

## Parts

```
include/broremote/
  wire.h        brolink's framing and primitives, with broremote's 64 MiB limit
  protocol.h    message types and their encode/decode
  stream.h      brolink's blocking byte streams (local connection, child-process
                stdio for ssh, stdio) under broremote's socket names, and the proxy
  frame.h       brovideo's Frame, Packet (EncodedPacket), Picture (DecodedFrame); InputEvent, CursorState
  codec.h       brovideo's Codec, Encoder, Decoder and their configs; codec_known() for the wire
  server.h      Server: the host submits frames and drains input
  client.h      Client: connects, receives packets, sends input
  audio.h       PCM formats and conversion, the SPSC ring, the jitter buffer,
                tone analysis (level, frequency, onset), WAV files
  audio_device.h  the small audio device layer: Backend (capture, playback,
                devices), the platform backend, paced generated / discarding
                endpoints for tests
  audio_client.h  the viewer's audio lane: AudioClient (the protocol) and
                AudioSession (devices on both ends, latency)
  connect.h     ConnectTarget: where a viewer connects (ssh HOST + socket, or
                a local socket), and opening its streams and lanes
  latency.h     LatencyTracker: where each frame's time goes; latency probes
                and the input-marker row serve-test --latency draws
  viewer.h      ViewerSession: a whole viewer with no window (connect, decode,
                ping and audio threads); a display takes its newest picture
  api.h         the JavaScript binding's public header (forwards to src/api)
src/            implementation (no platform pieces: those are brolink's)
src/audio/      audio.cpp; device_pipewire.cpp (Linux), device_wasapi.cpp
                (Windows), device_paced.cpp (paced endpoints; the fallback)
src/server_audio.cpp  the host's side of the audio lanes
src/api/        broremote_api, the bronze JavaScript binding (bro.remote):
                api.cpp (hosting), viewer_api.cpp (bro.remote.connect)
tools/          broremote (CLI: proxy, serve-test, codecs, encode, record,
                tone, wav-info, mic-check, audio-devices),
                connect (the --ssh/--socket target, shared), test_pattern,
                picture (decoded pictures to RGBA, PSNR)
tools/view/     broremote-view, the stopgap SDL viewer (bro's <remoteview>
                and helmapps' helmremote replace it): keymap (SDL scancode
                -> evdev), input_map, display_timing (when a present reached
                the screen, from DXGI), viewer_app (window, render loop),
                png, main; its session is the library's ViewerSession
tests/          ctest suite
```

CMake target `broremote` (alias `broremote::broremote`). Options:
`BROREMOTE_BUILD_TESTS`, `BROREMOTE_BUILD_TOOLS`, `BROREMOTE_BUILD_VIEWER`
(on when SDL3 is found), `BROREMOTE_ENABLE_API` (off; bro turns it on: builds
`broremote_api` and its test against bronze, below). Which codecs exist is
brovideo's build (`BROVIDEO_WITH_VAAPI`, auto on Linux when libva is found;
`BROVIDEO_WITH_MF`, on for Windows) and brovideo's run-time probe
(`brovideo::capabilities()`), so the codec lists the server and viewer offer
are honest on a box without the hardware.

Dependencies resolve by the ecosystem convention, cmake/bro_deps.cmake
(existing target, then the `../<name>` working tree, then the commit pinned in
CMakeLists.txt, fetched at configure). brovideo is required (on Linux it
brings brodmabuf in turn), and so is brolink. SDL3 for
the viewer: an existing `SDL3::SDL3` target, else `find_package(SDL3)`; on
Windows, when no vcpkg toolchain file points at it, the vcpkg trees at
`$VCPKG_ROOT`, `../vcpkg` and `../../vcpkg` (triplet x64-windows) are tried,
and `SDL3.dll` is copied next to the executables that load it. The viewer is
built only when SDL3 is found, so a Linux box without it builds the rest.

The C runtime: a top-level MSVC build links the CRT statically, while
vcpkg's x64-windows SDL3 is a DLL with the dynamic CRT. That mix is sound
and deliberate: SDL's API never passes CRT objects (heap blocks, FILE*)
across the DLL boundary (what it allocates is freed with `SDL_free`), so
each side keeps its own CRT, the import library adds no conflicting default
libraries, and `broremote.exe` stays free of the VC++ runtime. With
`BROREMOTE_ENABLE_API` the top-level build uses the DLL CRT instead, because
bronze's shared runtime (`bronze_runtime_shared`) hands CRT objects across its
boundary. Inside bro,
broremote sets no CRT of its own and `SDL3::SDL3` is bro's static SDL built
with bro's settings, so nothing mixes there.

## Wire format

brolink's framing, the same as bromux's (docs/protocol.md there):

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

### Lanes (1.2)

A session is a bundle of connections. The control connection says `Hello`
and its `Welcome` carries a grant (a session id and a 32-byte token from the
OS CSPRNG, brolink's `lanes::Registry`). A further connection opens with
`Join{session, token, "input"}` and becomes the session's input lane, which
carries `Input` and `Ping`. The point is that input never waits behind video:
a keyframe queued on the control connection holds up everything behind it in
the server's queue, the proxy and the ssh channel, and a lane is a separate
connection all the way (over ssh, a second ssh). The session ends with the
control connection; a lane that fails or closes only sends input back to the
control connection. Never ssh's ControlMaster: multiplexed channels share one
TCP connection and its head-of-line blocking, which is what lanes avoid.

### The audio lane (1.3)

A third connection, `Join{..., "audio"}`, carries raw PCM both ways, timestamped
on the sender's clock; docs/protocol.md has the messages. Raw first, a codec
later (a new sample-format value): at 48 kHz s16, stereo down and mono up, it
is 1.5 + 0.77 Mbit/s, nothing next to the video.

```
viewer                                         host
mic (WASAPI, Communications) --AudioUp-->  jitter buffer --> virtual source node
speakers (WASAPI)  <-- jitter buffer <--AudioDown-- default output's monitor
```

- **The viewer's mic is a microphone on the host.** The host makes a PipeWire
  node of class `Audio/Source` (an output stream, not autoconnected), named
  `broremote.mic.<viewer host>.<run>.<session>` with the description
  "broremote: <viewer host> mic", that exists only while that lane does, so
  anything recording from it (bro.mic, stt, wake, any program) hears the
  viewer. With `mic_as_default` it is the default source meanwhile: the host
  sets `default.configured.audio.source` in the "default" metadata and puts
  back the previous value (or clears it) when the last such node goes. The
  node does not write WirePlumber's per-stream restore state
  (`state.restore-props` / `state.restore-target` false). One trace does
  remain with `mic_as_default`: WirePlumber appends the node's name to its
  history of configured default sources (`default.configured.audio.source.N`
  in ~/.local/state/wireplumber/default-nodes). The name carries a random
  token drawn per server process as well as the session id (which restarts
  at 1 with each server), so a later node never matches an old entry and is
  never picked as the default unasked; the history is WirePlumber's to trim.
- **The host's audio** is the default sink's monitor (`stream.capture.sink`),
  so it is what the machine plays, bro's output included, with no
  per-application wiring. It works even with no real output: the halo's
  default sink is the null "Dummy Output", whose monitor carries everything.
- **The viewer's devices** are Windows' communications-default mic, opened
  with `AudioCategory_Communications` so Windows applies its voice processing
  (echo cancellation where the driver offers it; the render endpoint is named
  as the echo reference through `IAcousticEchoCancellationControl` when that
  exists), and the console-default speakers. Shared mode, event driven,
  `AUTOCONVERTPCM` (the device's mix format is never our problem), MMCSS
  "Pro Audio". Whether echo cancellation is on is reported, as far as
  Windows says (`IAudioEffectsManager`).
- **Jitter buffers** on both receivers: fill to a target (20 ms) before
  playing, play silence and refill after running dry (an underrun), and
  drop the oldest audio down to the target when beyond the bound (80 ms),
  so a stall or clock drift never turns into lasting latency. One
  consequence: while nothing records from the mic node, PipeWire does not
  run it, and the host's buffer drops the viewer's audio at the bound; the
  "dropped frames" count grows (by design, not loss), and the mic latency
  reads "not playing yet" until something records.
- **Latency first.** Both one-way latencies are measured, not estimated: the
  viewer keeps the host clock offset from Ping / Pong on the audio lane (the
  smallest round trip of recent samples), the downlink pairs each frame's
  host capture stamp with when the speakers play it here, and the uplink
  comes back in `AudioStats` (a viewer capture stamp and the host time its
  node played it). Nothing is batched: a packet is one device period.
- **Isolation.** The lane is its own connection with its own threads at both
  ends; its loss, a host without audio devices (`AudioStarted` with both
  directions off and why), or a device that will not open changes nothing
  for video or input. A host whose lane backs up drops its oldest unsent
  `AudioDown` (more than 16 queued) rather than queueing.

**Why not broaudio's device layer.** It is compiled into the whole broaudio
engine (synthesis, effects, spatial: far more than a lane needs), it uses SDL
on Windows (no Communications category, so no echo cancellation, and SDL
would be a library dependency where only the viewer tool may have one), and it
has no virtual source or sink monitor. broremote's rule is platform APIs and
siblings, so it has its own small layer (audio_device.h): about 400 lines each
over PipeWire and over WASAPI.

### Server to client

| Message | Body |
|---|---|
| `Welcome` | major, minor, server name; 1.2: the lane grant |
| `Joined` | (empty; 1.2) the `Join` was accepted |
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
| `Join` | (1.2) session id, token, lane name: the first message of a lane connection |

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
- **Relative motion (1.4):** a device delta in stream pixels, sent instead of
  positions while the host's pointer is locked (`CursorState::locked`, from
  1.4 the `Cursor` message's last field): a game holding a pointer lock
  reads only motion. The viewer hides and holds its own pointer meanwhile.

Text input/IME and clipboard are later minors. The server ignores an input
kind it does not know (a newer minor's) instead of treating it as an error.

## Library interfaces

### Frame

The frame, packet and picture types are brovideo's (brovideo/frame.h),
brought into broremote by name:

```cpp
using Frame = brovideo::Frame;           // dmabuf planes + acquire fence, or CPU RGBA/BGRA rows
using EncodedPacket = brovideo::Packet;  // data, keyframe, pts_ns
using DecodedFrame = brovideo::Picture;  // RGBA8 (Raw) or NV12 (MF), CPU memory or a D3D11 texture
struct CursorState { bool visible; int32_t x, y; uint32_t hotspot_x, hotspot_y; std::string shape; };
```

The fds belong to the host. The server never closes them; it holds the frame
until it calls the release callback. The viewer asks its decoders for CPU
pictures: the read-back and upload cost about 1-2 ms of a 16.7 ms frame, so
brovideo's D3D11 texture output is not used yet.

### Server (host side)

```cpp
struct ServerConfig {
    std::string socket_name = "default";   // $XDG_RUNTIME_DIR/broremote/<name>.sock
    std::vector<Codec> codecs = {Codec::HEVC, Codec::H264}; // preference order; ones this build cannot encode are skipped
    uint32_t bitrate_kbps = 20000;
    uint32_t fps = 60;
    uint32_t max_frames_in_flight = 2;     // unacked frames per client before encoding pauses
    uint32_t keyframe_interval_s = 0;      // 0: keyframes only on demand
    std::string name = "broremote";
    ServerAudioConfig audio;               // 1.3: enabled, backend, mic_as_default, jitter_ms 20,
                                           // max_buffer_ms 80, period_ms 5
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
    // The stream being sent ({codec, width, height, bitrate_kbps}), or
    // nullopt before the first frame is encoded.
    std::optional<StreamInfo> stream() const;
    // Each audio lane: the viewer's name, directions and mutes, its mic
    // node and whether it is the default source, the mic buffer's depth.
    std::vector<AudioViewer> audio_viewers() const;
};
```

Audio (src/server_audio.cpp): the backend is made at the first `AudioStart`
(PipeWire where the build has it, `BROREMOTE_PIPEWIRE`, on when pkg-config
finds libpipewire-0.3). Each lane gets an `AudioPeer` with its own mic node
and jitter buffer, fed by `AudioUp` on the I/O thread and drained on
PipeWire's realtime thread. One monitor capture is shared: its realtime
callback writes an SPSC ring, and an audio thread sends each period to every
lane that wants it as `AudioDown`, and `AudioStats` to each lane with a mic
twice a second. `Stats` counts audio lanes and the packets each way.

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
codec or bitrate change, a new `StreamConfig`). A size or bitrate change
reconfigures the encoder (`Encoder::reconfigure`); a codec change, or a
reconfigure the encoder refuses, makes a new one. A client that has not
yet had a keyframe of the current stream is sent no predicted frames. Frame
ids count every packet from 1 across streams. If the encoder cannot be made
or fails, every client gets `Error(EncoderFailed)` and is closed.

The I/O thread is brolink's event loop. Each client's outgoing messages
are handed to the loop one at a time, so a `Pong` can still go ahead of
queued video, and `FrameSent` is sent once a `Video` has wholly left for the
transport. Lane connections (above) are clients of the same loop whose
messages go to the session's input queue; `Stats` counts the lanes joined
and the inputs that came on them.

The address is brolink's `local_address("broremote", name)`, with bromux's
safeguards. On Linux the socket is created mode 0600 in a 0700 directory and
the peer's uid is checked (`SO_PEERCRED`) on both sides; without
`$XDG_RUNTIME_DIR` the directory is `/tmp/broremote-<uid>/`. On Windows (tests
and development only; the real host is bro on Linux) it is a named pipe,
`\\.\pipe\broremote-<SID>-<name>`, with a DACL granting only the user,
remote clients refused, and both ends checking the other process runs as the
same user. A second server on a live name fails ("a server is already
listening"); a stale socket file is replaced.

### Client (viewer side)

```cpp
struct ClientOptions {
    std::string name = "broremote";
    std::vector<Codec> codecs;          // sent as SetCodec after the handshake when not empty
    uint32_t max_bitrate_kbps = 0;
    uint32_t connect_timeout_ms = 10000;
    // 1.2: opens a second connection the way the first was opened (a local
    // connection, or another ssh + proxy) to carry input. Called at the
    // start of connect(), so both connections come up together; it joins
    // once Welcome grants the session. Empty, an older server or a lane
    // that fails: input goes on the control connection.
    std::function<std::unique_ptr<Stream>(std::string* err)> open_input_lane;
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
    bool input_lane() const;            // input is going on its own lane
    std::string input_lane_error() const;  // why not, when one was asked for
    bool connected() const;
    void close();
};
```

The handlers are fixed at `connect()` rather than assigned afterwards: the
reader thread starts inside `connect()`, and a `StreamConfig` or the first
`Video` can arrive before `connect()` returns, so callbacks assigned later
could miss them. For the same reason a handler that needs the `Client` (to
ack, say) must not assume `connect()` has returned yet.

### ViewerSession (viewer.h)

A viewer of any kind, minus its window: `broremote-view` and bro's
`<remoteview>` (through `bro.remote.connect`) both put their display and
input around one. `start(ViewerOptions)` opens the target (`ConnectTarget`:
`ssh HOST broremote proxy`, or a local socket) on a connect thread, then
the Client; a decode thread decodes each packet, acks it, crops the picture
to the stream's size and publishes it as the newest frame (`take_frame`
swaps it out; pictures never taken are replaced, not queued); a ping thread
keeps the round trip and clock offset; the audio lane comes up once video
is. `status()`, `stats()`, `cursor()`, `send_input()`, `probe()` /
`latency()` (the latency tracker), the audio lane's stats and mutes.

What the display needs is in the options: `output` (where pictures should
live: `PictureMemory::Cpu`, or `D3D11` to leave Media Foundation's pictures
on the GPU), `prepare` (decode thread, each picture before it is published:
bro converts a D3D11 picture into a texture Vulkan shares there, off its
own thread, and lets go of the decoder's surface), `read_marker` (decode
thread, while a latency probe is open: the input marker of a picture not
in CPU memory; bro reads 32 luma samples through a tiny staging copy) and
`log`.

### The viewer in JavaScript (src/api/viewer_api.cpp)

`bro.remote.connect(options)` makes a ViewerSession and returns its
session object: `status()`, `stats()` (rates over the interval since the
last call, and the latency tracker's means), `audio()`, `probe()` /
`probes()`, `sendInput()`, mutes, `close()`, and `state` / `config` events
(delivered by `tickRemote()`). The host shows it: `ViewerHooks`
(`sessionStarting` sets the options above before the session starts,
`sessionGone` comes just before one is destroyed, `wake` when a picture
arrives) and `viewerSession(object)`, which finds the session an object
stands for. bro's `<remoteview>` is that host (docs/remote-api.js).

### Codecs

Every encoder and decoder is brovideo's (`brovideo::create_encoder`,
`brovideo::create_decoder`; codec.h brings the names into broremote). Its
docs/design.md is the reference for the API contract (release exactly once,
keyframe rules, lost sync, picture cropping), the VA-API encoder (colour,
device choice, sequence and HRD buffer, packed headers, AV1 padding and the
radeonsi/VCN workarounds) and the Media Foundation decoder (hardware,
latency, sync checks, probe). `Codec::Raw` is brovideo's too (its
docs/raw.md); it is there so the protocol and server/client run and are
tested everywhere (CI, Windows, a box with no VA-API), not for real use.

What stays in broremote is the policy around them:

- *Which codec.* `ServerConfig::codecs` is the server's preference list;
  `Server::create` drops what brovideo cannot encode here
  (`brovideo::codecs(Direction::Encode)`) and fails when nothing is left.
  Clients narrow it with `SetCodec` (above). HEVC is the default, then H.264:
  on a LAN AV1 gains little on desktop content, and radeonsi uses none of
  AV1's screen-content tools; AV1 is offered when asked for. The viewer
  offers what brovideo can decode here (`codecs(Direction::Decode)`).
- *Keyframes.* A client joining, a `RequestKeyframe`, a new stream, or the
  optional `keyframe_interval_s` forces one; otherwise every frame is
  predicted. The viewer requests one after a failed decode (once per run of
  failures) and acks the failed frame anyway.
- *When to encode.* Only while a client is attached and the ack window is
  open (flow control, above); the host is told through `wants_frames()`.
- *Reconfigure.* A size or bitrate change reconfigures the encoder in place;
  a codec change makes a new one. Either way a new `StreamConfig` goes out.
- *Cropping.* The `StreamConfig` size is the truth: a decoded picture can be
  larger (AV1 on VCN codes 1080 lines as 1082), and the viewer crops it to
  the config's width and height.
- *Which decoder.* The viewer asks for `Hardware::Prefer` and CPU pictures
  (`$BROVIDEO_HARDWARE=0` forces software). `Decoder::describe()` goes in
  the window title and the report: whether a picture comes from the GPU or
  the CPU is the first thing to know about a slow viewer.

Measured decode (RTX 4090, Windows 11, Media Foundation, 1920x1080 to CPU
NV12): H.264 1.9-2.3 ms in hardware, HEVC 1.4-1.6 ms. Encode on the halo
(VCN 4, dmabuf to packet): 2.7-2.9 ms at 1920x1080, 4.4 ms at 2560x1440.

## Tools

- `broremote proxy [--socket NAME] [--pty]`: relays stdin/stdout to the
  local server address (brolink's relay). This is what `ssh host broremote
  proxy` runs, once per connection (a viewer with an input lane runs two).
  With `--pty` (for `ssh -tt`) it makes its terminal raw and writes a ready
  marker before any protocol byte (`run_proxy` in stream.h).
- `broremote serve-test [--socket NAME] [--size WxH] [--codec C] [--fps N]
  [--bitrate KBPS] [--seconds N] [--window N] [--content scroll|desktop]
  [--latency]`: a server fed a moving test pattern (CPU frames), so a viewer
  can be tested with no bro. On exit it prints how many input lanes joined
  and how many inputs arrived on them. `--latency` answers each key or button press
  at once (it looks for input every millisecond) with a frame whose second
  block row counts the presses, for `broremote-view --latency-test`;
  `--content desktop` is a still picture whose counter changes every frame
  and which changes whole once a second; `--window` sets the ack window. The pattern (tools/test_pattern.h) is a
  scrolling gradient, a sweeping white bar, pure red/green/blue swatches and
  the frame counter as 32 one-bit blocks, so motion, channel order and
  dropped frames all show. With a codec this machine cannot encode it says
  so and exits 1. Input it receives is printed to stderr; pointer motion
  moves the cursor it reports. Audio lanes are on where the platform backend
  is (`--no-audio` turns them off; `--mic-default` makes each viewer's mic
  the default source); each change in the audio lanes is printed.
- `broremote tone --out WAV [--hz F] [--amplitude A] [--seconds S]`: writes
  a sine WAV (for `--mic-file`). `broremote wav-info WAV`: the level and
  dominant frequency of the part with sound in it (or "digital silence").
- `broremote audio-devices`: the mics and speakers here, by the names
  `--mic-device` / `--speaker-device` match. `broremote mic-check [--seconds
  S] [--out WAV] [--play-tone HZ] [--device NAME]`: records the mic an audio
  lane would send (the communications default) and prints its level and
  whether echo cancellation is on; `--play-tone` plays a tone meanwhile,
  so the recording shows what echo cancellation leaves of it.
- `broremote codecs`: what this build can encode and decode here.
- `broremote encode [--codec C] [--size WxH] [--frames N] [--bitrate KBPS]
  [--fps N] [--keyframe-every N] [--out FILE] [--content scroll|desktop]`:
  the test pattern straight through an encoder, no server; writes the
  elementary stream and prints the per-frame encode time and the packet
  sizes by kind (keyframes, scene changes, quiet frames).
- `broremote record [--ssh HOST [--ssh-command CMD] | --socket NAME]
  [--codec C] [--frames N] --out FILE`: connects as a viewer and writes the
  bitstream from the first keyframe to a file, acking each packet: what a
  server really sends (the test fixtures were made this way).
- `broremote-view [--ssh HOST [--ssh-command CMD] | --socket NAME]
  [options]`: the viewer, below.

### broremote-view

`--ssh HOST` runs `ssh -T -o BatchMode=yes HOST <cmd>` (`cmd` defaults to
`broremote proxy`; `--ssh-command` replaces it; with `--socket NAME` and no
`--ssh-command` the proxy gets `--socket NAME`). BatchMode because ssh runs
with no console to prompt on: a password or host-key question fails at once
with its reason instead of hanging. `--socket NAME` alone connects locally.

Input lane (protocol 1.2): the viewer opens its input lane the same way as
the control connection, so with `--ssh` it runs a second ssh (same program,
options and command) and locally it makes a second connection; both start
together, so the lane costs no extra connect time. Stderr says "input on
its own lane", or why not (an older server, the second ssh failing); input
then goes on the control connection as before. `--no-input-lane` turns it
off.

Audio lane (protocol 1.3): once video is up, the viewer opens a third
connection the same way on its own thread and starts an `AudioSession`
(audio_client.h, a library part: no SDL in it). Flags: `--no-audio-lane`,
`--no-mic` (hear only), `--no-audio-playback` (send only), `--mic-device` /
`--speaker-device NAME` (default: the communications mic, the default
output), `--mic-tone HZ` / `--mic-file WAV` (sent instead of the mic, paced
in real time), `--record-audio WAV` (what the speakers were handed),
`--audio-buffer MS`, `--audio-stats` (a line a second). Ctrl+Alt+M mutes the
mic and Ctrl+Alt+A the host's audio (not forwarded); the title shows both
latencies, and the closing report gives each latency's mean / min / max with
the buffers' underruns and drops.

Which ssh: `--ssh-program`, else `$BROREMOTE_SSH`, else on Windows the
system's own OpenSSH (`%SystemRoot%\System32\OpenSSH\ssh.exe`) when it is
there, else `ssh` from PATH. From a shell, PATH's ssh is often Git's MSYS
build, whose emulated `select()` on the viewer's pipe holds each small
write (input, acks, pings) for up to a timer tick: measured, a 5 ms mean
and 15 ms worst round trip and 16 ms key press to picture, against 0.7 /
1.5 ms and 11 ms with the system's ssh.

`--ssh-pty` runs `ssh -tt -e none -o ObscureKeystrokeTiming=no` and `proxy
--pty` instead: OpenSSH sets `TCP_NODELAY` (and the low-delay IPQoS) only
on a session with a terminal, so this is the way to get Nagle off at both
ends; the proxy makes the terminal raw and the viewer sends nothing until
it has read the proxy's ready marker (`await_proxy_ready`), so the cooked
terminal never sees a protocol byte. It is not the default because it
measured no different: with video flowing both ways, and also with a quiet
stream and the pointer moving constantly (`--latency-motion`, the case
where Nagle would hold small writes), the key-press-to-picture time and
the round trip were the same within noise either way.

Threads: the render (main) thread owns the SDL window and does nothing that
blocks on the network or the decoder. A connect thread opens the stream and
the `Client` (and meanwhile probes the decoders), so the window shows
"connecting" at once. The Client's reader queues configs and packets for a
decode thread (`tools/view/session.cpp`), which makes a decoder per codec
(kept across size changes, which it handles in band), decodes each packet,
acks it at once, decoded or not, and on a failure requests a keyframe (once
per run of failures, until a keyframe arrives), crops the picture to the
`StreamConfig` size and publishes it in a one-slot mailbox. The render
thread takes the newest picture when woken (an SDL user event, at most one
queued), so it never shows a queue of old frames; buffers are swapped, not
copied or reallocated. Acking after decode rather than after display keeps
the server's two-frame window moving even when presentation waits for
vsync.

Display: NV12 goes straight into an SDL NV12 streaming texture created
with the BT.709 limited-range colour space (`SDL_UpdateNVTexture`, no CPU
conversion); Raw's RGBA into an RGBA32 texture. The picture is drawn
letterboxed into the window (resizable; on the first stream the window
fits it, up to 90% of the display), linear scaling, vsync on unless
`--no-vsync`. Vsync stays on: measured with the swap chain's own frame
statistics, present-to-shown was the same with it off (8.7 ms mean
windowed under DWM, 3.5 ms fullscreen, on a 360 Hz display; the window's
DWM composition is the difference), and SDL's D3D11 vsync-off present
(`DXGI_PRESENT_DO_NOT_WAIT`) can drop a present outright, which on a
desktop that then stops changing would leave the last change unshown. Ctrl+Alt+Enter toggles fullscreen (not forwarded), and in
fullscreen the keyboard is grabbed so system shortcuts go to the remote
session. A zero-copy D3D11 path was not needed: the read-back and upload
cost about 1-2 ms of a 16.7 ms frame.

Input (`input_map.cpp`, `keymap.cpp`): keys from SDL scancodes (USB HID
usages, the same on every platform) to evdev `KEY_*` through a full
table (letters, digits, F1-F24, modifiers, navigation, keypad,
punctuation, ISO/JIS/Korean keys, media keys; checked against
linux/input-event-codes.h); auto-repeat is not forwarded (the host repeats).
Buttons to `BTN_LEFT/RIGHT/MIDDLE/SIDE/EXTRA`; a click first moves the
pointer to where it happened. Pointer positions map from window
coordinates through the letterbox to stream pixels, clamped to the picture
(motion over the bars pins to the edge). The wheel goes in 120ths, SDL's +y
up negated to the protocol's +y down, with fractions of a 120th carried to
the next event. Only what was pressed in the window is released, and focus
loss releases every held key and button.

Status: the window title shows the target and state (connecting, codec,
size, displayed fps, decode time, decoder), and before the first picture
the window says it in text. A connection that fails or ends exits with the
reason (ssh's stderr included, e.g. "broremote: command not found");
exit code 1 for a failure, 0 for a user close or the server's clean
shutdown. A server that cannot send any codec this machine decodes is
named as such (with the decoders here); with `--any-codec` (no `SetCodec`)
a stream it cannot decode says "this machine cannot decode the server's
h264 stream". Server `Cursor` messages are logged.

Timing (protocol 1.1): the session pings four times a second; the Pong
gives the round trip and the offset between the clocks, and with each
Video's server timing and FrameSent the tracker (`latency.cpp`) places
every presented frame's age: server queue, encode, wait to send, net (the
server's socket through ssh to fully received here, one way), decode wait,
decode, present, and then present-to-shown from DXGI. The title shows the
age and its main parts each second; `--stats` prints the whole line.
`--latency-test N` against `serve-test --latency` closes the loop: it sends
a press and release of `KEY_F13` (about four a second, one at a time),
watches decoded pictures' input-marker row for the count that answers it,
and reports input-to-decoded and input-to-presented with the mean parts
(uplink plus the server noticing, queue, encode, wait, net, decode,
present). `--latency-motion` keeps the pointer moving while it probes.

Scripting: `--frames N` exits after N pictures were shown (`--timeout S`
fails if they were not), `--dump-png FILE` writes the last picture shown,
`--check-pattern` compares it with serve-test's pattern (counter, luma and
RGB PSNR) and reads the window back to check the colour swatches on
screen. Every run ends with a report: pictures decoded/failed/shown,
keyframe requests, fps, Mbit/s, decode time and packet-received-to-presented
latency (mean, p50, p99).

Measured from Windows (RTX 4090) to the halo's `serve-test --size
1920x1080 --fps 60` over ssh on the LAN: H.264 900 of 900 pictures shown at
60.1 fps, 19.7 Mbit/s, decode mean 3.0 ms (p99 7.4), received-to-presented
mean 3.4 ms (p99 8.2); HEVC 60.3 fps, decode 2.0 ms (p99 2.6),
received-to-presented 2.5 ms; the pattern matches at 57-58 dB luma PSNR and
the on-screen swatches are exact.

### Latency, measured

Windows (RTX 4090, 360 Hz display) to the halo's `serve-test --latency
--size 1920x1080 --codec hevc --fps 60` over ssh on the LAN, 100 probes,
key press to the answering picture presented (mean / p90 / max, ms):

| Setup | scrolling pattern | desktop content |
|---|---|---|
| Before: Git's ssh, -T, 0.5 s HRD | 16.5 / 22.6 / 26.8 | |
| Windows' own ssh | 10.5-11.5 / 13.0-13.8 / 19 | 9.3 / 11.7 / 17 |
| ... and the 50 ms HRD | | 9.5 / 11.7-12.4 / 16 |

Mean parts with the system's ssh: uplink and the server noticing 1.4-1.6
(round trip 0.3-0.5, the rest serve-test's 1 ms input poll), queue 0.5
(the encoder still busy with the previous frame), encode 4.5-5 (CPU
frames, upload included), wait 0.01, net 0.4-0.8, decode 2-3.5 (MF,
read back to the CPU included), present call 0.35; then 6-9 ms to the
screen windowed (3.5 fullscreen). The ack window of 2 closed on 2-3 frames
in 1500; a window of 1 closes on 49 and adds 1.1 ms, so acking on receipt
instead of after decode, or a wider window, would change nothing at 60 fps.

The bro viewer (helmapps' helmremote, `bro helmremote HOST -s lat
--latency-test 100`: a `<remoteview>` over `bro.remote.connect`) against the
SDL viewer, back to back on the same server (scrolling pattern, mean / p90 /
max, ms):

| Run | SDL viewer | bro viewer |
|---|---|---|
| 1 | 11.55 / 14.71 / 24.17 | 11.4 / 14.1 / 15.9 |
| 2 | 11.82 / 14.74 / 16.69 | 11.9 / 14.1 / 19.9 |
| 3 | 11.83 / 15.10 / 26.19 | 12.1 / 14.7 / 16.9 |

The parts differ in two places that roughly cancel. bro's decode is 2.9-3.1
ms against 3.7-3.8, because its pictures stay on the GPU: the decoder's D3D11
texture converts on the GPU into a BGRA texture that Vulkan imports and
composites, with nothing read back. bro's present is 1.2-1.35 ms against
0.35, because the SDL viewer presents when a picture is decoded, while bro's
loop presents every refresh (FIFO, 2.8 ms at 360 Hz): a picture that arrives
mid-refresh waits for the next pass. A loop that held unchanged frames on
Windows and woke for a picture would recover that millisecond.

With the audio lane carrying a tone up and the host's audio down at the same
time (the setup above, scrolling pattern, two runs each way): 11.6 and 11.9 ms
mean against 12.5 and 12.5 without it; the parts are the same within noise.

### Audio, measured

Windows to the halo over ssh (the system's ssh, a tone of 0.5 amplitude in
place of the mic, `serve-test --mic-default`), each one-way latency from
capture to output at the other end:

- **Mic to the host's node:** 440.15 Hz at -9.03 dBFS, exactly what was sent,
  recorded from the node with `pw-record`; bro.mic in a headless bro on the
  halo (the node being the default source) hears 440.0 Hz at -9.03 dBFS,
  peak 0.500. Latency 24.7-25.2 ms with the host buffer at 9-14 ms (another
  run: mean 30.6, 29.7-32.1).
- **Host to the speakers:** a 1000 Hz tone played on the halo (`pw-play`)
  arrives at 1000.00 Hz, peak 0.250 (as played). Latency 34-42 ms; about 10
  of it is the playback buffer here, and the rest the host's monitor period,
  the Voicemeeter virtual device's own buffering and WASAPI's.
- Round trip on the lane 1.8-2.4 ms; no underruns either way.

In process (tests/test_audio.cpp, paced 10 ms periods): 20-25 ms up, about 30
down, with the reported values within 0.6 ms of the measured onsets.

The real mic could not be checked on the development machine: every capture
endpoint there is a Voicemeeter bus, and none carried sound (ffmpeg's dshow
capture agrees), so the run used `--mic-tone`; for the same reason echo
cancellation reads "not reported" (virtual devices offer none).

## The bro side (not in this repo)

bro links broremote under `BRO_WITH_REMOTE`. A small host adapter in bro
submits each composited frame from the KMS presenter (the scanout buffer is a
GBM dmabuf already, with its render-done fence) as soon as its GPU work is
done, before the page flip, while `wants_frames()` is true,
submits the current frame when `wants_frames()` turns true even if nothing
was redrawn (a joining viewer needs a keyframe, and the server keeps no
frame of its own), and feeds drained input to the engine's DRM input path, so remote input routes
to the shell or a client exactly as local input does. helm decides whether to
host, through broremote's JavaScript binding (`broremote_api`, optional,
gated by `BROREMOTE_ENABLE_API`, mounted by bro's `installSiblingApis`).

### The JavaScript binding (src/api)

`bro.remote` owns the Server: `host({socket, codecs, bitrateKbps, fps,
name, audio, micAsDefault})` makes one (the same options again keep it; others replace it),
`stop()` destroys it, `status()` reports it (clients, the stream from
`Server::stream()`, `stats()`, and `audio`: enabled, micAsDefault and the
`audio_viewers()`), `codecs()` is what brovideo can encode here
(`brovideo::codecs(Direction::Encode)`), and
`attach` / `detach` events fire as the client count changes. The binding
never names the host: the host sets `HostHooks` before `installRemote()`,
and `serverChanged(server, config)` tells it when a server starts and just
before one is destroyed; between the two the host submits frames, drains
input and sets the cursor itself. `tickRemote()` (once per host frame)
delivers the events; `shutdownRemote()` stops the server before the host
goes away. One server per process: a page reload keeps it, and the new
realm's `status()` sees it. bro's docs/remote-api.js is the reference.

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
- `broremote proxy` as a child stream relaying a whole session three ways
  (plain pipes; `--pty` over pipes; on POSIX `--pty` on a real terminal in
  its default cooked mode, which the Raw frames' arbitrary bytes must
  cross untouched), and `serve-test` end to end (tests/test_proxy.cpp).
- Protocol 1.1 timing: Ping / Pong, every Video's timing inside the Pongs
  around it, FrameSent per frame in order, none to a 1.0 client, and the
  window-wait count (tests/test_server.cpp, tests/test_wire.cpp).
- Protocol 1.2 lanes: the Welcome grant and Join round-trip; a Client with
  an opener sends its input on the lane, one without or with a failing
  opener on the control connection; every refusal (wrong token, unknown
  lane, no such session, a lane joined twice, Join after Hello); Ping and
  unknown messages on a lane; the control connection closing ends the lane
  (tests/test_server.cpp, tests/test_wire.cpp); and the lane through a
  second `broremote proxy` (tests/test_proxy.cpp). The transport itself is
  tested in brolink.
- Hardware streaming (Linux with a hardware encoder; skipped otherwise),
  per codec brovideo reports in hardware, ffmpeg decoding when it is on PATH
  (tests/test_hw_stream.cpp): a Server to a Client in process with a
  keyframe request halfway, and `broremote serve-test` in its own process to
  a Client; every picture compared with the source frame its counter names
  (luma PSNR >= 35 dB, RGB within 5 dB of the uncoded 4:2:0 ceiling), whole
  and from each keyframe alone. The encoders and decoders themselves (quality
  bars, dmabuf frames, latency, the Media Foundation fixtures) are tested in
  brovideo.
- The viewer (tests/test_view.cpp, wherever SDL3 is found): an in-process
  Server streams Raw to the real `Viewer` on SDL's offscreen driver in a
  1000x1000 window (the 640x360 picture letterboxed); injected SDL events
  must arrive at the server as exactly the expected InputEvents: pointer
  mapping and clamping, buttons, wheel units and fractions, key codes, no
  auto-repeat, the fullscreen hotkey kept local, release on focus loss; then
  a stream size change and the mapping following it.
- The binding (tests/test_api.cpp, `BROREMOTE_ENABLE_API`; builds bronze
  and brass): a bronze realm with the hooks set and `bro.remote`
  installed; option errors, host / same-options no-op / replace / stop and
  the hook calls each makes, attach and detach from a real Client, a CPU
  frame through the server the hook handed over and the stream it shows in
  `status()`, listeners added and removed, a GC-stress loop, and
  `shutdownRemote()`; the audio options and `status().audio`.
- Audio (tests/test_audio.cpp): PCM encode/decode and channel conversion,
  the ring and the jitter buffer (priming, underrun, the bound), the audio
  wire messages; then a loopback with paced devices in process: a Server
  and an `AudioSession` over a real local connection, a 440 Hz tone up and
  1000 Hz down at once, each checked for frequency (within 1 Hz), level
  (within 0.5 dB) and onset latency (both reported latencies within 5 ms of
  the measured onsets); mutes; the audio lane ending while the control
  connection and input go on, and a second audio lane refused; the control
  connection ending the audio lane; a host with audio off; and the lane's
  protocol errors (a wrong token; `AudioUp` before `AudioStart`).
  The platform backends are checked by hand (numbers above).
- End to end: `serve-test` on the halo, `broremote-view --ssh halo
  --frames N --check-pattern` on Windows (by hand; numbers above).
