// Timer-paced endpoints: a thread per endpoint, one period at a time on the
// steady clock (sleep_until, so the pace does not drift with the work).
#include "broremote/audio.h"
#include "broremote/audio_device.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace broremote::audio {

namespace {

class PacedEndpoint final : public Endpoint {
public:
    // tick(buffer, n, t_us) runs once per period.
    PacedEndpoint(uint32_t rate, uint32_t channels, uint32_t period, std::string name,
                  std::function<void(float*, uint32_t, int64_t)> tick, bool capture)
        : rate_(rate), channels_(channels), period_(period ? period : rate / 100), name_(std::move(name)),
          tick_(std::move(tick)), capture_(capture) {}
    ~PacedEndpoint() override { stop(); }

    bool start(std::string*) override {
        if (thread_.joinable()) return true;
        stop_ = false;
        thread_ = std::thread([this] { run(); });
        return true;
    }
    void stop() override {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }
    [[nodiscard]] EndpointInfo info() const override {
        EndpointInfo i;
        i.device = name_;
        i.period_frames = period_;
        return i;
    }

private:
    void run() {
        std::vector<float> buf(size_t(period_) * channels_);
        const auto step = std::chrono::nanoseconds(int64_t(period_) * 1000000000ll / rate_);
        auto next = std::chrono::steady_clock::now();
#ifdef _WIN32
        // sleep_until rounds to the system timer tick (up to 15.6 ms) here:
        // a high-resolution waitable timer keeps the period.
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
#endif
        while (!stop_) {
            next += step;
#ifdef _WIN32
            const auto wait = next - std::chrono::steady_clock::now();
            if (timer && wait > std::chrono::microseconds(0)) {
                LARGE_INTEGER due;
                due.QuadPart = -int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(wait).count() / 100);
                if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
            } else {
                std::this_thread::sleep_until(next);
            }
#else
            std::this_thread::sleep_until(next);
#endif
            // A capture's block ended now, so it began a period ago; a
            // playback's block leaves now.
            const int64_t now = now_us();
            const int64_t t = capture_ ? now - int64_t(period_) * 1000000 / rate_ : now;
            tick_(buf.data(), period_, t);
            // Fallen far behind (a stalled machine): restart the pace rather than catch up in a burst.
            if (std::chrono::steady_clock::now() - next > step * 8) next = std::chrono::steady_clock::now();
        }
#ifdef _WIN32
        if (timer) CloseHandle(timer);
#endif
    }

    uint32_t rate_, channels_, period_;
    std::string name_;
    std::function<void(float*, uint32_t, int64_t)> tick_;
    bool capture_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace

std::unique_ptr<Endpoint> generated_capture(uint32_t rate, uint32_t channels, uint32_t period_frames,
                                            GenerateFn generate, CaptureFn deliver, std::string name) {
    return std::make_unique<PacedEndpoint>(
        rate, channels, period_frames, std::move(name),
        [generate = std::move(generate), deliver = std::move(deliver)](float* buf, uint32_t n, int64_t t) {
            generate(buf, n);
            deliver(buf, n, t);
        },
        true);
}

std::unique_ptr<Endpoint> paced_playback(uint32_t rate, uint32_t channels, uint32_t period_frames, PlaybackFn pull,
                                         CaptureFn played, std::string name) {
    return std::make_unique<PacedEndpoint>(
        rate, channels, period_frames, std::move(name),
        [pull = std::move(pull), played = std::move(played)](float* buf, uint32_t n, int64_t t) {
            pull(buf, n, t);
            if (played) played(buf, n, t);
        },
        false);
}

std::unique_ptr<Endpoint> PacedBackend::open_capture(const CaptureSpec& spec, CaptureFn fn, std::string*) {
    const Source source = spec.source;
    const uint32_t ch = spec.channels, rate = spec.rate;
    auto gen = generate;
    return generated_capture(
        rate, ch, period_frames ? period_frames : spec.period_frames,
        [gen, source, ch, rate](float* buf, uint32_t n) {
            if (gen) gen(source, buf, n, ch, rate);
            else std::fill(buf, buf + size_t(n) * ch, 0.0f);
        },
        std::move(fn), spec.name.empty() ? "paced capture" : spec.name);
}

std::unique_ptr<Endpoint> PacedBackend::open_playback(const PlaybackSpec& spec, PlaybackFn fn, std::string*) {
    const Sink sink = spec.sink;
    const uint32_t ch = spec.channels, rate = spec.rate;
    auto tap = played;
    CaptureFn after;
    if (tap) after = [tap, sink, ch, rate](const float* f, uint32_t n, int64_t t) { tap(sink, f, n, ch, rate, t); };
    return paced_playback(rate, ch, period_frames ? period_frames : spec.period_frames, std::move(fn),
                          std::move(after), spec.description.empty() ? "paced playback" : spec.description);
}

#if !defined(_WIN32) && !BROREMOTE_HAS_PIPEWIRE
std::shared_ptr<Backend> platform_backend(std::string* err) {
    if (err) *err = "no audio backend in this build (PipeWire on Linux, WASAPI on Windows)";
    return nullptr;
}
#endif

}  // namespace broremote::audio
