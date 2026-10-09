#pragma once
// The viewer's side of the audio lane (protocol 1.3).
//
// AudioClient speaks the lane: it joins a session's audio lane with the
// grant from the control connection's Welcome, says what it wants
// (AudioStart), and then carries PCM both ways. AudioSession puts devices on
// its two ends: the mic (or a generator standing in for it) is sent up, and
// the host's audio is played through a jitter buffer to the speakers. It
// keeps the clock offset to the host from Ping / Pong on the lane, so both
// one-way latencies can be reported.
//
// Neither touches the control connection or the input lane: losing the
// audio lane ends audio and nothing else. No windowing toolkit is involved,
// so a viewer of any kind (`broremote probe`, a bro app) can use them.

#include "broremote/audio.h"
#include "broremote/audio_device.h"
#include "broremote/protocol.h"
#include "broremote/stream.h"

#include <brolink/lanes.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace broremote {

struct AudioLaneOptions {
    std::string source;  // this machine's name, for the host's mic node; empty: the host name
    bool playback = true;
    audio::Format playback_format{48000, 2, audio::SampleFormat::S16};
    bool mic = true;
    audio::Format mic_format{48000, 1, audio::SampleFormat::S16};
    uint32_t timeout_ms = 10000;  // for Joined and AudioStarted
};

// Callbacks run on the lane's reader thread.
struct AudioLaneHandlers {
    // AudioDown, decoded to float frames of started().playback_format.channels.
    std::function<void(uint64_t seq, uint64_t capture_us, const float* frames, uint32_t n)> on_audio;
    std::function<void(const AudioStatsMsg&)> on_stats;
    std::function<void(std::chrono::steady_clock::time_point sent, std::chrono::steady_clock::time_point received,
                       uint64_t server_time_us)>
        on_pong;
    // Once, when the lane ends, with why.
    std::function<void(const std::string&)> on_closed;
};

class AudioClient {
public:
    // Joins the lane on `stream` and exchanges AudioStart / AudioStarted.
    // Null with *err when the host refuses or does not answer.
    static std::unique_ptr<AudioClient> connect(std::unique_ptr<Stream> stream, const brolink::lanes::Grant& grant,
                                                AudioLaneHandlers handlers, const AudioLaneOptions& options,
                                                std::string* err);
    ~AudioClient();  // closes the lane and joins the reader (on_closed runs first)
    AudioClient(const AudioClient&) = delete;
    AudioClient& operator=(const AudioClient&) = delete;

    // What the host agreed to.
    [[nodiscard]] const AudioStartedMsg& started() const;
    // Any one thread: sends n frames of mic audio (started().mic_format's
    // channel count) captured at capture_us. False once the lane is gone.
    bool send_mic(const float* frames, uint32_t n, uint64_t capture_us);
    void set_muted(bool playback_muted, bool mic_muted);
    bool ping();
    [[nodiscard]] bool connected() const;
    void close();

    struct Impl;

private:
    explicit AudioClient(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

struct AudioSessionOptions {
    AudioLaneOptions lane;
    // The devices. Null: audio::platform_backend() (WASAPI on Windows).
    std::shared_ptr<audio::Backend> backend;
    // Devices by (part of) their name; empty: the system defaults (the
    // communications mic, the console speakers).
    std::string mic_device;
    std::string speaker_device;
    // When set, the mic's frames come from this (a tone, a file) instead of
    // the mic device, paced in real time.
    audio::GenerateFn mic_generator;
    uint32_t jitter_ms = 20;      // the playback buffer fills to this before playing
    uint32_t max_buffer_ms = 80;  // and drops the oldest audio beyond this
    uint32_t period_ms = 10;      // the device period asked for
    // The host's audio as the speakers were handed it, on their realtime
    // thread (t_us: when it is heard): for recording or checking it.
    audio::CaptureFn on_played;
};

class AudioSession {
public:
    // Opens the devices, then the lane on `stream` (another connection to the
    // same server, opened the way the control connection was) with the
    // control connection's grant. Null with *err when the lane fails. A
    // device that cannot open turns its direction off (stats() says why)
    // rather than failing the session.
    static std::unique_ptr<AudioSession> start(std::unique_ptr<Stream> stream, const brolink::lanes::Grant& grant,
                                               const AudioSessionOptions& options, std::string* err);
    ~AudioSession();
    AudioSession(const AudioSession&) = delete;
    AudioSession& operator=(const AudioSession&) = delete;

    void set_mic_muted(bool muted);
    void set_playback_muted(bool muted);
    [[nodiscard]] bool mic_muted() const;
    [[nodiscard]] bool playback_muted() const;

    struct Stats {
        bool connected = false;
        std::string closed;             // why the lane ended (when it did)
        bool playback = false;          // the host's audio is coming and playing here
        bool mic = false;               // this mic goes to the host
        std::string mic_node;           // the host's name for it
        std::string mic_device;         // what it captures from here
        std::string speaker_device;
        bool echo_cancel = false;       // the mic's echo cancellation is on (as far as the OS says)
        std::string notes;              // why a direction is off, and host_status when there is one
        std::string host_status;        // 1.5: what is wrong with the host's audio now (its AudioStats)
        double rtt_ms = -1;             // the lane's round trip (the best recent sample)
        // One-way latencies, -1 until measured:
        double mic_latency_ms = -1;       // captured here -> played into the host's mic node
        double playback_latency_ms = -1;  // captured on the host -> heard here
        // The host's mic buffer, from its AudioStats.
        double host_mic_buffer_ms = 0;
        uint64_t host_mic_underruns = 0;
        uint64_t host_mic_dropped = 0;
        // The playback buffer here.
        double playback_buffer_ms = 0;
        uint64_t playback_underruns = 0;
        uint64_t playback_dropped = 0;  // frames dropped here, plus packets the host dropped
        uint64_t mic_packets = 0;       // AudioUp sent
        uint64_t mic_overflow = 0;      // mic frames dropped before sending (the lane stalled)
        uint64_t playback_packets = 0;  // AudioDown received
    };
    [[nodiscard]] Stats stats() const;

    struct Impl;

private:
    explicit AudioSession(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// This machine's name (for AudioLaneOptions::source).
std::string local_host_name();

}  // namespace broremote
