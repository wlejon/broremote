#pragma once
// The viewer's whole session, with no window in it: what a viewer of any
// kind (a bro app through bro.remote.connect) puts its own display and
// input around; `broremote probe` runs one with no display at all.
//
// A connect thread opens the stream (ssh or a local socket) and the Client;
// the Client's reader hands configs and packets to a decode thread, which
// decodes each packet, acks it (decoded or not; a failure also requests a
// keyframe, once until the next keyframe arrives), crops the picture to the
// stream's size and publishes it as the newest frame. The display takes only
// the newest: frames it never took are replaced, never queued. A ping thread
// keeps the round trip and the clock offset (latency.h), and once video is
// up the audio lane comes up on a thread of its own (audio_client.h).

#include "broremote/audio_client.h"
#include "broremote/client.h"
#include "broremote/connect.h"
#include "broremote/latency.h"

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

namespace broremote {

// The audio lane (protocol 1.3): a further connection, opened like the
// control connection, carrying the host's audio here and this mic there.
struct ViewerAudioOptions {
    bool enabled = true;
    bool mic = true;
    bool playback = true;
    double mic_tone_hz = 0;       // a tone instead of the mic
    std::string mic_file;         // a WAV (looped) instead of the mic
    // What the speakers played is kept (the first channel) for
    // recorded_audio(): a minute, or `record_seconds`.
    uint32_t record_seconds = 60;
    uint32_t jitter_ms = 20;
    std::string mic_device;       // (part of) a name; empty: the communications default
    std::string speaker_device;   // empty: the default output
    bool mic_muted = false;       // start muted
    bool playback_muted = false;
};

struct ViewerOptions {
    ConnectTarget target;
    ViewerAudioOptions audio;
    std::string client_name = "broremote viewer";  // sent in Hello
    // Send SetCodec with the codecs this machine decodes (so a server that
    // could fall back to one of them does). Off: take whatever it sends.
    bool negotiate = true;
    // Where decoded pictures should live: Cpu, or on the GPU where the
    // decoder can hand them over (Media Foundation: PictureMemory::D3D11);
    // a picture says where it is (DecodedFrame::memory).
    brovideo::PictureMemory output = brovideo::PictureMemory::Cpu;
    // Decode thread, for each picture after cropping and before it is
    // published: a display that turns a GPU picture into something of its
    // own (a texture it shares, say) does it here, off its own thread.
    std::function<void(DecodedFrame&)> prepare;
    // Decode thread, while a latency probe is open: the input marker of a
    // picture not in CPU memory (read_marker reads CPU pictures itself).
    // -1 when it cannot be read.
    std::function<int64_t(const DecodedFrame&)> read_marker;
    // What the session has to say (lanes, audio, decode failures), one line
    // at a time, from any of its threads. Empty: nothing is said.
    std::function<void(const std::string&)> log;
};

enum class ViewerState { Connecting, Connected, Closed };

struct ViewerStatus {
    ViewerState state = ViewerState::Connecting;
    std::string message;          // why it closed, or a decoder problem while connected
    bool failed = false;          // closed because something went wrong (not the user, not a clean shutdown)
    bool have_config = false;
    StreamConfig config;
    std::string decoder;          // Decoder::describe(), or the codec name
    bool hardware = false;        // the decoder runs in hardware
    std::string server;           // the server's name from Welcome
    uint16_t server_minor = 0;    // the protocol minor the server speaks
    bool input_lane = false;      // input goes on its own lane
    std::string input_lane_error; // why not, when one was asked for
};

// Timing of the picture a take_frame() returned.
struct ViewerFrameInfo {
    uint64_t frame_id = 0;
    uint64_t sequence = 0;        // counts published pictures from 1
    Clock::time_point received;   // the packet arrived
    Clock::time_point decoded;    // the picture was ready
};

struct ViewerStats {
    uint64_t packets = 0, decoded = 0, failed = 0, keyframe_requests = 0;
    uint64_t bytes = 0;
    uint64_t gpu_pictures = 0;    // pictures the decoder left on the GPU (never read back here)
    Clock::time_point first_decoded, last_decoded;
    std::vector<double> decode_ms;  // per decoded picture (bounded: the most recent ones)
};

class ViewerSession {
public:
    // `wake` is called (from any thread) when a frame is published, the
    // cursor or the status changes, so the display can stop waiting.
    explicit ViewerSession(std::function<void()> wake);
    ~ViewerSession();  // closes the connection and joins every thread
    ViewerSession(const ViewerSession&) = delete;
    ViewerSession& operator=(const ViewerSession&) = delete;

