# broremote

Remote desktop as a reusable C++20 library: a host submits its composited
frames, broremote encodes them on the GPU and streams them to viewers, and
viewers send keyboard and pointer input back. Transport security is ssh's: the
server listens only on a local socket, and a remote viewer reaches it through
`ssh host broremote proxy`.

In the [bro ecosystem](https://github.com/wlejon/bro/blob/main/docs/ecosystem.md),
broremote remotes helm, the desktop shell. It does not depend on bro or bronze.

[docs/design.md](docs/design.md) is the design and the contract between the
parts; [docs/protocol.md](docs/protocol.md) is the wire catalogue.

## Status

| Part | State |
|------|-------|
| Wire primitives, protocol, streams (local socket, ssh child, stdio) | done, tested on Windows and Linux |
| Server (I/O + encode threads, ack window, keyframes, input, cursor) and Client | done, tested on Windows and Linux |
| `Codec::Raw` (CPU RGBA, run-length + XOR-delta) and the codec factory | done |
| `broremote proxy`, `serve-test`, `codecs`, `encode`, `record` | done |
| VA-API encoder (Linux): H.264, HEVC; AV1 opt-in | done, tested on radeonsi |
| Media Foundation decoder (Windows): H.264, HEVC, AV1, D3D11 or software | done |
| `broremote-view` (SDL3): Windows decodes; Linux shows Raw only, so far | done |
| bro host adapter (in bro, `BRO_WITH_REMOTE`) | not yet |

Measured: the halo's 1920x1080 test pattern at 60 fps over ssh to a Windows
viewer runs at 60 fps with 2-3 ms decode and 2.5-3.5 ms from packet to
screen (docs/design.md has the details).

## Building

The core has no dependencies beyond a C++20 compiler, CMake 3.24 and the
platform's sockets.

Windows (Visual Studio 2022; one build dir, the config picked at build time):
```bash
cmake -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

Linux:
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

Options: `BROREMOTE_BUILD_TESTS` (on; only for a top-level build),
`BROREMOTE_BUILD_TOOLS` (on), `BROREMOTE_BUILD_VIEWER` (on when SDL3 is
found), `BROREMOTE_WITH_VAAPI` (on on Linux when libva is found),
`BROREMOTE_WITH_MF` (on on Windows), `BROREMOTE_ENABLE_API` (off; the
`bro.remote` JavaScript binding, which bro turns on; it needs ../bronze and
../brass).

SDL3 for the viewer comes from an existing `SDL3::SDL3` target or
`find_package(SDL3)`; on Windows a vcpkg tree at `$VCPKG_ROOT`, `../vcpkg` or
`../../vcpkg` (x64-windows) is found without a toolchain file, and
`SDL3.dll` is copied next to the viewer. The static CRT of a top-level MSVC
build and SDL3.dll's dynamic one coexist safely (see docs/design.md).

Tests use ffmpeg as an oracle when it is on PATH (never linked).

## Trying it

On the host (Linux with VA-API):
```bash
broremote serve-test --codec h264 --size 1920x1080   # a server fed a moving test pattern
```
On the viewer (Windows):
```bash
broremote-view --ssh HOST                             # runs `ssh -T HOST broremote proxy`
broremote-view --ssh HOST --ssh-command "~/broremote/build/broremote proxy"   # not on PATH there
broremote-view --ssh HOST --frames 600 --check-pattern --dump-png last.png    # scripted check
```
Ctrl+Alt+Enter toggles fullscreen. Elsewhere:
```bash
broremote serve-test --codec raw --size 1280x720     # Raw works everywhere, locally
broremote-view --socket default                       # a local server
broremote codecs                                      # what this build can encode and decode, and how
broremote record --ssh HOST --frames 60 --out s.h264  # capture what a server sends
```

The server's socket is `$XDG_RUNTIME_DIR/broremote/<name>.sock` (mode 0600 in
a 0700 directory, peers checked to be the same user); `--socket NAME` picks
the name, `default` by default. Windows hosts too, with an AF_UNIX socket under
`%LOCALAPPDATA%\broremote\`, but that is for tests and development: the real
host is bro on Linux.

MIT licensed.
