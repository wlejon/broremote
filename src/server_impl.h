#pragma once
// Server internals shared by server.cpp (lifecycle and the host API),
// server_io.cpp (the I/O thread) and server_encode.cpp (the encode thread).
//
// The I/O thread runs brolink's event loop: the listener and every
// connection, nonblocking, with the loop's callbacks on that thread. One
// mutex, `m`, guards all shared state. The I/O thread takes it in each loop
// callback and between turns of the loop (to hand queued messages to it),
// and releases it while the loop waits. The encode thread holds it to take
// the pending frame and to queue a packet, never while encoding, and wakes
// the loop. Release callbacks never run under it.

#include "broremote/protocol.h"
#include "broremote/server.h"

#include <brolink/lanes.h>
#include <brolink/loop.h>

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

// One connection: a client's control connection (Hello), or one of its lanes
// (Join). Messages queue in `out`; the I/O thread hands the loop one at a
// time, so a Pong can still go ahead of video that is queued here, and a
// message the loop has fully taken (pending_output 0) is in the transport.
struct ClientConn {
    brolink::ConnId id = 0;
    wire::MessageSplitter in;
    std::deque<SharedMessage> out;
    bool handed = false;            // out.front() is with the loop
    size_t out_bytes = 0;           // bytes queued in `out` (including the handed front)
    bool attached = false;          // Hello answered with Welcome
    bool lane = false;              // Join answered with Joined: this is a lane of another client
    std::string lane_name;
    bool synced = false;            // sent a keyframe of the current stream
    std::deque<uint64_t> unacked;   // frame ids sent and not yet acked, oldest first
    std::vector<Codec> codecs;      // from SetCodec; empty: no preference
    uint32_t max_kbps = 0;          // from SetCodec; 0: no limit
    bool dead = false;              // the connection failed: close it now
    bool closed = false;            // close() was asked of the loop; on_closed will follow
    bool closing = false;           // flush what is queued (an Error), then close it
    Clock::time_point close_deadline{};
    std::string name;
    uint16_t minor = 0;             // the protocol minor the client's Hello said
    // Video messages in `out` whose FrameSent is owed (minor >= 1), oldest
    // first; `msg` identifies the message while it is still queued.
    struct SentMark {
        const std::string* msg = nullptr;
        uint64_t frame_id = 0;
        Clock::time_point queued{};
    };
    std::deque<SentMark> marks;
    Clock::time_point front_started{};  // when out.front() was handed to the loop
};

// One viewer's audio lane (server_audio.cpp). The rings come before the
// endpoints, so the endpoints (whose realtime callbacks use the rings) are
// destroyed, and stopped, first.
struct AudioPeer {
    brolink::ConnId lane = 0;
    std::string source;
    AudioStartedMsg started;
    bool playback_muted = false;
    bool mic_muted = false;
    std::unique_ptr<audio::PcmRing> down;       // monitor (realtime) -> the audio thread
    std::unique_ptr<audio::JitterBuffer> mic;   // the I/O thread -> the mic node (realtime)
    std::atomic<uint64_t> down_overflow{0};     // monitor frames that found the ring full
    std::unique_ptr<audio::Endpoint> monitor;
    std::unique_ptr<audio::Endpoint> mic_node;
    bool mic_default = false;
    uint64_t down_seq = 0;
    uint64_t down_dropped = 0;  // AudioDown messages dropped because the lane backed up
    uint64_t up_seq = 0;
    uint64_t up_gaps = 0;       // AudioUp sequence gaps (the viewer dropped some)
    Clock::time_point next_stats{};
    std::vector<float> scratch;
    std::vector<float> decoded;
    std::string pcm;
};

struct Pending {
    Frame frame;
    std::function<void()> release;
    Clock::time_point submitted{};
};

// The server's clock as the protocol carries it (Pong, FrameTiming).
inline uint64_t mono_us(Clock::time_point t) {
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count());
}
inline uint64_t span_us(Clock::time_point a, Clock::time_point b) {
    return b > a ? uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(b - a).count()) : 0;
}