    void start(const ViewerOptions& options);
    // Ends the connection from this side (the user closed the view).
    void close();

    [[nodiscard]] ViewerStatus status() const;
    // Swaps the newest decoded picture into `frame` (whose buffer is reused)
    // when there is one newer than the last taken. False when none is.
    bool take_frame(DecodedFrame& frame, ViewerFrameInfo& info);
    // Any thread. Dropped unless connected.
    void send_input(const InputEvent& e);
    [[nodiscard]] ViewerStats stats() const;
    // The host's pointer (Cursor), and a count of the changes so far (0: none
    // received yet).
    uint64_t cursor(CursorState& out) const;

    // Timing: the display reports each picture it presented; probes
    // (serve-test --latency) send a key press and wait for the answer.
    [[nodiscard]] LatencyTracker& latency() { return latency_; }
    void note_presented(uint64_t frame_id, Clock::time_point when) { latency_.on_presented(frame_id, when); }
    // Sends one probe (a press and release of KEY_F13) when none is open. False otherwise.
    bool probe();

    // The audio lane. False when there is none (not yet, refused, or off).
    bool audio_stats(AudioSession::Stats& out) const;
    void set_mic_muted(bool muted);
    void set_playback_muted(bool muted);
    void toggle_mic_mute() { set_mic_muted(!mic_muted()); }
    void toggle_playback_mute() { set_playback_muted(!playback_muted()); }
    [[nodiscard]] bool mic_muted() const;
    [[nodiscard]] bool playback_muted() const;
    // What the speakers played so far (first channel); the rate is 0 when
    // nothing was recorded.
    void recorded_audio(std::vector<float>& frames, uint32_t& rate, uint32_t& channels) const;

private:
    struct Item {
        bool is_config = false;
        StreamConfig config;
        VideoPacket packet;
        Clock::time_point received;
    };
    // The audio recording, filled on the speakers' realtime thread: no
    // allocation there, so it is sized up front and stops when full.
    struct Recording {
        std::vector<float> frames;
        std::atomic<size_t> used{0};
        uint32_t rate = 0, channels = 0;
    };

    void connect_thread(ViewerOptions options);
    void decode_thread();
    void ping_thread();
    void audio_thread(ViewerOptions options, brolink::lanes::Grant grant);
    void decode_one(Item& item, Client& client);
    void on_closed(const std::string& why);
    void set_status(const std::function<void(ViewerStatus&)>& f);
    void say(const std::string& line) const;

    std::function<void()> wake_;
    ViewerOptions options_;          // fixed at start()
    std::thread connector_, decoder_thread_, pinger_, audio_thread_;
    LatencyTracker latency_;

    mutable std::mutex m_;           // status, the client pointer, the queue, the cursor, audio
    std::condition_variable cv_;
    ViewerStatus status_;
    std::unique_ptr<Client> client_;
    Client* live_ = nullptr;         // client_ once connect() returned and while open
    std::shared_ptr<Stream> stream_; // to abandon a connect, and for ssh's stderr after the end
    std::deque<Item> queue_;
    bool stopping_ = false;
    bool user_closed_ = false;
    bool server_shutdown_ = false;
    std::string no_common_codec_;    // the explanation, when the server refused our codec list
    std::string where_;              // the target, for messages
    CursorState cursor_;
    uint64_t cursor_changes_ = 0;
    std::shared_ptr<Stream> audio_stream_;   // to abandon the audio lane's connect
    std::unique_ptr<AudioSession> audio_;
    bool want_mic_muted_ = false, want_playback_muted_ = false;  // before the lane is up

    // The audio thread's sources.
    std::unique_ptr<Recording> recording_;
    std::vector<float> mic_file_;    // mono at mic_file_rate_
    uint32_t mic_file_rate_ = 0;
    size_t mic_file_pos_ = 0;
    std::unique_ptr<audio::Tone> mic_tone_;

    // Decode thread only.
    std::unique_ptr<Decoder> decoder_;
    Codec decoder_codec_ = Codec::Raw;
    StreamConfig config_;
    bool keyframe_requested_ = false;
    DecodedFrame work_;

    mutable std::mutex frame_m_;     // the newest picture and the stats
    DecodedFrame latest_;
    ViewerFrameInfo latest_info_;
    uint64_t published_ = 0, taken_ = 0;
    ViewerStats stats_;
};

}  // namespace broremote
