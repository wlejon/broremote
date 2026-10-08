// Windows sockets for the server loop and the client stream: AF_UNIX stream
// sockets (Windows 10 1803 and later), WSAPoll, and a loopback-TCP waker.
#include "net.h"
#include "win_util.h"

#include <afunix.h>
#include <ws2tcpip.h>

#include <cstring>
#include <mutex>

#ifndef SIO_AF_UNIX_GETPEERPID
#define SIO_AF_UNIX_GETPEERPID _WSAIOR(IOC_VENDOR, 256)
#endif

namespace broremote::net {

namespace {

std::string wsa_text(int e) { return win::error_text(DWORD(e)); }

bool make_addr(const std::string& path, sockaddr_un& sa, std::string* err) {
    sa = {};
    sa.sun_family = AF_UNIX;
    if (path.size() >= sizeof sa.sun_path) {
        if (err) *err = "socket path too long: " + path;
        return false;
    }
    std::memcpy(sa.sun_path, path.c_str(), path.size() + 1);
    return true;
}

bool set_nonblocking(SOCKET s) {
    u_long one = 1;
    return ioctlsocket(s, FIONBIO, &one) == 0;
}

void no_inherit(SOCKET s) { SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0); }

}  // namespace

bool init(std::string* err) {
    static std::once_flag once;
    static int result = 0;
    std::call_once(once, [] {
        WSADATA wsa;
        result = WSAStartup(MAKEWORD(2, 2), &wsa);
    });
    if (result != 0) {
        if (err) *err = "WSAStartup: " + wsa_text(result);
        return false;
    }
    return true;
}

sock_t listen_at(const std::string& path, std::string* err) {
    if (!init(err)) return kInvalidSocket;
    sockaddr_un sa;
    if (!make_addr(path, sa, err)) return kInvalidSocket;
    const std::wstring wpath = win::to_wide(path);
    if (GetFileAttributesW(wpath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        bool not_running = false;
        sock_t probe = connect_to(path, nullptr, &not_running);
        if (probe != kInvalidSocket) {
            closesocket(SOCKET(probe));
            if (err) *err = "a server is already listening at " + path;
            return kInvalidSocket;
        }
        DeleteFileW(wpath.c_str());
    }
    SOCKET s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) {
        if (err) *err = "socket(AF_UNIX): " + wsa_text(WSAGetLastError());
        return kInvalidSocket;
    }
    no_inherit(s);
    if (bind(s, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
        const int e = WSAGetLastError();
        closesocket(s);
        if (err) *err = "cannot bind " + path + ": " + wsa_text(e);
        return kInvalidSocket;
    }
    if (listen(s, 16) != 0 || !set_nonblocking(s)) {
        const int e = WSAGetLastError();
        closesocket(s);
        DeleteFileW(wpath.c_str());
        if (err) *err = "cannot listen on " + path + ": " + wsa_text(e);
        return kInvalidSocket;
    }
    return sock_t(s);
}

void remove_path(const std::string& path) { DeleteFileW(win::to_wide(path).c_str()); }

sock_t accept_one(sock_t listener) {
    SOCKET s = accept(SOCKET(listener), nullptr, nullptr);
    if (s == INVALID_SOCKET) return kInvalidSocket;
    no_inherit(s);
    set_nonblocking(s);
    return sock_t(s);
}

sock_t connect_to(const std::string& path, std::string* err, bool* not_running) {
    if (not_running) *not_running = false;
    if (!init(err)) return kInvalidSocket;
    sockaddr_un sa;
    if (!make_addr(path, sa, err)) return kInvalidSocket;
    SOCKET s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) {
        if (err) *err = "socket(AF_UNIX): " + wsa_text(WSAGetLastError());
        return kInvalidSocket;
    }
    no_inherit(s);
    if (connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
        const int e = WSAGetLastError();
        closesocket(s);
        if (e == WSAECONNREFUSED ||e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
            if (not_running) *not_running = true;
            if (err) *err = "no server is listening at " + path;
        } else if (err) {
            *err = "cannot connect to " + path + ": " + wsa_text(e);
        }
        return kInvalidSocket;
    }
    return sock_t(s);
}

bool peer_is_same_user(sock_t s) {
    ULONG pid = 0;
    DWORD got = 0;
    if (WSAIoctl(SOCKET(s), SIO_AF_UNIX_GETPEERPID, nullptr, 0, &pid, sizeof pid, &got, nullptr, nullptr) != 0 ||
        pid == 0) {
        return false;
    }
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return false;
    win::UniqueHandle hold(proc);
    const std::vector<unsigned char> mine = win::current_user_sid();
    return !mine.empty() && win::process_user_sid(proc) == mine;
}