// Wakes the I/O thread's loop from any thread.
struct LoopWaker {
    brolink::EventLoop* loop = nullptr;
    void wake() const {
        if (loop) loop->wake();
    }
};

struct Server::Impl final : brolink::LoopHandler {
    ServerConfig cfg;
    std::vector<Codec> server_codecs;  // cfg.codecs this build can encode, in preference order
    std::string path;
    std::unique_ptr<brolink::EventLoop> loop;  // the listener and every connection; I/O thread only
    LoopWaker waker;
    std::thread io_thread;
    std::thread encode_thread;

    mutable std::mutex m;
    std::condition_variable encode_cv;
    bool stop = false;
    std::optional<Pending> pending;
    // Every connection: attached clients, their lanes, and those not yet past
    // Hello / Join. Only the I/O thread adds or removes them.
    std::vector<std::unique_ptr<ClientConn>> clients;
    brolink::lanes::Registry lanes;  // which connections make up which client's session
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
    uint64_t lane_inputs = 0;  // Input messages that came on an input lane (under input_m)

    // The audio lanes (server_audio.cpp). Peers and the backend under `m`.
    std::vector<std::unique_ptr<AudioPeer>> audio_peers;
    std::shared_ptr<audio::Backend> audio_backend;
    std::string audio_backend_error;  // why there is none (set once tried)
    bool audio_backend_tried = false;
    std::thread audio_thread;          // packs the monitor's audio into AudioDown, sends AudioStats
    std::mutex audio_wake_m;
    std::condition_variable audio_cv;
    std::atomic<bool> audio_pending{false};  // a monitor delivered frames (set from realtime threads)
    std::atomic<bool> audio_stop{false};

    // ---- server_io.cpp (I/O thread; `m` held unless noted) ----
    void io_loop();                                  // takes `m` itself
    // brolink::LoopHandler, on the I/O thread (each takes `m` itself).
    void on_accept(brolink::ConnId id) override;
    void on_data(brolink::ConnId id, const char* data, size_t n) override;
    void on_closed(brolink::ConnId id) override;
    ClientConn* find(brolink::ConnId id);
    void handle_message(ClientConn& c, uint16_t type, std::string_view payload);
    void handle_lane_message(ClientConn& c, uint16_t type, std::string_view payload);
    void handle_hello(ClientConn& c, std::string_view payload);
    void handle_join(ClientConn& c, std::string_view payload);
    void handle_set_codec(ClientConn& c, std::string_view payload);
    void handle_input(ClientConn& c, std::string_view payload);
    void handle_ping(ClientConn& c, std::string_view payload);
    // Any thread: queue an Error and close once it is written. Never calls the loop.
    void close_client(ClientConn& c, ErrorCode code, const std::string& message);
    void drop_unsent(ClientConn& c);
    void flush_client(ClientConn& c);                // hands queued messages to the loop
    void service_clients();                          // flush all, close the dead and the finished
    void shutdown_clients();                         // takes `m` itself
    // Recompute want_codec / want_kbps from the attached clients' SetCodec.
    // False when no codec suits every client.
    bool choose_codec();

    // ---- server_audio.cpp ----
    void handle_audio_message(ClientConn& c, uint16_t type, std::string_view payload);  // I/O thread, `m` held
    void start_audio(ClientConn& c, const AudioStartMsg& start);                         // I/O thread, `m` held
    AudioPeer* audio_peer(brolink::ConnId lane);                                        // `m` held
    void drop_audio_peer(brolink::ConnId lane);                                         // `m` held
    void audio_loop();                                                                  // the audio thread
    void pump_audio(AudioPeer& p, Clock::time_point now);                               // `m` held
    void queue_audio(ClientConn& c, AudioPeer& p, SharedMessage msg);                   // `m` held

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
