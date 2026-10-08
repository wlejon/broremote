// Server lifecycle and the host-facing API.
#include "server_impl.h"
#include "broremote/stream.h"

#include <algorithm>

namespace broremote {

std::unique_ptr<Server> Server::create(const ServerConfig& cfg, std::string* err) {
    auto impl = std::make_unique<Impl>();
    impl->cfg = cfg;
    if (impl->cfg.max_frames_in_flight == 0) impl->cfg.max_frames_in_flight = 1;
    if (impl->cfg.fps == 0) impl->cfg.fps = 60;
    const std::vector<Codec> available = available_encoders();
    for (Codec c : cfg.codecs) {
        const bool can = std::find(available.begin(), available.end(), c) != available.end();
        const bool dup = std::find(impl->server_codecs.begin(), impl->server_codecs.end(), c) !=
                         impl->server_codecs.end();
        if (can && !dup) impl->server_codecs.push_back(c);
    }
    if (impl->server_codecs.empty()) {
        if (err) {
            *err = "none of the configured codecs (";
            for (size_t i = 0; i < cfg.codecs.size(); ++i) *err += std::string(i ? ", " : "") + codec_name(cfg.codecs[i]);
            *err += ") can be encoded in this build or on this machine";
        }
        return nullptr;
    }
    impl->want_codec = impl->server_codecs.front();
    impl->want_kbps = impl->cfg.bitrate_kbps;

    if (!net::init(err)) return nullptr;
    impl->path = broremote::socket_path(cfg.socket_name, err);
    if (impl->path.empty()) return nullptr;
    if (!impl->waker.open(err)) return nullptr;
    impl->listener = net::listen_at(impl->path, err);
    if (impl->listener == net::kInvalidSocket) return nullptr;

    Impl* p = impl.get();
    p->io_thread = std::thread([p] { p->io_loop(); });
    p->encode_thread = std::thread([p] { p->encode_loop(); });
    return std::unique_ptr<Server>(new Server(std::move(impl)));
}

Server::Server(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Server::~Server() {
    Impl& s = *impl_;
    {
        std::lock_guard<std::mutex> lk(s.m);
        s.stop = true;
    }
    s.encode_cv.notify_all();
    s.waker.wake();
    // The encode thread finishes any encode in progress (releasing its frame)
    // and exits; the I/O thread says goodbye to the clients and closes them.
    if (s.encode_thread.joinable()) s.encode_thread.join();
    if (s.io_thread.joinable()) s.io_thread.join();
    std::optional<Pending> held;
    {
        std::lock_guard<std::mutex> lk(s.m);
        held.swap(s.pending);
    }
    if (held && held->release) held->release();
    net::close_socket(s.listener);
    net::remove_path(s.path);
}

void Server::submit(const Frame& frame, std::function<void()> release) {
    Impl& s = *impl_;
    std::optional<Pending> old;
    bool now = false;
    {
        std::lock_guard<std::mutex> lk(s.m);
        ++s.stats.submitted;
        if (s.stop || s.attached.load() == 0) {
            ++s.stats.unwatched;
            now = true;
        } else {
            if (s.pending) {
                ++s.stats.replaced;
                old.swap(s.pending);
            }
            if (s.paused()) ++s.stats.window_waits;
            s.pending = Pending{frame, std::move(release), Clock::now()};
        }
    }
    if (now) {
        if (release) release();
        return;
    }
    s.encode_cv.notify_one();
    if (old && old->release) old->release();
}

void Server::drain_input(std::vector<InputEvent>& out) {
    Impl& s = *impl_;
    std::lock_guard<std::mutex> lk(s.input_m);
    out.insert(out.end(), s.input.begin(), s.input.end());
    s.input.clear();
}

void Server::set_cursor(const CursorState& cursor) {
    Impl& s = *impl_;
    {
        std::lock_guard<std::mutex> lk(s.m);
        if (s.cursor_set && s.cursor == cursor) return;
        s.cursor = cursor;
        s.cursor_set = true;
        s.queue_attached(std::make_shared<const std::string>(CursorMsg{cursor}.encode()));
    }
    s.waker.wake();
}

size_t Server::client_count() const { return impl_->attached.load(); }

bool Server::wants_frames() const { return impl_->attached.load() > 0; }

const std::string& Server::socket_path() const { return impl_->path; }

std::optional<Server::StreamInfo> Server::stream() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    if (!impl_->stream) return std::nullopt;
    StreamInfo info;
    info.codec = impl_->stream->codec;
    info.width = impl_->stream->width;
    info.height = impl_->stream->height;
    info.bitrate_kbps = impl_->stream_kbps;
    return info;
}

Server::Stats Server::stats() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    return impl_->stats;
}

// ---- shared helpers -------------------------------------------------------------------

bool Server::Impl::paused() const {
    for (const auto& c : clients) {
        if (c->attached && !c->closing && !c->dead && c->unacked.size() >= cfg.max_frames_in_flight) return true;
    }
    return false;
}

void Server::Impl::queue(ClientConn& c, SharedMessage msg) {
    if (c.dead || c.closing) return;
    c.out_bytes += msg->size();
    c.out.push_back(std::move(msg));
    if (c.out_bytes > kMaxQueuedOutput) c.dead = true;
}

void Server::Impl::queue_attached(const SharedMessage& msg) {
    for (auto& c : clients) {
        if (c->attached) queue(*c, msg);
    }
}

}  // namespace broremote
