// The audio lane's platform-free core (include/broremote/audio.h).
#include "broremote/audio.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace broremote::audio {

const char* sample_format_name(SampleFormat f) noexcept {
    switch (f) {
        case SampleFormat::S16: return "s16";
        case SampleFormat::F32: return "f32";
    }
    return "?";
}

std::string Format::describe() const {
    return std::to_string(rate) + " Hz, " + std::to_string(channels) + " ch, " + sample_format_name(sample);
}

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void encode_pcm(const float* frames, uint32_t n, const Format& f, std::string& out) {
    const size_t samples = size_t(n) * f.channels;
    const size_t at = out.size();
    out.resize(at + samples * f.sample_bytes());
    char* p = out.data() + at;
    if (f.sample == SampleFormat::F32) {
        for (size_t i = 0; i < samples; ++i) {
            uint32_t bits;
            std::memcpy(&bits, &frames[i], 4);
            for (int b = 0; b < 4; ++b) *p++ = char((bits >> (8 * b)) & 0xFF);
        }
    } else {
        for (size_t i = 0; i < samples; ++i) {
            const float c = std::clamp(frames[i], -1.0f, 1.0f);
            const auto v = int16_t(std::lrint(c * 32767.0f));
            *p++ = char(uint16_t(v) & 0xFF);
            *p++ = char(uint16_t(v) >> 8);
        }
    }
}

bool decode_pcm(const uint8_t* bytes, size_t size, const Format& f, std::vector<float>& out) {
    const size_t fb = f.frame_bytes();
    if (fb == 0 || size % fb) return false;
    const size_t samples = size / f.sample_bytes();
    out.resize(samples);
    if (f.sample == SampleFormat::F32) {
        for (size_t i = 0; i < samples; ++i) {
            const uint8_t* q = bytes + i * 4;
            const uint32_t bits = uint32_t(q[0]) | uint32_t(q[1]) << 8 | uint32_t(q[2]) << 16 | uint32_t(q[3]) << 24;
            float v;
            std::memcpy(&v, &bits, 4);
            out[i] = std::isfinite(v) ? v : 0.0f;
        }
    } else {
        for (size_t i = 0; i < samples; ++i) {
            const auto v = int16_t(uint16_t(bytes[i * 2]) | uint16_t(bytes[i * 2 + 1]) << 8);
            out[i] = float(v) / 32768.0f;
        }
    }
    return true;
}

void convert_channels(const float* in, uint32_t frames, uint32_t in_ch, float* out, uint32_t out_ch) {
    if (in_ch == out_ch) {
        std::memcpy(out, in, size_t(frames) * in_ch * sizeof(float));
        return;
    }
    for (uint32_t i = 0; i < frames; ++i) {
        const float* s = in + size_t(i) * in_ch;
        float* d = out + size_t(i) * out_ch;
        if (in_ch == 1) {
            for (uint32_t c = 0; c < out_ch; ++c) d[c] = s[0];
        } else if (out_ch == 1) {
            float sum = 0;
            for (uint32_t c = 0; c < in_ch; ++c) sum += s[c];
            d[0] = sum / float(in_ch);
        } else {
            for (uint32_t c = 0; c < out_ch; ++c) d[c] = s[c < in_ch ? c : in_ch - 1];
        }
    }
}

// ---- PcmRing --------------------------------------------------------------------------------

PcmRing::PcmRing(uint32_t channels, uint32_t rate, uint32_t capacity_frames)
    : channels_(std::max<uint32_t>(1, channels)),
      rate_(std::max<uint32_t>(1, rate)),
      capacity_(std::max<uint32_t>(1, capacity_frames)),
      buf_(size_t(capacity_) * channels_),
      marks_(kMarks) {}

