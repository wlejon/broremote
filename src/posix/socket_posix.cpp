// POSIX sockets for the server loop and the client stream.
#include "net.h"
#include "posix_util.h"

#include <poll.h>
#include <sys/stat.h>
#include <sys/un.h>

namespace broremote::net {

namespace {

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

int new_socket(std::string* err) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        if (err) *err = "socket: " + posix::errno_text(errno);
        return -1;
    }
    posix::set_cloexec(fd);
    posix::no_sigpipe(fd);
    return fd;
}

}  // namespace

bool init(std::string*) { return true; }

sock_t listen_at(const std::string& path, std::string* err) {
    sockaddr_un sa;
    if (!make_addr(path, sa, err)) return kInvalidSocket;
    // A live server answers a connect; a stale socket file refuses it.
    struct stat st {};
    if (::lstat(path.c_str(), &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            if (err) *err = path + " exists and is not a socket; refusing to replace it";
            return kInvalidSocket;
        }
        bool not_running = false;
        sock_t probe = connect_to(path, nullptr, &not_running);
        if (probe != kInvalidSocket) {
            ::close(probe);
            if (err) *err = "a server is already listening at " + path;
            return kInvalidSocket;
        }
        ::unlink(path.c_str());
    }
    int fd = new_socket(err);
    if (fd < 0) return kInvalidSocket;
    // Bind under a 0177 umask so the socket is 0600 from the start.
    const mode_t old = ::umask(0177);
    const int r = ::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa);
    const int e = errno;
    ::umask(old);
    if (r != 0) {
        ::close(fd);
        if (err) *err = "cannot bind " + path + ": " + posix::errno_text(e);
        return kInvalidSocket;
    }
    ::chmod(path.c_str(), 0600);
    if (::listen(fd, 16) != 0 || !posix::set_nonblocking(fd)) {
        const int le = errno;
        ::close(fd);
        ::unlink(path.c_str());
        if (err) *err = "cannot listen on " + path + ": " + posix::errno_text(le);
        return kInvalidSocket;
    }
    return fd;
}

void remove_path(const std::string& path) { ::unlink(path.c_str()); }

sock_t accept_one(sock_t listener) {
    for (;;) {
        int fd = ::accept(listener, nullptr, nullptr);
        if (fd >= 0) {
            posix::set_cloexec(fd);
            posix::no_sigpipe(fd);
            posix::set_nonblocking(fd);
            return fd;
        }
        if (errno == EINTR) continue;
        return kInvalidSocket;
    }
}

sock_t connect_to(const std::string& path, std::string* err, bool* not_running) {
    if (not_running) *not_running = false;
    sockaddr_un sa;
    if (!make_addr(path, sa, err)) return kInvalidSocket;
    int fd = new_socket(err);
    if (fd < 0) return kInvalidSocket;
    int r;
    do {
        r = ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa);
    } while (r != 0 && errno == EINTR);
    if (r != 0) {
        const int e = errno;
        ::close(fd);
        if (e == ENOENT || e == ECONNREFUSED) {
            if (not_running) *not_running = true;
            if (err) *err = "no server is listening at " + path;
        } else if (err) {
            *err = "cannot connect to " + path + ": " + posix::errno_text(e);
        }
        return kInvalidSocket;
    }
    return fd;
}

bool peer_is_same_user(sock_t fd) {
#if defined(SO_PEERCRED)
    struct ucred cred {};
    socklen_t len = sizeof cred;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return false;
    return cred.uid == ::getuid();
#else
    uid_t uid = 0;
    gid_t gid = 0;
    if (getpeereid(fd, &uid, &gid) != 0) return false;
    return uid == ::getuid();
#endif
}

void close_socket(sock_t s) {
    if (s >= 0) ::close(s);
}

void shutdown_socket(sock_t s) {
    if (s >= 0) ::shutdown(s, SHUT_RDWR);
}

long recv_some(sock_t s, char* buf, size_t n) {
    for (;;) {
        ssize_t r = ::recv(s, buf, n, 0);
        if (r > 0) return long(r);
        if (r == 0) return 0;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
        return 0;
    }
}

long send_some(sock_t s, const char* data, size_t n) {
    for (;;) {
        ssize_t r = ::send(s, data, n, posix::send_flags());
        if (r >= 0) return long(r);
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
        return -2;
    }
}

long recv_blocking(sock_t s, char* buf, size_t n) {
    for (;;) {
        ssize_t r = ::recv(s, buf, n, 0);
        if (r >= 0) return long(r);
        if (errno == EINTR) continue;
        return 0;
    }
}

bool send_all(sock_t s, const char* data, size_t n) {
    while (n > 0) {
        ssize_t r = ::send(s, data, n, posix::send_flags());
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data += r;
        n -= size_t(r);
    }
    return true;
}

void cancel_blocking(sock_t s) { shutdown_socket(s); }

bool poll(std::vector<PollItem>& items, int timeout_ms) {
    std::vector<pollfd> fds(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        fds[i].fd = items[i].fd;
        fds[i].events = short((items[i].want_read ? POLLIN : 0) | (items[i].want_write ? POLLOUT : 0));
    }
    int r;
    do {
        r = ::poll(fds.data(), nfds_t(fds.size()), timeout_ms);
    } while (r < 0 && errno == EINTR);
    if (r < 0) return false;
    for (size_t i = 0; i < items.size(); ++i) {
        const short re = fds[i].revents;
        items[i].readable = (re & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0;
        items[i].writable = (re & (POLLOUT | POLLERR | POLLHUP)) != 0;
    }
    return true;
}

Waker::~Waker() {
    if (r_ >= 0) ::close(r_);
    if (w_ >= 0) ::close(w_);
}

bool Waker::open(std::string* err) {
    int fds[2];
    if (::pipe(fds) != 0) {
        if (err) *err = "pipe: " + posix::errno_text(errno);
        return false;
    }
    for (int fd : fds) {
        posix::set_cloexec(fd);
        posix::set_nonblocking(fd);
    }
    r_ = fds[0];
    w_ = fds[1];
    return true;
}

void Waker::wake() {
    const char c = 1;
    // A full pipe already wakes the poll; the result does not matter.
    [[maybe_unused]] ssize_t r = posix::write_pipe(w_, &c, 1);
}

void Waker::drain() {
    char buf[256];
    while (::read(r_, buf, sizeof buf) > 0) {
    }
}

}  // namespace broremote::net
