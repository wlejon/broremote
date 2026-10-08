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
| `broremote proxy`, `broremote serve-test`, `broremote codecs` | done |
| VA-API encoder (Linux: H.264, then HEVC/AV1) | not yet |
| Media Foundation decoder + `broremote-view` (SDL3) | not yet |
| bro host adapter (in bro, `BRO_WITH_REMOTE`) | not yet |

Until the hardware codecs land, only `--codec raw` works: fine on a local
socket, far too much bandwidth for a real link.

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
`BROREMOTE_BUILD_TOOLS` (on).

## Trying it

```bash
broremote serve-test --codec raw --size 1280x720     # a server fed a moving test pattern
broremote proxy                                       # what `ssh host broremote proxy` runs
broremote codecs                                      # what this build can encode and decode
```

The server's socket is `$XDG_RUNTIME_DIR/broremote/<name>.sock` (mode 0600 in
a 0700 directory, peers checked to be the same user); `--socket NAME` picks
the name, `default` by default. Windows hosts too, with an AF_UNIX socket under
`%LOCALAPPDATA%\broremote\`, but that is for tests and development: the real
host is bro on Linux.

MIT licensed.