bool PcmRing::write(const float* frames, uint32_t n, int64_t stamp_us) {
    const uint64_t w = w_.load(std::memory_order_relaxed);
    const uint64_t r = r_.load(std::memory_order_acquire);
    if (w - r + n > capacity_) return false;
    for (uint32_t done = 0; done < n;) {
        const uint32_t at = uint32_t((w + done) % capacity_);
        const uint32_t k = std::min(n - done, capacity_ - at);
        std::memcpy(&buf_[size_t(at) * channels_], frames + size_t(done) * channels_, size_t(k) * channels_ * sizeof(float));
        done += k;
    }
    // The block's stamp. With the marks full (a consumer far behind) the
    // stamp is skipped and the frames are timed from an older one.
    const uint64_t mw = mw_.load(std::memory_order_relaxed);
    if (mw - mr_.load(std::memory_order_acquire) < kMarks) {
        marks_[mw % kMarks] = Mark{w, stamp_us};
        mw_.store(mw + 1, std::memory_order_release);
    }
    w_.store(w + n, std::memory_order_release);
    return true;
}

uint32_t PcmRing::available() const {
    return uint32_t(w_.load(std::memory_order_acquire) - r_.load(std::memory_order_relaxed));
}

void PcmRing::advance_marks(uint64_t pos) {
    uint64_t mr = mr_.load(std::memory_order_relaxed);
    const uint64_t mw = mw_.load(std::memory_order_acquire);
    while (mr < mw && marks_[mr % kMarks].pos <= pos) {
        cur_ = marks_[mr % kMarks];
        have_cur_ = true;
        ++mr;
    }
    mr_.store(mr, std::memory_order_release);
}

uint32_t PcmRing::read(float* out, uint32_t n, int64_t* stamp_us) {
    const uint64_t r = r_.load(std::memory_order_relaxed);
    const uint32_t k = std::min(n, available());
    advance_marks(r);
    if (stamp_us) {
        *stamp_us = have_cur_ ? cur_.stamp_us + int64_t((r - cur_.pos) * 1000000ull / rate_) : 0;
    }
    for (uint32_t done = 0; done < k;) {
        const uint32_t at = uint32_t((r + done) % capacity_);
        const uint32_t m = std::min(k - done, capacity_ - at);
        std::memcpy(out + size_t(done) * channels_, &buf_[size_t(at) * channels_], size_t(m) * channels_ * sizeof(float));
        done += m;
    }
    r_.store(r + k, std::memory_order_release);
    return k;
}

void PcmRing::skip(uint32_t n) {
    const uint64_t r = r_.load(std::memory_order_relaxed);
    const uint32_t k = std::min(n, available());
    advance_marks(r + k);
    r_.store(r + k, std::memory_order_release);
}

// ---- JitterBuffer ---------------------------------------------------------------------------

namespace {
uint32_t ms_frames(uint32_t ms, uint32_t rate) { return uint32_t(uint64_t(ms) * rate / 1000); }
}  // namespace

JitterBuffer::JitterBuffer(uint32_t channels, uint32_t rate, uint32_t target_ms, uint32_t max_ms)
    // A second of room: far beyond the bound, so a push only fails when the
    // consumer is not running at all.
    : ring_(channels, rate, std::max(rate, ms_frames(max_ms, rate) * 2)),
      target_(ms_frames(target_ms, rate)),
      max_(std::max(ms_frames(max_ms, rate), ms_frames(target_ms, rate) + std::max<uint32_t>(1, rate / 100))) {}

void JitterBuffer::push(const float* frames, uint32_t n, int64_t stamp_us) {
    if (!ring_.write(frames, n, stamp_us)) overflow_.fetch_add(n, std::memory_order_relaxed);
}

