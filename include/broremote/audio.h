#pragma once
// The audio lane's platform-free core: PCM formats and their conversion, the
// lock-free rings that carry audio between a device's realtime thread and
// the lane, the jitter buffer in front of every playout, and the small
// analysis and WAV helpers the tools and tests use.
//
// Inside a process audio is always interleaved float32 frames; the wire
// carries s16 or f32 (the format each direction agreed on in AudioStart /
// AudioStarted, protocol.h). Times are microseconds on the process's
// steady clock (now_us), the clock Pong reports for a server, so a viewer
// that knows the offset between the two clocks can turn a timestamp from
// the other side into its own.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace broremote::audio {

enum class SampleFormat : uint8_t {
    S16 = 1,  // signed 16-bit little endian
    F32 = 2,  // IEEE-754 single little endian
};

inline constexpr uint32_t kMinRate = 8000;
inline constexpr uint32_t kMaxRate = 192000;
inline constexpr uint32_t kMaxChannels = 8;

struct Format {
    uint32_t rate = 48000;
    uint32_t channels = 2;
    SampleFormat sample = SampleFormat::S16;
    [[nodiscard]] uint32_t sample_bytes() const { return sample == SampleFormat::F32 ? 4 : 2; }
    [[nodiscard]] uint32_t frame_bytes() const { return channels * sample_bytes(); }
    [[nodiscard]] bool valid() const {
        return rate >= kMinRate && rate <= kMaxRate && channels >= 1 && channels <= kMaxChannels &&
               (sample == SampleFormat::S16 || sample == SampleFormat::F32);
    }
    [[nodiscard]] std::string describe() const;  // "48000 Hz, 2 ch, s16"
    bool operator==(const Format&) const = default;
};

[[nodiscard]] const char* sample_format_name(SampleFormat) noexcept;

// The steady clock in microseconds (the server's mono_us).
[[nodiscard]] int64_t now_us();

// Interleaved float frames <-> wire bytes in `f`'s sample format (the
// channel count is the caller's: both sides have f.channels).
void encode_pcm(const float* frames, uint32_t n, const Format& f, std::string& out);
// False when `bytes` is not a whole number of frames.
bool decode_pcm(const uint8_t* bytes, size_t size, const Format& f, std::vector<float>& out);

// Converts interleaved frames between channel counts: down-mixes by
// averaging, up-mixes by repeating (mono -> every channel).
void convert_channels(const float* in, uint32_t frames, uint32_t in_ch, float* out, uint32_t out_ch);

// ---- the ring -------------------------------------------------------------------------------

// A single-producer single-consumer ring of interleaved float frames, with
// the sender's timestamp of each block written, so the consumer can tell the
// time any frame it reads was captured. Lock-free: the producer and the
// consumer may each be a realtime thread. Positions count frames from 0 and
// never wrap (64 bits).
class PcmRing {
public:
    PcmRing(uint32_t channels, uint32_t rate, uint32_t capacity_frames);

    [[nodiscard]] uint32_t channels() const { return channels_; }
    [[nodiscard]] uint32_t rate() const { return rate_; }
    [[nodiscard]] uint32_t capacity() const { return capacity_; }

    // Producer. Appends n frames whose first was captured at stamp_us (the
    // sender's clock). False, writing nothing, when there is no room for all
    // of them.
    bool write(const float* frames, uint32_t n, int64_t stamp_us);

    // Consumer.
    [[nodiscard]] uint32_t available() const;
    // Reads up to n frames into out. *stamp_us (when given) is the capture
    // time of the first frame read, from its block's stamp; 0 when no block
    // stamp is known (nothing was ever read).
    uint32_t read(float* out, uint32_t n, int64_t* stamp_us = nullptr);
    // Discards n frames (at most what is available).
    void skip(uint32_t n);

    // Any thread: frames written / read so far (approximate while running).
    [[nodiscard]] uint64_t written() const { return w_.load(std::memory_order_acquire); }
    [[nodiscard]] uint64_t consumed() const { return r_.load(std::memory_order_acquire); }

private:
    struct Mark {
        uint64_t pos = 0;
        int64_t stamp_us = 0;
    };
    static constexpr uint32_t kMarks = 1024;
    void advance_marks(uint64_t pos);  // consumer: the current mark is the last with mark.pos <= pos

