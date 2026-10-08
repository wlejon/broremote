#pragma once
// Blocking byte streams, the server's socket address, and child processes.
//
// A Stream is read by one thread and written by others (writes are
// serialised by the caller); shutdown() from any thread unblocks a pending
// read. Kinds: a connection to a local server socket, a child process's
// stdin/stdout (how a remote viewer runs `ssh host broremote proxy`), and
// this process's own stdin/stdout (the proxy's end).
//
// The server's socket is an AF_UNIX stream socket on every platform:
//   Linux    $XDG_RUNTIME_DIR/broremote/<name>.sock (the directory 0700, the
//            socket 0600; without XDG_RUNTIME_DIR, /tmp/broremote-<uid>/),
//            and each side checks the peer runs as the same uid (SO_PEERCRED).
//   Windows  %LOCALAPPDATA%\broremote\<name>.sock (AF_UNIX exists since
//            Windows 10 1803), inside the user's profile; the client checks
//            the server process runs as the same user. Hosting on Windows is
//            for tests and development: the real host is bro on Linux.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace broremote {

class Stream {
public:
    virtual ~Stream() = default;
    // Blocks until data arrives. Returns the bytes read; 0 at end of stream,
    // on error or after shutdown().
    virtual size_t read(char* buf, size_t n) = 0;
    // Writes everything (blocking). False when the stream is gone.
    virtual bool write(std::string_view data) = 0;
    // Unblocks read() and fails further I/O. Idempotent, any thread.
    virtual void shutdown() = 0;
    // Extra text for error messages (e.g. what an ssh child said on stderr).
    [[nodiscard]] virtual std::string diagnostics() const { return {}; }
};

inline constexpr std::string_view kDefaultSocketName = "default";

// A socket name is 1-64 of [A-Za-z0-9._-], not starting with '.'.
[[nodiscard]] bool valid_socket_name(std::string_view name) noexcept;

// The socket path for server `name`, creating the private directory that
// holds it. Empty (with *err) on failure.
std::string socket_path(std::string_view name, std::string* err = nullptr);

// Connect to the server called `name`. On failure returns null and sets
// `not_running` when nothing is listening there.
std::unique_ptr<Stream> connect_local(std::string_view name, std::string* err = nullptr,
                                      bool* not_running = nullptr);

// Spawn argv[0] (searched on PATH) with its stdin/stdout as the stream; its
// stderr is collected for diagnostics(). shutdown() ends the child.
std::unique_ptr<Stream> spawn_stream(const std::vector<std::string>& argv, std::string* err = nullptr);

// This process's stdin (read) and stdout (write), in binary.
std::unique_ptr<Stream> stdio_stream();

// A plain child process (stdio inherited), for tools and tests.
class Process {
public:
    virtual ~Process() = default;
    [[nodiscard]] virtual int64_t pid() const = 0;
    virtual void kill() = 0;
    // Wait for exit; true with the exit code when it exited in time.
    virtual bool wait_for(std::chrono::milliseconds timeout, int* exit_code = nullptr) = 0;
    static std::unique_ptr<Process> spawn(const std::vector<std::string>& argv, std::string* err = nullptr);
};

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