void JitterBuffer::pull(float* out, uint32_t n, int64_t out_us) {
    const uint32_t ch = ring_.channels();
    uint32_t avail = ring_.available();
    if (!playing_) {
        if (avail < std::max<uint32_t>(target_, 1)) {
            std::fill(out, out + size_t(n) * ch, 0.0f);
            depth_.store(avail, std::memory_order_relaxed);
            return;
        }
        playing_ = true;
    }
    if (avail > max_) {
        const uint32_t drop = avail - target_;
        ring_.skip(drop);
        dropped_.fetch_add(drop, std::memory_order_relaxed);
        avail = target_;
    }
    int64_t stamp = 0;
    const uint32_t k = ring_.read(out, n, &stamp);
    if (k < n) {
        std::fill(out + size_t(k) * ch, out + size_t(n) * ch, 0.0f);
        underruns_.fetch_add(1, std::memory_order_relaxed);
        playing_ = false;
    }
    if (k > 0) played_.fetch_add(k, std::memory_order_relaxed);
    if (k > 0 && stamp != 0) {
        const uint64_t s = pair_seq_.load(std::memory_order_relaxed);
        pair_seq_.store(s + 1, std::memory_order_release);  // odd: writing
        pair_stamp_.store(stamp, std::memory_order_relaxed);
        pair_out_.store(out_us, std::memory_order_relaxed);
        pair_seq_.store(s + 2, std::memory_order_release);
    }
    depth_.store(ring_.available(), std::memory_order_relaxed);
}

JitterBuffer::Stats JitterBuffer::stats() const {
    Stats s;
    s.played = played_.load(std::memory_order_relaxed);
    s.underruns = underruns_.load(std::memory_order_relaxed);
    s.dropped = dropped_.load(std::memory_order_relaxed);
    s.overflow = overflow_.load(std::memory_order_relaxed);
    s.depth_frames = depth_.load(std::memory_order_relaxed);
    for (int tries = 0; tries < 64; ++tries) {
        const uint64_t a = pair_seq_.load(std::memory_order_acquire);
        if (a == 0) break;
        if (a & 1) continue;
        const int64_t stamp = pair_stamp_.load(std::memory_order_relaxed);
        const int64_t out = pair_out_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (pair_seq_.load(std::memory_order_relaxed) != a) continue;
        s.valid = true;
        s.stamp_us = stamp;
        s.out_us = out;
        break;
    }
    return s;
}

// ---- analysis -------------------------------------------------------------------------------

ToneAnalysis analyze(const float* frames, size_t n, uint32_t channels, uint32_t rate) {
    ToneAnalysis a;
    if (n == 0 || channels == 0 || rate == 0) return a;
    // Accumulated in locals: MSVC 19.4x /O2 lost the peak when it was
    // reduced straight into a.peak (the early return below then fired).
    double sum = 0, peak = 0;
    for (size_t i = 0; i < n; ++i) {
        const double v = frames[i * channels];
        sum += v * v;
        const double m = v < 0 ? -v : v;
        if (m > peak) peak = m;
    }
    const double rms = std::sqrt(sum / double(n));
    a.rms_dbfs = rms > 0 ? 20.0 * std::log10(rms) : -200.0;
    a.peak = peak;
    if (peak < 1e-4) return a;
    // Rising zero crossings, placed between samples by linear interpolation,
    // with a little hysteresis so noise near zero does not count.
    const double hyst = peak * 0.05;
    double first = -1, last = -1;
    size_t count = 0;
    bool below = frames[0] < -hyst;
    for (size_t i = 1; i < n; ++i) {
        const double v = frames[i * channels];
        if (v < -hyst) below = true;
        if (below && v >= 0) {
            const double p = frames[(i - 1) * channels];
            const double t = double(i - 1) + (p < 0 ? -p / (v - p) : 0.0);
            if (first < 0) first = t;
            last = t;
            ++count;
            below = false;
        }
    }
    if (count >= 2 && last > first) a.frequency_hz = double(count - 1) * double(rate) / (last - first);
    return a;
}

size_t first_onset(const float* frames, size_t n, uint32_t channels, float threshold) {
    for (size_t i = 0; i < n; ++i) {
        if (std::abs(frames[i * channels]) >= threshold) return i;
    }
    return SIZE_MAX;
}

void Tone::fill(float* out, uint32_t frames) {
    constexpr double kTwoPi = 6.283185307179586;
    const double step = kTwoPi * hz_ / double(rate_);
    for (uint32_t i = 0; i < frames; ++i) {
        const float v = float(amp_ * std::sin(phase_));
        for (uint32_t c = 0; c < ch_; ++c) out[size_t(i) * ch_ + c] = v;
        phase_ += step;
        if (phase_ >= kTwoPi) phase_ -= kTwoPi;
    }
}

