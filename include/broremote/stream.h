#pragma once
// Blocking byte streams, the server's address, and child processes. The
// transport is brolink's (brolink/stream.h, paths.h, proxy.h); this names it
// for broremote.
//
// A Stream is read by one thread and written by others (writes are
// serialised by the caller); shutdown() from any thread unblocks a pending
// read. Kinds: a connection to a local server, a child process's
// stdin/stdout (how a remote viewer runs `ssh host broremote proxy`), and
// this process's own stdin/stdout (the proxy's end).
//
// Where the server called <name> listens:
//   Linux    a Unix socket, $XDG_RUNTIME_DIR/broremote/<name>.sock (the
//            directory 0700, the socket 0600; without XDG_RUNTIME_DIR,
//            /tmp/broremote-<uid>/), and each side checks the peer runs as
//            the same uid (SO_PEERCRED).
//   Windows  a named pipe, \\.\pipe\broremote-<user SID>-<name>, with a DACL
//            that admits only the user, remote clients refused, and the
//            client checks the process serving it runs as the same user.
//            Hosting on Windows is for tests and development: the real host
//            is bro on Linux.

#include <brolink/stream.h>

#include <memory>
#include <string>
#include <string_view>

namespace broremote {

using brolink::Process;
using brolink::Stream;
using brolink::spawn_stream;
using brolink::stdio_stream;

inline constexpr std::string_view kDefaultSocketName = "default";

// A socket name is 1-64 of [A-Za-z0-9._-], not starting with '.'.
[[nodiscard]] bool valid_socket_name(std::string_view name) noexcept;

// The address of server `name` (a socket path, or a pipe name on Windows),
// creating the private directory a socket lives in. Empty (with *err) on
// failure.
std::string socket_path(std::string_view name, std::string* err = nullptr);

// Connect to the server called `name`. On failure returns null and sets
// `not_running` when nothing is listening there.
std::unique_ptr<Stream> connect_local(std::string_view name, std::string* err = nullptr,
                                      bool* not_running = nullptr);

// `broremote proxy`: relay this process's stdin/stdout to the server called
// `name` until either side closes. Returns the process exit code.
//
// `pty`: the proxy runs on a terminal ssh allocated (`ssh -tt`, which is
// what makes OpenSSH turn Nagle off and mark the session low-delay). It
// then puts that terminal in raw mode (no echo, no line editing, no
// character translation: a binary-clean pipe), and only then writes
// kProxyReady, before any protocol byte. A viewer must not send until it
// has read it (await_proxy_ready), or the terminal's cooked mode would echo
// and edit what it sent. Errors go to stdout before the marker too, since
// a terminal merges stderr into it.
int run_proxy(std::string_view name, std::string* err, bool pty = false);

inline constexpr std::string_view kProxyReady = "\nbroremote-proxy-ready\n";

// Wraps a stream to `broremote proxy --pty`: the first read or write first
// reads up to and including kProxyReady, discarding it. If the stream ends
// first, the first write fails and diagnostics() includes what came instead
// (the proxy's error, or the remote shell's).
std::unique_ptr<Stream> await_proxy_ready(std::unique_ptr<Stream> inner);

}  // namespace broremote
