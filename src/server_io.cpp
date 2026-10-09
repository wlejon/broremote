// The server's I/O thread: brolink's event loop accepts connections and
// reads them; between its turns this thread hands each connection's queued
// messages to it, one at a time, and closes the connections that are done.
#include "server_impl.h"

#include <algorithm>

namespace broremote {

namespace {

constexpr auto kCloseGrace = std::chrono::milliseconds(1000);
constexpr auto kShutdownGrace = std::chrono::milliseconds(250);
constexpr int kIdleWaitMs = 250;
constexpr int kClosingWaitMs = 50;

SharedMessage shared(std::string s) { return std::make_shared<const std::string>(std::move(s)); }

}  // namespace

void Server::Impl::io_loop() {
    for (;;) {
        int wait_ms = kIdleWaitMs;
        {
            std::lock_guard<std::mutex> lk(m);
            if (stop) break;
            service_clients();
            for (const auto& c : clients) {
                if (c->closing) wait_ms = kClosingWaitMs;
            }
        }
        loop->run_once(wait_ms);
    }
    shutdown_clients();
}

ClientConn* Server::Impl::find(brolink::ConnId id) {
    for (auto& c : clients) {
        if (c->id == id) return c.get();
    }
    return nullptr;
}

void Server::Impl::on_accept(brolink::ConnId id) {
    std::lock_guard<std::mutex> lk(m);
    auto c = std::make_unique<ClientConn>();
    c->id = id;
    clients.push_back(std::move(c));
}

void Server::Impl::on_data(brolink::ConnId id, const char* data, size_t n) {
    std::lock_guard<std::mutex> lk(m);
    ClientConn* c = find(id);
    if (stop || !c || c->dead || c->closing) return;
    c->in.feed(data, n);
    wire::MessageSplitter::Message msg;
    while (!c->dead && !c->closing && c->in.next(msg)) handle_message(*c, msg.type, msg.payload);
    if (c->in.error()) close_client(*c, ErrorCode::BadMessage, "framing error");
}

void Server::Impl::on_closed(brolink::ConnId id) {
    std::lock_guard<std::mutex> lk(m);
    auto it = std::find_if(clients.begin(), clients.end(), [id](const auto& c) { return c->id == id; });
    if (it == clients.end()) return;
    if ((*it)->attached && !(*it)->closing) attached.fetch_sub(1);
    clients.erase(it);
    drop_audio_peer(id);  // an audio lane's devices go with it
    // A client's session ends with its control connection: its lanes go too.
    for (brolink::ConnId lane : lanes.closed(id)) {
        if (ClientConn* l = find(lane); l && !l->closed) {
            l->closed = true;
            loop->close(lane, false);
        }
    }
    // One leaving may unpause encoding or change the codec choice.
    choose_codec();
    encode_cv.notify_all();
}

void Server::Impl::service_clients() {
    const auto now = Clock::now();
    for (auto& c : clients) {
        if (c->closed) continue;
        if (!c->dead) flush_client(*c);
        if (c->closing && (c->out.empty() || now >= c->close_deadline)) c->dead = true;
        if (c->dead) {
            c->closed = true;
            loop->close(c->id, false);  // on_closed follows on a later turn
        }
    }
}

void Server::Impl::flush_client(ClientConn& c) {
    for (;;) {
        if (c.handed) {
            if (loop->pending_output(c.id) > 0) return;  // the loop says when it has gone
            // The front message is wholly in the transport.
            const std::string* done = c.out.front().get();
            c.out_bytes -= c.out.front()->size();
            c.out.pop_front();
            c.handed = false;
            if (!c.marks.empty() && c.marks.front().msg == done) {
                // The whole Video message is in the socket: say how long it waited and took.
                const ClientConn::SentMark mark = c.marks.front();
                c.marks.pop_front();
                FrameSentMsg fs;
                fs.frame_id = mark.frame_id;
                fs.wait_us = span_us(mark.queued, c.front_started);
                fs.write_us = span_us(c.front_started, Clock::now());
                queue(c, shared(fs.encode()));
            }
        }
        if (c.out.empty() || c.dead) return;
        c.front_started = Clock::now();
        c.handed = true;
        loop->write(c.id, *c.out.front());
    }
}

// Nothing that is not yet on its way matters to a client being closed, except
// a message the loop already has, which must finish to keep what follows readable.
void Server::Impl::drop_unsent(ClientConn& c) {
    if (!c.handed) c.out.clear();
    else c.out.erase(c.out.begin() + 1, c.out.end());
    c.out_bytes = c.out.empty() ? 0 : c.out.front()->size();
    c.marks.clear();
}

void Server::Impl::close_client(ClientConn& c, ErrorCode code, const std::string& message) {
    if (c.dead || c.closing) return;
    drop_unsent(c);
    c.out.push_back(shared(ErrorMsg{code, message}.encode()));
    c.out_bytes += c.out.back()->size();
    if (c.attached) attached.fetch_sub(1);
    c.closing = true;
    c.close_deadline = Clock::now() + kCloseGrace;
    c.unacked.clear();
    encode_cv.notify_all();  // a client at its window may have been holding encoding up
}

void Server::Impl::shutdown_clients() {
    // Best effort: tell each client why, giving the messages a moment to go.
    const SharedMessage bye = shared(ErrorMsg{ErrorCode::ServerShutdown, "the server is shutting down"}.encode());
    {
        std::lock_guard<std::mutex> lk(m);
        for (auto& c : clients) {
            if (c->dead || c->closing || c->closed) continue;
            drop_unsent(*c);
            c->out.push_back(bye);
            c->out_bytes += bye->size();
        }
    }
    const auto until = Clock::now() + kShutdownGrace;
    while (Clock::now() < until) {
        bool waiting = false;
        {
            std::lock_guard<std::mutex> lk(m);
            for (auto& c : clients) {
                if (c->closed || c->dead) continue;
                flush_client(*c);
                waiting = waiting || !c->out.empty();
            }
        }
        if (!waiting) break;
        loop->run_once(20);
    }
    std::lock_guard<std::mutex> lk(m);
    for (auto& c : clients) {
        if (!c->closed) loop->close(c->id, false);
    }
    clients.clear();
    audio_peers.clear();
    attached.store(0);
    loop->close_listener();
}

void Server::Impl::handle_message(ClientConn& c, uint16_t type, std::string_view payload) {
    if (c.lane) return handle_lane_message(c, type, payload);
    const MsgType t = MsgType(type);
    if (!c.attached) {
        if (t == MsgType::Hello) handle_hello(c, payload);
        else if (t == MsgType::Join) handle_join(c, payload);
        else close_client(c, ErrorCode::HelloRequired, "the first message must be Hello (or Join)");
        return;
    }
    switch (t) {
        case MsgType::Hello:
            close_client(c, ErrorCode::BadMessage, "Hello sent twice");
            return;
        case MsgType::Join:
            close_client(c, ErrorCode::BadMessage, "Join on a connection that sent Hello");
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
        case MsgType::Input:
            handle_input(c, payload);
            return;
        case MsgType::SetCodec:
            handle_set_codec(c, payload);
            return;
        case MsgType::Ping:
            handle_ping(c, payload);
            return;
        default:
            queue(c, shared(ErrorMsg{ErrorCode::UnknownMessage, "unknown message type " + std::to_string(type)}.encode()));
            return;
    }
}

// An input lane carries Input, and Ping to time it; the rest is the control
// connection's. An audio lane is server_audio.cpp's.
void Server::Impl::handle_lane_message(ClientConn& c, uint16_t type, std::string_view payload) {
    if (c.lane_name == kAudioLane) return handle_audio_message(c, type, payload);
    switch (MsgType(type)) {
        case MsgType::Input:
            handle_input(c, payload);
            return;
        case MsgType::Ping:
            handle_ping(c, payload);
            return;
        case MsgType::Hello:
        case MsgType::Join:
            close_client(c, ErrorCode::BadMessage, "Hello or Join on a lane");
            return;
        default:
            queue(c, shared(ErrorMsg{ErrorCode::UnknownMessage,
                                     "message type " + std::to_string(type) + " does not go on the " + c.lane_name +
                                         " lane"}
                                .encode()));
            return;
    }
}

void Server::Impl::handle_input(ClientConn& c, std::string_view payload) {
    InputMsg in;
    bool unknown_kind = false;
    if (!in.decode(payload, &unknown_kind)) {
        if (unknown_kind) return;  // a newer minor's input kind: ignored
        return close_client(c, ErrorCode::BadMessage, "malformed Input");
    }
    std::lock_guard<std::mutex> ilk(input_m);
    if (input.size() < kMaxQueuedInput) input.push_back(in.event);
    if (c.lane) ++lane_inputs;
}

void Server::Impl::handle_ping(ClientConn& c, std::string_view payload) {
    PingMsg p;
    if (!p.decode(payload)) return close_client(c, ErrorCode::BadMessage, "malformed Ping");
    // Ahead of queued video (after a message the loop already has, which must
    // finish first), so the round trip is the transport's.
    SharedMessage pong = shared(PongMsg{p.token, mono_us(Clock::now())}.encode());
    if (c.dead || c.closing) return;
    c.out_bytes += pong->size();
    c.out.insert(c.out.begin() + (c.handed ? 1 : 0), std::move(pong));
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
    c.minor = h.minor;
    c.attached = true;
    attached.fetch_add(1);
    WelcomeMsg w;
    w.name = cfg.name;
    // The client's session: further connections join it as lanes with this
    // grant. Without one (the OS refused random bytes) the client simply
    // has no lanes and sends everything on this connection.
    if (auto grant = lanes.open(c.id)) w.grant = *grant;
    queue(c, shared(w.encode()));
    if (stream) queue(c, shared(stream->encode()));
    if (cursor_set) queue(c, shared(CursorMsg{cursor}.encode()));
    // A joining client starts at a keyframe: the next frame is one.
    keyframe_requested = true;
    encode_cv.notify_all();
}

void Server::Impl::handle_join(ClientConn& c, std::string_view payload) {
    JoinMsg j;
    if (!j.decode(payload)) return close_client(c, ErrorCode::BadMessage, "malformed Join");
    if (j.join.lane != kInputLane && j.join.lane != kAudioLane) {
        return close_client(c, ErrorCode::JoinRefused, "there is no '" + j.join.lane + "' lane here");
    }
    const brolink::lanes::JoinResult r = lanes.join(c.id, j.join);
    if (r != brolink::lanes::JoinResult::Joined) {
        return close_client(c, ErrorCode::JoinRefused, brolink::lanes::join_result_name(r));
    }
    c.lane = true;
    c.lane_name = j.join.lane;
    ++stats.lanes;
    queue(c, shared(JoinedMsg{}.encode()));
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