// ---- WAV --------------------------------------------------------------------------------------

namespace {

uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

}  // namespace

bool read_wav(const std::string& path, WavData& out, std::string* err) {
    auto fail = [&](const std::string& why) {
        if (err) *err = path + ": " + why;
        return false;
    };
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return fail("cannot open");
    std::vector<uint8_t> d;
    uint8_t chunk[65536];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof chunk, f)) > 0) d.insert(d.end(), chunk, chunk + got);
    std::fclose(f);
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) {
        return fail("not a RIFF/WAVE file");
    }
    uint16_t fmt_tag = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t* data = nullptr;
    size_t data_size = 0;
    for (size_t at = 12; at + 8 <= d.size();) {
        const uint32_t size = le32(&d[at + 4]);
        const size_t body = at + 8;
        if (size > d.size() - body) {
            if (!std::memcmp(&d[at], "data", 4)) {  // a truncated data chunk: take what is there
                data = &d[body];
                data_size = d.size() - body;
            }
            break;
        }
        if (!std::memcmp(&d[at], "fmt ", 4) && size >= 16) {
            fmt_tag = le16(&d[body]);
            ch = le16(&d[body + 2]);
            rate = le32(&d[body + 4]);
            bits = le16(&d[body + 14]);
            if (fmt_tag == 0xFFFE && size >= 26) fmt_tag = le16(&d[body + 24]);  // WAVE_FORMAT_EXTENSIBLE
        } else if (!std::memcmp(&d[at], "data", 4)) {
            data = &d[body];
            data_size = size;
        }
        at = body + size + (size & 1);
    }
    if (!data || !ch || !rate) return fail("no fmt or data chunk");
    const bool is_float = fmt_tag == 3 && bits == 32;
    const bool is_pcm = fmt_tag == 1 && (bits == 16 || bits == 24 || bits == 32);
    if (!is_float && !is_pcm) return fail("only PCM 16/24/32-bit and float32 WAV are read");
    const size_t bps = bits / 8;
    const size_t samples = data_size / bps;
    out.rate = rate;
    out.channels = ch;
    out.frames.resize(samples - samples % ch);
    for (size_t i = 0; i < out.frames.size(); ++i) {
        const uint8_t* p = data + i * bps;
        float v = 0;
        if (is_float) {
            const uint32_t b = le32(p);
            std::memcpy(&v, &b, 4);
        } else if (bits == 16) {
            v = float(int16_t(le16(p))) / 32768.0f;
        } else if (bits == 24) {
            const int32_t s = int32_t((uint32_t(p[0]) << 8) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 24)) >> 8;
            v = float(s) / 8388608.0f;
        } else {
            v = float(double(int32_t(le32(p))) / 2147483648.0);
        }
        out.frames[i] = v;
    }
    return true;
}

bool write_wav(const std::string& path, const WavData& in, std::string* err) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        if (err) *err = path + ": cannot create";
        return false;
    }
    auto put32 = [&](uint32_t v) {
        const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
        std::fwrite(b, 1, 4, f);
    };
    auto put16 = [&](uint16_t v) {
        const uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
        std::fwrite(b, 1, 2, f);
    };
    const uint32_t bytes = uint32_t(in.frames.size() * 4);
    std::fwrite("RIFF", 1, 4, f);
    put32(36 + bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    put32(16);
    put16(3);  // IEEE float
    put16(uint16_t(in.channels));
    put32(in.rate);
    put32(in.rate * in.channels * 4);
    put16(uint16_t(in.channels * 4));
    put16(32);
    std::fwrite("data", 1, 4, f);
    put32(bytes);
    std::fwrite(in.frames.data(), 4, in.frames.size(), f);  // little-endian hosts only (x86, arm)
    const bool ok = std::fclose(f) == 0;
    if (!ok && err) *err = path + ": write failed";
    return ok;
}

}  // namespace broremote::audio
