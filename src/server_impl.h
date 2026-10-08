#pragma once
// Server internals shared by server.cpp (lifecycle and the host API),
// server_io.cpp (the I/O thread) and server_encode.cpp (the encode thread).
//
// One mutex, `m`, guards all shared state. The I/O thread holds it while it
// handles readiness (every socket call there is non-blocking) and releases
// it only to poll. The encode thread holds it to take the pending frame and
// to queue a packet, never while encoding. Release callbacks never run under it.

#include "broremote/protocol.h"
#include "broremote/server.h"
#include "net.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace broremote {

using Clock = std::chrono::steady_clock;
using SharedMessage = std::shared_ptr<const std::string>;

inline constexpr size_t kMaxQueuedInput = 65536;
// A client whose unsent output grows past this is not reading (the ack window
// bounds video for a client that is): it is dropped.
inline constexpr size_t kMaxQueuedOutput = 256u << 20;

struct ClientConn {
    uint64_t id = 0;
    net::sock_t sock = net::kInvalidSocket;
    wire::MessageSplitter in;
    std::deque<SharedMessage> out;
    size_t out_offset = 0;          // bytes of out.front() already written
    size_t out_bytes = 0;           // bytes queued in `out` (including out.front()'s written part)
    bool attached = false;          // Hello answered with Welcome
    bool synced = false;            // sent a keyframe of the current stream
    std::deque<uint64_t> unacked;   // frame ids sent and not yet acked, oldest first
    std::vector<Codec> codecs;      // from SetCodec; empty: no preference
    uint32_t max_kbps = 0;          // from SetCodec; 0: no limit
    bool dead = false;              // the connection failed: drop it now
    bool closing = false;           // flush what is queued (an Error), then drop it
    Clock::time_point close_deadline{};
    std::string name;
};

struct Pending {
    Frame frame;
    std::function<void()> release;
};

struct Server::Impl {
    ServerConfig cfg;
    std::vector<Codec> server_codecs;  // cfg.codecs this build can encode, in preference order
    std::string path;
    net::sock_t listener = net::kInvalidSocket;
    net::Waker waker;
    std::thread io_thread;
    std::thread encode_thread;

    mutable std::mutex m;
    std::condition_variable encode_cv;
    bool stop = false;
    std::optional<Pending> pending;
    std::vector<std::unique_ptr<ClientConn>> clients;
    uint64_t next_client_id = 1;
    std::atomic<size_t> attached{0};

    // The stream. `stream` is the config clients were last sent; the encode
    // thread changes it. `want_codec` / `want_kbps` are what the clients'
    // SetCodec ask for; a difference from the encoder's makes a new stream.
    std::optional<StreamConfig> stream;
    uint32_t stream_kbps = 0;          // the bitrate `stream` was made with
    Codec want_codec = Codec::Raw;
    uint32_t want_kbps = 0;
    bool keyframe_requested = false;
    uint64_t next_frame_id = 1;
    uint64_t stream_counter = 0;
    Clock::time_point last_keyframe{};

    CursorState cursor;
    bool cursor_set = false;

    Stats stats;

    std::mutex input_m;
    std::vector<InputEvent> input;

    // ---- server_io.cpp (I/O thread; `m` held unless noted) ----
    void io_loop();                                  // takes `m` itself
    void handle_message(ClientConn& c, uint16_t type, std::string_view payload);
    void handle_hello(ClientConn& c, std::string_view payload);
    void handle_set_codec(ClientConn& c, std::string_view payload);
    void close_client(ClientConn& c, ErrorCode code, const std::string& message);
    void drop_unsent(ClientConn& c);
    void read_client(ClientConn& c);
    void flush_client(ClientConn& c);
    void shutdown_clients();
    // Recompute want_codec / want_kbps from the attached clients' SetCodec.
    // False when no codec suits every client.
    bool choose_codec();

    // ---- server_encode.cpp (encode thread) ----
    void encode_loop();                              // takes `m` itself

    // ---- shared (`m` held) ----
    [[nodiscard]] bool paused() const;               // a client is at its ack window
    void queue(ClientConn& c, SharedMessage msg);    // append to c.out; the caller wakes the I/O thread
    void queue_attached(const SharedMessage& msg);   // to every attached client
};

// Calls a release callback exactly once, however the encode goes.
class ReleaseOnce {
public:
    explicit ReleaseOnce(std::function<void()> f) : f_(std::move(f)) {}
    ~ReleaseOnce() { run(); }
    void run() {
        if (f_) {
            auto f = std::move(f_);
            f_ = nullptr;
            f();
        }
    }
    [[nodiscard]] bool done() const { return !f_; }

private:
    std::function<void()> f_;
};

}  // namespace broremote
