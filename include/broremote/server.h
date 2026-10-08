#pragma once
// The host side: a server listening on a local socket. The host submits
// frames and drains input; viewers connect (directly, or through
// `ssh host broremote proxy`) and receive one encoded stream.
//
// Threads: an I/O thread runs the accept/read/write loop, and an encode
// thread takes the pending frame, encodes it and queues the packet to every
// client. Destroying the Server joins both, releases any frame it still
// holds, and closes every client (each is sent Error(ServerShutdown) first,
// best effort).

#include "broremote/codec.h"
#include "broremote/frame.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace broremote {

struct ServerConfig {
    std::string socket_name = "default";        // the socket file is <runtime dir>/<name>.sock (stream.h)
    std::vector<Codec> codecs = {Codec::HEVC, Codec::H264};  // preference order; those this build cannot encode are skipped
    uint32_t bitrate_kbps = 20000;
    uint32_t fps = 60;                          // a hint for the encoder and the viewer
    uint32_t max_frames_in_flight = 2;          // unacked frames per client before encoding pauses
    uint32_t keyframe_interval_s = 0;           // 0: keyframes only on demand
    std::string name = "broremote";             // sent in Welcome
};

class Server {
public:
    // Creates the socket and starts the threads. Null (with *err) when no
    // configured codec can be encoded here, or the socket cannot be made
    // (including when another server already listens on it).
    static std::unique_ptr<Server> create(const ServerConfig&, std::string* err);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Any thread; returns at once. The server holds at most one pending
    // frame: a newer submit replaces it and the replaced frame is released.
    // With no client attached the frame is released at once, unencoded.
    // While encoding is paused (a client is at its ack window) the pending
    // frame waits, so the frame encoded on resume is the newest one.
    //
    // `release` is called exactly once per submit, once the server no longer
    // reads the frame's memory: inside submit() itself (on the caller's
    // thread) for a replaced frame or when nobody is watching; otherwise on
    // the encode thread, right after the encoder's conversion copy. It must
    // not call back into the Server.
    void submit(const Frame& frame, std::function<void()> release);

    // Appends every input event received since the last call. Events are
    // queued up to a bound (65536); beyond it new events are dropped.
    void drain_input(std::vector<InputEvent>& out);

    // The pointer state, sent to every client when it changes and to each
    // client as it joins.
    void set_cursor(const CursorState& cursor);

    // Clients that completed the handshake.
    [[nodiscard]] size_t client_count() const;
    // False when no client is attached: the host can skip submitting.
    [[nodiscard]] bool wants_frames() const;
    [[nodiscard]] const std::string& socket_path() const;

    // The stream clients were last sent: the codec and size being encoded and
    // the bitrate in force. Empty before the first frame was encoded, and
    // after the encoder failed.
    struct StreamInfo {
        Codec codec = Codec::Raw;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t bitrate_kbps = 0;
    };
    [[nodiscard]] std::optional<StreamInfo> stream() const;

    struct Stats {
        uint64_t submitted = 0;   // frames passed to submit()
        uint64_t encoded = 0;     // frames encoded and sent
        uint64_t keyframes = 0;   // of those, keyframes
        uint64_t replaced = 0;    // released unencoded: replaced by a newer submit
        uint64_t unwatched = 0;   // released unencoded: no client attached
        uint64_t failed = 0;      // released unencoded: the encoder failed
        uint64_t streams = 0;     // StreamConfigs made (one per reconfigure)
        uint64_t window_waits = 0;  // frames submitted while a client was at its ack window (they wait)
    };
    [[nodiscard]] Stats stats() const;

    struct Impl;

private:
    explicit Server(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace broremote
