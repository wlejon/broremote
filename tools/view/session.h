#pragma once
// The viewer's connection, away from the render thread. A connect thread
// opens the stream (ssh or a local socket) and the Client; the Client's
// reader hands configs and packets to a decode thread, which decodes each
// packet, acks it (decoded or not; a failure also requests a keyframe, once
// until the next keyframe arrives), crops the picture to the stream's size,
// and publishes it as the newest frame. The render thread takes only the
// newest: frames it never took are simply replaced, never queued.

#include "broremote/client.h"
#include "connect.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace broremote::view {

using Clock = std::chrono::steady_clock;

struct SessionOptions {
    tools::ConnectTarget target;
    std::string client_name = "broremote-view";
    // Send SetCodec with the codecs this machine decodes (so a server that
    // could fall back to one of them does). Off: take whatever it sends.
    bool negotiate = true;
};

enum class SessionState { Connecting, Connected, Closed };

struct SessionStatus {
    SessionState state = SessionState::Connecting;
    std::string message;          // why it closed, or a decoder problem while connected
    bool failed = false;          // closed because something went wrong (not the user, not a clean shutdown)
    bool have_config = false;
    StreamConfig config;
    std::string decoder;          // Decoder::describe(), or the codec name
    std::string server;           // the server's name from Welcome
};

// Timing of the picture a take_frame() returned.
struct FrameInfo {
    uint64_t frame_id = 0;
    uint64_t sequence = 0;        // counts published pictures from 1
    Clock::time_point received;   // the packet arrived
    Clock::time_point decoded;    // the picture was ready
};

struct SessionStats {
    uint64_t packets = 0, decoded = 0, failed = 0, keyframe_requests = 0;
    uint64_t bytes = 0;
    Clock::time_point first_decoded, last_decoded;
    std::vector<double> decode_ms;  // per decoded picture (bounded: the most recent ones)
};

class Session {
public:
    // `wake` is called (from any thread) when a frame is published or the
    // status changes, so the render thread can stop waiting.
    explicit Session(std::function<void()> wake);
    ~Session();  // closes the connection and joins every thread
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void start(const SessionOptions& options);
    // Ends the connection from this side (the user closed the window).
    void close();

    [[nodiscard]] SessionStatus status() const;
    // Swaps the newest decoded picture into `frame` (whose buffer is reused)
    // when there is one newer than the last taken. False when none is.
    bool take_frame(DecodedFrame& frame, FrameInfo& info);
    void send_input(const InputEvent& e);
    [[nodiscard]] SessionStats stats() const;

private:
    struct Item {
        bool is_config = false;
        StreamConfig config;
        VideoPacket packet;
        Clock::time_point received;
    };

    void connect_thread(SessionOptions options);
    void decode_thread();
    void decode_one(Item& item, Client& client);
    void on_closed(const std::string& why);
    void set_status(const std::function<void(SessionStatus&)>& f);

    std::function<void()> wake_;
    std::thread connector_, decoder_thread_;

    mutable std::mutex m_;           // status, the client pointer, the queue
    std::condition_variable cv_;
    SessionStatus status_;
    std::unique_ptr<Client> client_;
    Client* live_ = nullptr;         // client_ once connect() returned and while open
    std::shared_ptr<Stream> stream_; // to abandon a connect, and for ssh's stderr after the end
    std::deque<Item> queue_;
    bool stopping_ = false;
    bool user_closed_ = false;
    bool server_shutdown_ = false;
    std::string no_common_codec_;    // the explanation, when the server refused our codec list
    std::string where_;              // the target, for messages

    // Decode thread only.
    std::unique_ptr<Decoder> decoder_;
    Codec decoder_codec_ = Codec::Raw;
    StreamConfig config_;
    bool keyframe_requested_ = false;
    DecodedFrame work_;

    mutable std::mutex frame_m_;     // the newest picture and the stats
    DecodedFrame latest_;
    FrameInfo latest_info_;
    uint64_t published_ = 0, taken_ = 0;
    SessionStats stats_;
};

}  // namespace broremote::view
