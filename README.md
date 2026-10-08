# broremote

Remote desktop as a reusable C++20 library: a host submits its composited
frames, broremote encodes them on the GPU and streams them to viewers, and
viewers send keyboard and pointer input back. Transport security is ssh's: the
server listens only on a local socket, and a remote viewer reaches it through
`ssh host broremote proxy`.

In the [bro ecosystem](https://github.com/wlejon/bro/blob/main/docs/ecosystem.md),
broremote remotes helm, the desktop shell. It does not depend on bro or bronze.

Status: under construction. [docs/design.md](docs/design.md) is the design.

MIT licensed.