    uint32_t channels_, rate_, capacity_;
    std::vector<float> buf_;
    std::vector<Mark> marks_;
    std::atomic<uint64_t> w_{0}, r_{0};
    std::atomic<uint64_t> mw_{0}, mr_{0};  // marks written / consumed
    Mark cur_{};                            // consumer only
    bool have_cur_ = false;
};

// ---- the jitter buffer ------------------------------------------------------------------------

// What a playout pulls from: a PcmRing with a target and a bound on its
// depth. Before playing it fills to `target` frames (priming), so packets
// that arrive unevenly still play continuously; when it runs dry it plays
// silence and primes again (an underrun); when it holds more than `max` it
// drops the oldest frames down to the target (so a burst after a stall, or
// a sender clock running faster than the playout's, never builds latency).
// push() is the producer (the lane's reader), pull() the consumer (a
// device's realtime thread).
class JitterBuffer {
public:
    JitterBuffer(uint32_t channels, uint32_t rate, uint32_t target_ms, uint32_t max_ms);

    [[nodiscard]] uint32_t channels() const { return ring_.channels(); }
    [[nodiscard]] uint32_t rate() const { return ring_.rate(); }

    // Producer. Frames that do not fit are counted as overflow and dropped.
    void push(const float* frames, uint32_t n, int64_t stamp_us);

    // Consumer: fills `out` with n frames, silence where there are none.
    // out_us is when out[0] leaves this process (is heard, or handed to the
    // next node), on this process's clock: it pairs with the stamp of the
    // frame played, for the latency.
    void pull(float* out, uint32_t n, int64_t out_us);

    struct Stats {
        uint64_t played = 0;          // frames pulled that carried audio
        uint64_t underruns = 0;       // times it ran dry while playing
        uint64_t dropped = 0;         // frames dropped to hold the depth bound
        uint64_t overflow = 0;        // frames pushed that did not fit
        uint32_t depth_frames = 0;    // frames queued at the last pull
        // The latest frame played: its sender's capture stamp (the sender's
        // clock) and when it was played (this clock). valid once one played.
        bool valid = false;
        int64_t stamp_us = 0;
        int64_t out_us = 0;
    };
    // Any thread.
    [[nodiscard]] Stats stats() const;

    [[nodiscard]] uint32_t target_frames() const { return target_; }
    [[nodiscard]] uint32_t max_frames() const { return max_; }

private:
    PcmRing ring_;
    uint32_t target_, max_;
    bool playing_ = false;  // consumer only
    std::atomic<uint64_t> played_{0}, underruns_{0}, dropped_{0}, overflow_{0};
    std::atomic<uint32_t> depth_{0};
    // The latest (stamp, out) pair, published with a sequence lock.
    std::atomic<uint64_t> pair_seq_{0};
    std::atomic<int64_t> pair_stamp_{0}, pair_out_{0};
};

// ---- analysis -------------------------------------------------------------------------------

struct ToneAnalysis {
    double rms_dbfs = -200;   // level of the first channel, dB relative to full scale
    double frequency_hz = 0;  // dominant frequency (0 for silence)
    double peak = 0;          // largest absolute sample
};
// Analyses the first channel of interleaved frames: RMS level, peak, and
// the frequency of a dominant tone (from its rising zero crossings,
// interpolated between samples; meant for test tones, not music).
[[nodiscard]] ToneAnalysis analyze(const float* frames, size_t n, uint32_t channels, uint32_t rate);

// The index of the first frame whose first channel's absolute value reaches
// `threshold`, or npos-like SIZE_MAX when none does.
[[nodiscard]] size_t first_onset(const float* frames, size_t n, uint32_t channels, float threshold);

// A sine generator: fills interleaved frames, every channel the same.
class Tone {
public:
    Tone(double hz, double amplitude, uint32_t rate, uint32_t channels)
        : hz_(hz), amp_(amplitude), rate_(rate), ch_(channels) {}
    void fill(float* out, uint32_t frames);

private:
    double hz_, amp_;
    uint32_t rate_, ch_;
    double phase_ = 0;
};

// ---- WAV --------------------------------------------------------------------------------------

struct WavData {
    uint32_t rate = 0;
    uint32_t channels = 0;
    std::vector<float> frames;  // interleaved
};
// Reads PCM s16 / s24 / s32 or float32 WAV. False with *err otherwise.
bool read_wav(const std::string& path, WavData& out, std::string* err);
// Writes float32 WAV.
bool write_wav(const std::string& path, const WavData& in, std::string* err);

}  // namespace broremote::audio
