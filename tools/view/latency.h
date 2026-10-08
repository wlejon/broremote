#pragma once
// Where a picture's time goes, from the viewer's side. The tracker follows
// each frame from the server's submit to the viewer's present:
//
//   queue    submit -> encode start on the server (the encoder busy, or the ack window shut)
//   encode   encode start -> packet ready (conversion and encode)
//   wait     packet queued -> its first byte written to the client's socket (FrameSent)
//   net      first byte written -> the whole message received here: the
//            server's socket, the proxy, ssh both ways, the network, the pipe
//   dwait    received -> the decode thread started on it
//   decode   the decode
//   present  decoded -> SDL_RenderPresent returned with it on screen (the
//            compositor and the display add their own time after that)
//
// The server's times are on its own clock: Ping / Pong give the round trip
// and the offset between the clocks (from the sample with the smallest round
// trip of the recent ones, which is the least skewed), so "net" is a one-way
// figure. Latency probes (with `broremote serve-test --latency`) add the
// whole loop: a key press goes out, and the first decoded picture whose
// second block row counts it closes the loop.

#include "broremote/protocol.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace broremote::view {

using Clock = std::chrono::steady_clock;

// Means, in milliseconds, over the frames presented in a window.
struct LatencyWindow {
    uint64_t frames = 0;   // presented frames with server timing
    double rtt = -1;       // the best recent round trip (-1: no Pong yet)
    double queue = 0, encode = 0, wait = 0, net = 0, dwait = 0, decode = 0, present = 0;
    double age = 0;        // submit -> presented
    double kbytes = 0;     // mean packet size
    double max_age = 0;
};

// One closed latency probe, milliseconds.
struct Probe {
    double total_decoded = 0;   // key press sent -> the answer decoded
    double total_presented = 0; // key press sent -> the answer presented
    double uplink = 0;          // press sent -> the answering frame submitted (one way + the server noticing)
    double queue = 0, encode = 0, wait = 0, net = 0, dwait = 0, decode = 0, present = 0;
    double rtt = 0;             // the round trip when it was sent
};

class LatencyTracker {
public:
    void set_probing(bool on) { probing_ = on; }
    [[nodiscard]] bool probing() const { return probing_; }

    void on_pong(Clock::time_point sent, Clock::time_point received, uint64_t server_us);
    void on_video(const VideoPacket& v, Clock::time_point received);
    void on_frame_sent(const FrameSentMsg& f);
    // `marker`: the input-marker row read from the picture (-1: not read).
    void on_decoded(uint64_t frame_id, Clock::time_point start, Clock::time_point done, int64_t marker);
    void on_presented(uint64_t frame_id, Clock::time_point t);

    // A probe goes out now (the caller sends the key press). False when one
    // is still open (it is abandoned after two seconds).
    bool begin_probe(Clock::time_point now);
    [[nodiscard]] bool probe_open(Clock::time_point now);
    [[nodiscard]] std::vector<Probe> probes() const;
    [[nodiscard]] uint64_t probes_lost() const;

    // Means since the last call.
    LatencyWindow take_window();
    // The best recent round trip in ms (-1: none yet).
    [[nodiscard]] double rtt_ms() const;

private:
    struct Trace {
        uint64_t frame_id = 0;
        FrameTiming timing;
        size_t bytes = 0;
        Clock::time_point received, dstart, decoded;
        bool have_decoded = false;
        bool have_sent = false;
        uint64_t wait_us = 0, write_us = 0;
    };
    struct OpenProbe {
        Clock::time_point sent;
        int64_t expect = 0;  // the marker value that answers it
        bool decoded = false;
        Clock::time_point decoded_at;
        uint64_t frame_id = 0;   // the first picture that answered
        Trace trace;             // that picture's trace
    };

    Trace* find(uint64_t frame_id);
    // The server clock in microseconds, on this clock.
    [[nodiscard]] std::optional<Clock::time_point> to_local(uint64_t server_us) const;
    [[nodiscard]] double rtt_locked() const;

    mutable std::mutex m_;
    bool probing_ = false;
    struct Sample {
        double rtt_us;
        double offset_us;  // server clock - this clock, both in microseconds
        Clock::time_point at;
    };
    std::deque<Sample> samples_;
    std::deque<Trace> traces_;     // recent frames, oldest first
    LatencyWindow sum_;
    std::optional<OpenProbe> probe_;
    int64_t marker_ = -1;          // the newest marker value seen
    std::vector<Probe> probes_;
    uint64_t lost_ = 0;
};

}  // namespace broremote::view
