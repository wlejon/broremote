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

#include "broremote/audio_device.h"
#include "broremote/codec.h"
#include "broremote/frame.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace broremote {

// The audio lane (1.3). A viewer that joins one may ask for the host's audio
// (what this machine plays, captured from the default output's monitor) and
// may send its microphone, which appears here as a microphone node of its
// own ("broremote: <viewer> mic") for as long as that viewer is attached.
struct ServerAudioConfig {
    bool enabled = true;
    // The devices. Null: audio::platform_backend() (PipeWire on Linux), made
    // at the first AudioStart; without one a viewer is told audio is off.
    std::shared_ptr<audio::Backend> backend;
    // Make each viewer's mic node the default source while it exists (the
    // previous default comes back when the last one goes), so programs that
    // record from "the microphone" hear the viewer without being told.
    bool mic_as_default = false;
    uint32_t jitter_ms = 20;      // the mic's jitter buffer fills to this before playing
    uint32_t max_buffer_ms = 80;  // and drops the oldest audio beyond this
    uint32_t period_ms = 5;       // the device period asked for, both directions
};

struct ServerConfig {
    std::string socket_name = "default";        // where it listens: a socket or pipe named for it (stream.h)
    std::vector<Codec> codecs = {Codec::HEVC, Codec::H264};  // preference order; those this build cannot encode are skipped
    uint32_t bitrate_kbps = 20000;
    uint32_t fps = 60;                          // a hint for the encoder and the viewer
    uint32_t max_frames_in_flight = 2;          // unacked frames per client before encoding pauses
    uint32_t keyframe_interval_s = 0;           // 0: keyframes only on demand
    std::string name = "broremote";             // sent in Welcome
    ServerAudioConfig audio;
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
    // The address it listens on: a socket path, or a pipe name on Windows.
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
        uint64_t lanes = 0;         // lanes joined (1.2: input lanes; 1.3: audio lanes too)
        uint64_t lane_inputs = 0;   // Input messages that arrived on an input lane
        uint64_t audio_lanes = 0;   // audio lanes started (1.3)
        uint64_t audio_up = 0;      // AudioUp packets received (the viewers' mics)
        uint64_t audio_down = 0;    // AudioDown packets sent (this machine's audio)
    };
    [[nodiscard]] Stats stats() const;

    // Each viewer on an audio lane now.
    struct AudioViewer {
        std::string source;          // the viewer's machine, as it said
        bool playback = false;       // it is sent this machine's audio
        bool playback_muted = false;
        bool mic = false;            // its mic is a node here
        bool mic_muted = false;
        std::string mic_node;        // the node's description ("broremote: <source> mic")
        bool mic_default = false;    // the node is the default source
        double mic_buffer_ms = 0;    // the mic's jitter buffer depth now
        uint64_t mic_underruns = 0;
        uint64_t mic_dropped_frames = 0;
    };
    [[nodiscard]] std::vector<AudioViewer> audio_viewers() const;
    // What is wrong with this host's audio now (the audio system is down or
    // restarting, or there is none); empty when nothing is. Viewers on an
    // audio lane see the same in their AudioStats (1.5).
    [[nodiscard]] std::string audio_status() const;

    struct Impl;

private:
    explicit Server(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace broremote
