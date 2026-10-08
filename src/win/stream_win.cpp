// Windows streams: a connection to the server's AF_UNIX socket and this
// process's stdio.
#include "broremote/stream.h"
#include "net.h"
#include "win_util.h"

#include <atomic>

namespace broremote {

namespace {

// A connection to the server. The socket is non-blocking and every wait is a
// WSAPoll with a short timeout: a recv() blocked on a Windows AF_UNIX socket
// is not reliably woken when the peer closes (seen on Windows 11 26300: the
// FIN is there for the next call, but the blocked call sleeps on), and the
// timeout also bounds how long shutdown() takes to unblock a reader.
class SocketStream final : public Stream {
public:
    explicit SocketStream(net::sock_t s) : s_(s) {
        u_long one = 1;
        ioctlsocket(SOCKET(s_), FIONBIO, &one);
    }
    ~SocketStream() override { net::close_socket(s_); }
    size_t read(char* buf, size_t n) override {
        while (!stopped_) {
            const long r = net::recv_some(s_, buf, n);
            if (r > 0) return size_t(r);
            if (r == 0) return 0;
            wait(true);
        }
        return 0;
    }
    bool write(std::string_view data) override {
        while (!data.empty()) {
            if (stopped_) return false;
            const long r = net::send_some(s_, data.data(), data.size());
            if (r == -2) return false;
            if (r == -1) {
                wait(false);
                continue;
            }
            data.remove_prefix(size_t(r));
        }
        return true;
    }
    void shutdown() override {
        if (!stopped_.exchange(true)) net::shutdown_socket(s_);
    }

private:
    void wait(bool for_read) {
        WSAPOLLFD p{};
        p.fd = SOCKET(s_);
        p.events = for_read ? POLLRDNORM : POLLWRNORM;
        WSAPoll(&p, 1, 100);
    }

    net::sock_t s_;
    std::atomic<bool> stopped_{false};
};

// Synchronous handles (this process's stdin / stdout).
class HandleStream final : public Stream {
public:
    HandleStream(HANDLE in, HANDLE out) : in_(in), out_(out) {}
    size_t read(char* buf, size_t n) override {
        if (stopped_) return 0;
        DWORD got = 0;
        if (!ReadFile(in_, buf, DWORD(n > 0x7FFFFFFF ? 0x7FFFFFFF : n), &got, nullptr)) return 0;
        return got;
    }
    bool write(std::string_view data) override {
        while (!data.empty()) {
            if (stopped_) return false;
            DWORD put = 0;
            const DWORD chunk = DWORD(data.size() > (1u << 20) ? (1u << 20) : data.size());
            if (!WriteFile(out_, data.data(), chunk, &put, nullptr) || put == 0) return false;
            data.remove_prefix(put);
        }
        return true;
    }
    void shutdown() override { stopped_ = true; }

private:
    HANDLE in_;
    HANDLE out_;
    std::atomic<bool> stopped_{false};
};

}  // namespace

std::unique_ptr<Stream> connect_local(std::string_view name, std::string* err, bool* not_running) {
    if (not_running) *not_running = false;
    std::string path = socket_path(name, err);
    if (path.empty()) return nullptr;
    net::sock_t s = net::connect_to(path, err, not_running);
    if (s == net::kInvalidSocket) return nullptr;
    if (!net::peer_is_same_user(s)) {
        net::close_socket(s);
        if (err) *err = "the server at " + path + " does not run as this user; refusing it";
        return nullptr;
    }
    return std::make_unique<SocketStream>(s);
}

std::unique_ptr<Stream> stdio_stream() {
    return std::make_unique<HandleStream>(GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE));
}

}  // namespace broremote
