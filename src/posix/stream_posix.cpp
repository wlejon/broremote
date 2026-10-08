// POSIX streams: a connection to the server's socket and this process's stdio.
#include "broremote/stream.h"
#include "net.h"
#include "posix_util.h"

#include <atomic>
#include <mutex>

namespace broremote {

namespace {

class SocketStream final : public Stream {
public:
    explicit SocketStream(int fd) : fd_(fd) {}
    ~SocketStream() override { ::close(fd_); }
    size_t read(char* buf, size_t n) override {
        if (stopped_) return 0;
        long r = net::recv_blocking(fd_, buf, n);
        return r > 0 ? size_t(r) : 0;
    }
    bool write(std::string_view data) override {
        if (stopped_) return false;
        return net::send_all(fd_, data.data(), data.size());
    }
    void shutdown() override {
        if (!stopped_.exchange(true)) ::shutdown(fd_, SHUT_RDWR);
    }

private:
    int fd_;
    std::atomic<bool> stopped_{false};
};

// This process's stdin / stdout (not owned: never closed here).
class StdioStream final : public Stream {
public:
    size_t read(char* buf, size_t n) override {
        for (;;) {
            if (stopped_) return 0;
            ssize_t r = ::read(0, buf, n);
            if (r > 0) return size_t(r);
            if (r < 0 && errno == EINTR) continue;
            return 0;
        }
    }
    bool write(std::string_view data) override {
        while (!data.empty()) {
            if (stopped_) return false;
            ssize_t r = posix::write_pipe(1, data.data(), data.size());
            if (r < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            data.remove_prefix(size_t(r));
        }
        return true;
    }
    void shutdown() override { stopped_ = true; }

private:
    std::atomic<bool> stopped_{false};
};

}  // namespace

std::unique_ptr<Stream> connect_local(std::string_view name, std::string* err, bool* not_running) {
    if (not_running) *not_running = false;
    std::string path = socket_path(name, err);
    if (path.empty()) return nullptr;
    int fd = net::connect_to(path, err, not_running);
    if (fd < 0) return nullptr;
    if (!net::peer_is_same_user(fd)) {
        ::close(fd);
        if (err) *err = "the server at " + path + " does not run as this user; refusing it";
        return nullptr;
    }
    return std::make_unique<SocketStream>(fd);
}

std::unique_ptr<Stream> stdio_stream() { return std::make_unique<StdioStream>(); }

}  // namespace broremote
