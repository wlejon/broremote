#pragma once
// The platform socket layer under the server's I/O loop and the client's
// stream: AF_UNIX stream sockets, non-blocking I/O, poll, and a waker that
// interrupts a poll from another thread. Implemented in
// src/posix/socket_posix.cpp and src/win/socket_win.cpp.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace broremote::net {

#if defined(_WIN32)
using sock_t = uintptr_t;  // SOCKET
inline constexpr sock_t kInvalidSocket = ~uintptr_t(0);
#else
using sock_t = int;
inline constexpr sock_t kInvalidSocket = -1;
#endif

// Process-wide socket setup (WSAStartup on Windows; a no-op elsewhere).
bool init(std::string* err);

// Create the listening socket at `path`: a stale socket file left by a dead
// server is replaced, a live server there is an error. The socket is
// non-blocking and (POSIX) mode 0600.
sock_t listen_at(const std::string& path, std::string* err);
// Remove the socket file.
void remove_path(const std::string& path);

// Accept one pending connection (non-blocking), or kInvalidSocket when none.
// The accepted socket is non-blocking.
sock_t accept_one(sock_t listener);

// Blocking connect to `path`. `not_running` is set when nothing listens there.
sock_t connect_to(const std::string& path, std::string* err, bool* not_running);

// True when the process at the other end runs as this user (uid on POSIX,
// the token's user SID on Windows).
bool peer_is_same_user(sock_t s);

void close_socket(sock_t s);
void shutdown_socket(sock_t s);

// Non-blocking I/O. recv_some: > 0 bytes read, 0 the peer closed (or an
// error), -1 nothing available now. send_some: >= 0 bytes written, -1 the
// socket buffer is full, -2 the connection is gone.
long recv_some(sock_t s, char* buf, size_t n);
long send_some(sock_t s, const char* data, size_t n);

// Blocking I/O for a blocking socket (the client side). recv_blocking: > 0
// bytes, 0 closed / error. send_all: false when the connection is gone.
long recv_blocking(sock_t s, char* buf, size_t n);
bool send_all(sock_t s, const char* data, size_t n);
// Abort a recv_blocking() another thread is in on `s`.
void cancel_blocking(sock_t s);

struct PollItem {
    sock_t fd = kInvalidSocket;
    bool want_read = false;
    bool want_write = false;
    bool readable = false;  // (or hung up / errored: the next recv says which)
    bool writable = false;
};
// Waits until an item is ready or `timeout_ms` passes (-1: no timeout).
// False on a poll failure.
bool poll(std::vector<PollItem>& items, int timeout_ms);

// Interrupts a poll() from another thread: poll its read_fd() for reading;
// wake() makes it readable; drain() resets it.
class Waker {
public:
    Waker() = default;
    ~Waker();
    Waker(const Waker&) = delete;
    Waker& operator=(const Waker&) = delete;
    bool open(std::string* err);
    void wake();
    void drain();
    [[nodiscard]] sock_t read_fd() const noexcept { return r_; }

private:
    sock_t r_ = kInvalidSocket;
    sock_t w_ = kInvalidSocket;
};

}  // namespace broremote::net
