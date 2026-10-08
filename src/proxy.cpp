// `broremote proxy`: the remote end of `ssh host broremote proxy`. A byte
// relay: the protocol runs end to end between the viewer and the server, so
// the proxy reads none of it. Also the viewer's side of the --pty handshake.
#include "broremote/stream.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>

#if !defined(_WIN32)
#include <termios.h>
#include <unistd.h>
#endif

namespace broremote {

namespace {

// ssh -tt gives the proxy a terminal: make it a binary-clean pipe.
void make_stdio_raw() {
#if !defined(_WIN32)
    for (int fd : {0, 1}) {
        if (!::isatty(fd)) continue;
        termios t{};
        if (::tcgetattr(fd, &t) != 0) continue;
        ::cfmakeraw(&t);
        ::tcsetattr(fd, TCSANOW, &t);
    }
#endif
}

class ReadyStream final : public Stream {
public:
    explicit ReadyStream(std::unique_ptr<Stream> inner) : inner_(std::move(inner)) {}
    size_t read(char* buf, size_t n) override {
        if (!ready()) return 0;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (!leftover_.empty()) {
                const size_t k = std::min(n, leftover_.size());
                leftover_.copy(buf, k);
                leftover_.erase(0, k);
                return k;
            }
        }
        return inner_->read(buf, n);
    }
    bool write(std::string_view data) override { return ready() && inner_->write(data); }
    void shutdown() override { inner_->shutdown(); }
    std::string diagnostics() const override {
        std::string d = inner_->diagnostics();
        std::lock_guard<std::mutex> lk(m_);
        if (!found_ && !before_.empty()) {
            std::string said = before_;
            while (!said.empty() && (said.back() == '\n' || said.back() == '\r')) said.pop_back();
            d = d.empty() ? said : d + "\n" + said;
        }
        return d;
    }

private:
    // Reads up to the marker once; the reader and the first writer may both get here.
    bool ready() {
        std::lock_guard<std::mutex> rl(ready_m_);
        {
            std::lock_guard<std::mutex> lk(m_);
            if (found_ || failed_) return found_;
        }
        char buf[4096];
        for (;;) {
            const size_t n = inner_->read(buf, sizeof buf);
            std::lock_guard<std::mutex> lk(m_);
            if (n == 0) {
                failed_ = true;
                return false;
            }
            before_.append(buf, n);
            const size_t at = before_.find(kProxyReady);
            if (at != std::string::npos) {
                leftover_ = before_.substr(at + kProxyReady.size());
                before_.clear();
                found_ = true;
                return true;
            }
            if (before_.size() > 65536) before_.erase(0, before_.size() - 65536);
        }
    }

    std::unique_ptr<Stream> inner_;
    std::mutex ready_m_;
    mutable std::mutex m_;
    bool found_ = false, failed_ = false;
    std::string before_, leftover_;
};

}  // namespace

std::unique_ptr<Stream> await_proxy_ready(std::unique_ptr<Stream> inner) {
    if (!inner) return nullptr;
    return std::make_unique<ReadyStream>(std::move(inner));
}

int run_proxy(std::string_view name, std::string* err, bool pty) {
    std::shared_ptr<Stream> io(stdio_stream());
    if (pty) make_stdio_raw();
    std::shared_ptr<Stream> server(connect_local(name, err));
    if (!server) {
        // On a terminal stderr is stdout anyway; say it where the viewer reads.
        if (pty && err) io->write("broremote proxy: " + *err + "\n");
        return 1;
    }
    if (pty && !io->write(kProxyReady)) return 1;

    // stdin -> server on its own thread (detached: a blocked stdin read must
    // not hold up exit once the server side has gone).
    std::thread([server, io] {
        std::unique_ptr<char[]> buf(new char[64u << 10]);
        for (;;) {
            size_t n = io->read(buf.get(), 64u << 10);
            if (n == 0 || !server->write(std::string_view(buf.get(), n))) break;
        }
        server->shutdown();  // ends the other direction too
    }).detach();

    // server -> stdout here.
    std::unique_ptr<char[]> buf(new char[256u << 10]);
    for (;;) {
        size_t n = server->read(buf.get(), 256u << 10);
        if (n == 0 || !io->write(std::string_view(buf.get(), n))) break;
    }
    server->shutdown();
    return 0;
}

}  // namespace broremote
