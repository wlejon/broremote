// The server's I/O thread: accept, read and dispatch client messages, and
// write queued output, all non-blocking around one poll.
#include "server_impl.h"

#include <algorithm>

namespace broremote {

namespace {

constexpr auto kCloseGrace = std::chrono::milliseconds(1000);
constexpr size_t kReadChunk = 64u << 10;
constexpr size_t kMaxReadPerPass = 4u << 20;  // then let the other clients have a turn

SharedMessage shared(std::string s) { return std::make_shared<const std::string>(std::move(s)); }

}  // namespace

void Server::Impl::io_loop() {
    std::vector<net::PollItem> items;
    std::unique_lock<std::mutex> lk(m);
    while (!stop) {
        // Poll set: the waker, the listener, then every client in order.
        items.clear();
        items.push_back({waker.read_fd(), true, false});
        items.push_back({listener, true, false});
        bool any_closing = false;
        for (auto& c : clients) {
            net::PollItem it;
            it.fd = c->sock;
            it.want_read = !c->closing;
            it.want_write = !c->out.empty();
            items.push_back(it);
            any_closing = any_closing || c->closing;
        }
        const size_t polled = clients.size();
        lk.unlock();
        const bool ok = net::poll(items, any_closing ? 50 : -1);
        lk.lock();
        if (stop) break;
        if (!ok) {
            // A poll failure is not expected; avoid spinning on it.
            lk.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            lk.lock();
            continue;
        }
        if (items[0].readable) waker.drain();

        // Clients only change on this thread, so items[2 + i] is still clients[i].
        for (size_t i = 0; i < polled; ++i) {
            ClientConn& c = *clients[i];
            if (items[2 + i].readable && !c.closing) read_client(c);
        }
        if (items[1].readable) {
            for (;;) {
                net::sock_t s = net::accept_one(listener);
                if (s == net::kInvalidSocket) break;
                if (!net::peer_is_same_user(s)) {
                    net::close_socket(s);  // another user: refused without a word
                    continue;
                }
                auto c = std::make_unique<ClientConn>();
                c->id = next_client_id++;
                c->sock = s;
                clients.push_back(std::move(c));
            }
        }
        // Write whatever is queued, including what the encode thread added.
        const auto now = Clock::now();
        for (auto& c : clients) {
            if (!c->dead && !c->out.empty()) flush_client(*c);
            if (c->closing && (c->out.empty() || now >= c->close_deadline)) c->dead = true;
        }
        // Drop the dead. One leaving may unpause encoding or change the codec choice.
        bool removed = false;
        for (auto it = clients.begin(); it != clients.end();) {
            if ((*it)->dead) {
                if ((*it)->attached && !(*it)->closing) attached.fetch_sub(1);
                net::shutdown_socket((*it)->sock);
                net::close_socket((*it)->sock);
                it = clients.erase(it);
                removed = true;
            } else {
                ++it;
            }
        }
        if (removed) {
            choose_codec();
            encode_cv.notify_all();
        }
    }
    shutdown_clients();
}

void Server::Impl::read_client(ClientConn& c) {
    char buf[kReadChunk];
    size_t total = 0;
    while (total < kMaxReadPerPass && !c.dead && !c.closing) {
        const long n = net::recv_some(c.sock, buf, sizeof buf);
        if (n < 0) break;  // nothing more now
        if (n == 0) {      // the client went away
            c.dead = true;
            break;
        }
        total += size_t(n);
        c.in.feed(buf, size_t(n));
        wire::MessageSplitter::Message msg;
        while (!c.dead && !c.closing && c.in.next(msg)) handle_message(c, msg.type, msg.payload);
        if (c.in.error()) close_client(c, ErrorCode::BadMessage, "framing error");
    }
}

void Server::Impl::flush_client(ClientConn& c) {
    while (!c.out.empty()) {
        const std::string& front = *c.out.front();
        const long n = net::send_some(c.sock, front.data() + c.out_offset, front.size() - c.out_offset);
        if (n == -1) return;  // the socket buffer is full; poll says when to go on
        if (n < 0) {
            c.dead = true;
            return;
        }
        c.out_offset += size_t(n);
        if (c.out_offset == front.size()) {
            c.out.pop_front();
            c.out_offset = 0;
        }
    }
}

void Server::Impl::close_client(ClientConn& c, ErrorCode code, const std::string& message) {
    if (c.dead || c.closing) return;
    // Nothing that is not yet on its way matters to a client being closed,
    // except a partly written message, which must finish to keep the Error
    // readable.
    if (c.out_offset == 0) c.out.clear();
    else c.out.erase(c.out.begin() + 1, c.out.end());
    c.out.push_back(shared(ErrorMsg{code, message}.encode()));
    if (c.attached) attached.fetch_sub(1);
    c.closing = true;
    c.close_deadline = Clock::now() + kCloseGrace;
    c.unacked.clear();
    encode_cv.notify_all();  // a client at its window may have been holding encoding up
}

void Server::Impl::shutdown_clients() {
    // Best effort: one non-blocking attempt to tell each client why.
    const SharedMessage bye = shared(ErrorMsg{ErrorCode::ServerShutdown, "the server is shutting down"}.encode());
    for (auto& c : clients) {
        if (!c->dead && !c->closing) {
            if (c->out_offset == 0) c->out.clear();
            else c->out.erase(c->out.begin() + 1, c->out.end());
            c->out.push_back(bye);
            flush_client(*c);
        }
        net::shutdown_socket(c->sock);
        net::close_socket(c->sock);
    }
    clients.clear();
    attached.store(0);
}

void Server::Impl::handle_message(ClientConn& c, uint16_t type, std::string_view payload) {
    const MsgType t = MsgType(type);
    if (!c.attached) {
        if (t == MsgType::Hello) handle_hello(c, payload);
        else close_client(c, ErrorCode::HelloRequired, "the first message must be Hello");
        return;
    }
    switch (t) {
        case MsgType::Hello:
            close_client(c, ErrorCode::BadMessage, "Hello sent twice");
            return;
        case MsgType::Ack: {
            AckMsg a;
            if (!a.decode(payload)) return close_client(c, ErrorCode::BadMessage, "malformed Ack");
            const bool was_full = c.unacked.size() >= cfg.max_frames_in_flight;
            while (!c.unacked.empty() && c.unacked.front() <= a.frame_id) c.unacked.pop_front();
            if (was_full && c.unacked.size() < cfg.max_frames_in_flight) encode_cv.notify_all();
            return;
        }
        case MsgType::RequestKeyframe: {
            keyframe_requested = true;
            encode_cv.notify_all();
            return;
        }
        case MsgType::Input: {
            InputMsg in;
            bool unknown_kind = false;
            if (!in.decode(payload, &unknown_kind)) {
                if (unknown_kind) return;  // a newer minor's input kind: ignored
                return close_client(c, ErrorCode::BadMessage, "malformed Input");
            }
            std::lock_guard<std::mutex> ilk(input_m);
            if (input.size() < kMaxQueuedInput) input.push_back(in.event);
            return;
        }
        case MsgType::SetCodec:
            handle_set_codec(c, payload);
            return;
        default:
            queue(c, shared(ErrorMsg{ErrorCode::UnknownMessage, "unknown message type " + std::to_string(type)}.encode()));
            return;
    }
}

void Server::Impl::handle_hello(ClientConn& c, std::string_view payload) {
    HelloMsg h;
    if (!h.decode(payload)) return close_client(c, ErrorCode::BadMessage, "malformed Hello (or not a broremote client)");
    if (h.major != kProtocolMajor) {
        return close_client(c, ErrorCode::VersionMismatch,
                            "server speaks protocol " + std::to_string(kProtocolMajor) + ".x, client " +
                                std::to_string(h.major) + "." + std::to_string(h.minor));
    }
    c.name = h.name;
    c.attached = true;
    attached.fetch_add(1);
    queue(c, shared(WelcomeMsg{kProtocolMajor, kProtocolMinor, cfg.name}.encode()));
    if (stream) queue(c, shared(stream->encode()));
    if (cursor_set) queue(c, shared(CursorMsg{cursor}.encode()));
    // A joining client starts at a keyframe: the next frame is one.
    keyframe_requested = true;
    encode_cv.notify_all();
}

void Server::Impl::handle_set_codec(ClientConn& c, std::string_view payload) {
    SetCodecMsg sc;
    if (!sc.decode(payload)) return close_client(c, ErrorCode::BadMessage, "malformed SetCodec");
    c.codecs = sc.codecs;
    c.max_kbps = sc.max_bitrate_kbps;
    if (!choose_codec()) {
        c.codecs.clear();
        c.max_kbps = 0;
        close_client(c, ErrorCode::NoCommonCodec, "none of the client's codecs can be encoded for every client");
        choose_codec();
    }
    encode_cv.notify_all();
}

bool Server::Impl::choose_codec() {
    std::optional<Codec> pick;
    for (Codec cand : server_codecs) {
        bool all = true;
        for (const auto& c : clients) {
            if (!c->attached || c->closing || c->dead || c->codecs.empty()) continue;
            if (std::find(c->codecs.begin(), c->codecs.end(), cand) == c->codecs.end()) all = false;
        }
        if (all) {
            pick = cand;
            break;
        }
    }
    uint32_t kbps = cfg.bitrate_kbps;
    for (const auto& c : clients) {
        if (c->attached && !c->closing && !c->dead && c->max_kbps) kbps = std::min(kbps, c->max_kbps);
    }
    want_kbps = kbps;
    if (!pick) return false;
    want_codec = *pick;
    return true;
}

}  // namespace broremote
