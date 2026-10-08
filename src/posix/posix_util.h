#pragma once
// Small POSIX helpers shared by the POSIX platform files.

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace broremote::posix {

inline std::string errno_text(int e) { return std::string(std::strerror(e)) + " (" + std::to_string(e) + ")"; }

inline bool set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL);
    return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

inline void set_cloexec(int fd) {
    int fl = fcntl(fd, F_GETFD);
    if (fl >= 0) fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

// Make a socket never raise SIGPIPE where the platform has a per-socket switch.
inline void no_sigpipe(int fd) {
#if defined(SO_NOSIGPIPE)
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
    (void)fd;
#endif
}

inline int send_flags() {
#if defined(MSG_NOSIGNAL)
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

// write() to a pipe without SIGPIPE killing the process when the reader is
// gone: SIGPIPE is blocked on this thread for the call, and a SIGPIPE it
// raised is consumed before unblocking.
inline ssize_t write_pipe(int fd, const void* data, size_t n) {
    sigset_t pipe_set, old;
    sigemptyset(&pipe_set);
    sigaddset(&pipe_set, SIGPIPE);
    sigset_t pending_before;
    sigpending(&pending_before);
    const bool was_pending = sigismember(&pending_before, SIGPIPE) == 1;
    pthread_sigmask(SIG_BLOCK, &pipe_set, &old);
    ssize_t r = ::write(fd, data, n);
    const int saved = errno;
    if (r < 0 && saved == EPIPE && !was_pending) {
        sigset_t pending;
        sigpending(&pending);
        if (sigismember(&pending, SIGPIPE) == 1) {
            int sig = 0;
            sigwait(&pipe_set, &sig);
        }
    }
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
    errno = saved;
    return r;
}

}  // namespace broremote::posix