void close_socket(sock_t s) {
    if (s != kInvalidSocket) closesocket(SOCKET(s));
}

void shutdown_socket(sock_t s) {
    if (s != kInvalidSocket) shutdown(SOCKET(s), SD_BOTH);
}

long recv_some(sock_t s, char* buf, size_t n) {
    int r = recv(SOCKET(s), buf, int(n > 0x7FFFFFFF ? 0x7FFFFFFF : n), 0);
    if (r > 0) return r;
    if (r == 0) return 0;
    return WSAGetLastError() == WSAEWOULDBLOCK ? -1 : 0;
}

long send_some(sock_t s, const char* data, size_t n) {
    int r = send(SOCKET(s), data, int(n > (1u << 30) ? (1u << 30) : n), 0);
    if (r >= 0) return r;
    return WSAGetLastError() == WSAEWOULDBLOCK ? -1 : -2;
}

long recv_blocking(sock_t s, char* buf, size_t n) {
    int r = recv(SOCKET(s), buf, int(n > 0x7FFFFFFF ? 0x7FFFFFFF : n), 0);
    return r > 0 ? r : 0;
}

bool send_all(sock_t s, const char* data, size_t n) {
    while (n > 0) {
        int r = send(SOCKET(s), data, int(n > (1u << 30) ? (1u << 30) : n), 0);
        if (r <= 0) return false;
        data += r;
        n -= size_t(r);
    }
    return true;
}

void cancel_blocking(sock_t s) {
    // A blocking winsock call is an I/O request on the socket handle:
    // cancelling it fails the call at once, whatever thread is in it.
    shutdown_socket(s);
    CancelIoEx(reinterpret_cast<HANDLE>(s), nullptr);
}

bool poll(std::vector<PollItem>& items, int timeout_ms) {
    std::vector<WSAPOLLFD> fds(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        fds[i].fd = SOCKET(items[i].fd);
        fds[i].events = short((items[i].want_read ? POLLRDNORM : 0) | (items[i].want_write ? POLLWRNORM : 0));
    }
    int r = WSAPoll(fds.data(), ULONG(fds.size()), timeout_ms);
    if (r == SOCKET_ERROR) return false;
    for (size_t i = 0; i < items.size(); ++i) {
        const short re = fds[i].revents;
        items[i].readable = (re & (POLLRDNORM | POLLHUP | POLLERR | POLLNVAL)) != 0;
        items[i].writable = (re & (POLLWRNORM | POLLERR | POLLHUP)) != 0;
    }
    return true;
}

Waker::~Waker() {
    close_socket(r_);
    close_socket(w_);
}

// A connected loopback TCP pair: Windows has no pipe that WSAPoll can wait on.
bool Waker::open(std::string* err) {
    if (!init(err)) return false;
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l == INVALID_SOCKET) {
        if (err) *err = "waker socket: " + wsa_text(WSAGetLastError());
        return false;
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    int alen = sizeof a;
    SOCKET w = INVALID_SOCKET, r = INVALID_SOCKET;
    bool ok = bind(l, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 && listen(l, 1) == 0 &&
              getsockname(l, reinterpret_cast<sockaddr*>(&a), &alen) == 0;
    if (ok) {
        w = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ok = w != INVALID_SOCKET && connect(w, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
    }
    if (ok) {
        r = accept(l, nullptr, nullptr);
        ok = r != INVALID_SOCKET;
    }
    const int e = WSAGetLastError();
    closesocket(l);
    if (!ok) {
        if (w != INVALID_SOCKET) closesocket(w);
        if (r != INVALID_SOCKET) closesocket(r);
        if (err) *err = "waker: " + wsa_text(e);
        return false;
    }
    no_inherit(r);
    no_inherit(w);
    set_nonblocking(r);
    set_nonblocking(w);
    BOOL one = TRUE;
    setsockopt(w, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
    r_ = sock_t(r);
    w_ = sock_t(w);
    return true;
}

void Waker::wake() {
    const char c = 1;
    send(SOCKET(w_), &c, 1, 0);  // a full buffer already wakes the poll
}

void Waker::drain() {
    char buf[256];
    while (recv(SOCKET(r_), buf, sizeof buf, 0) > 0) {
    }
}

}  // namespace broremote::net
