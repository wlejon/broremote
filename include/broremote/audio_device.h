#pragma once
// The audio lane's device layer: the few things broremote asks of a
// machine's audio system, each over the platform's own API.
//
//   host (Linux, PipeWire)
//     Monitor capture   what the machine is playing: the default sink's
//                       monitor (stream.capture.sink), following the default.
//     Virtual mic       a source node (media.class Audio/Source) that other
//                       programs record from as a microphone, fed by the
//                       viewer's mic. Optionally the default source while it
//                       exists (the previous default is put back).
//   viewer (Windows, WASAPI)
//     Microphone        the default communications capture device, opened in
//                       the Communications category so Windows applies its
//                       communications processing (echo cancellation where
//                       the device or Windows provides it), with the echo
//                       reference pointed at the speakers below.
//     Speakers          the default render device.
//
// A backend that cannot do something says so (open_* returns null with an
// error); the lane then goes without that direction. The PipeWire backend
// survives its daemon restarting: it reconnects, and every endpoint that was
// running is made again on the new connection (the virtual mic reappears,
// the default source is claimed again); meanwhile status() says why there
// is no audio. Every endpoint calls
// its function on its own realtime thread with interleaved float frames;
// the function must not block, lock or allocate.
//
// Not a general audio library: broaudio is the ecosystem's audio engine.
// This is only the lane's two ends, kept free of it so broremote stays
// platform APIs plus its transport and codec siblings.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace broremote::audio {

// A capture endpoint delivers frames captured at t_us (the steady clock,
// now_us(): when the first frame was captured, as near as the API says).
using CaptureFn = std::function<void(const float* frames, uint32_t n, int64_t t_us)>;
// A playback endpoint asks for frames that leave it at t_us (heard, or
// handed to the graph's next node).
using PlaybackFn = std::function<void(float* frames, uint32_t n, int64_t t_us)>;

enum class Source {
    Microphone,  // the default (communications) capture device
    Monitor,     // what this machine plays: the default output's monitor
};
enum class Sink {
    Speakers,    // the default output device
    VirtualMic,  // a microphone node other programs record from
};

struct CaptureSpec {
    Source source = Source::Microphone;
    uint32_t rate = 48000;
    uint32_t channels = 1;
    uint32_t period_frames = 480;  // a hint
    std::string name;              // shown in the system mixer
    std::string device;            // Microphone: a device whose name contains this; empty: the default
    std::string echo_reference;    // Microphone: the speakers whose sound to cancel (as PlaybackSpec::device)
};

struct PlaybackSpec {
    Sink sink = Sink::Speakers;
    uint32_t rate = 48000;
    uint32_t channels = 2;
    uint32_t period_frames = 480;
    std::string name;              // VirtualMic: the node name (unique, no spaces)
    std::string description;       // VirtualMic: what the mixer shows, e.g. "broremote: laptop mic"
    bool make_default = false;     // VirtualMic: the default source while it exists
    std::string device;            // Speakers: a device whose name contains this; empty: the default
};

struct EndpointInfo {
    std::string device;            // the device or node it is on
    uint32_t period_frames = 0;    // 0: not known yet
    double latency_ms = 0;         // the device's own latency, as the API reports it (0: unknown)
    bool echo_cancel = false;      // Microphone: echo cancellation is on
    bool is_default = false;       // VirtualMic: it was made the default source
};

class Endpoint {
public:
    virtual ~Endpoint() = default;  // stops
    // Starts calling the function. False with *err when the device cannot run.
    virtual bool start(std::string* err) = 0;
    // After it returns the function is not running and will not be called again.
    virtual void stop() = 0;
    [[nodiscard]] virtual EndpointInfo info() const = 0;
};

class Backend {
public:
    virtual ~Backend() = default;
    [[nodiscard]] virtual const char* name() const = 0;
    // Null with *err when this backend cannot do it.
    virtual std::unique_ptr<Endpoint> open_capture(const CaptureSpec&, CaptureFn, std::string* err) = 0;
    virtual std::unique_ptr<Endpoint> open_playback(const PlaybackSpec&, PlaybackFn, std::string* err) = 0;
    // The devices a spec's `device` can name (empty where the backend has no choice).
    struct Device {
        std::string name;
        bool capture = false;
        bool is_default = false;  // the default (communications capture / console render)
    };
    [[nodiscard]] virtual std::vector<Device> devices() { return {}; }
    // What is wrong with the audio system right now, for a lane's notes
    // (e.g. "the PipeWire daemon went away; reconnecting"); empty when all
    // is well. Any thread.
    [[nodiscard]] virtual std::string status() const { return {}; }
};

// This machine's backend: PipeWire on Linux (when built with libpipewire and
// a daemon answers), WASAPI on Windows. Null with *err when there is none.
std::shared_ptr<Backend> platform_backend(std::string* err);

// ---- timer-paced endpoints (no device) -------------------------------------------------------
// Each runs a thread that calls its function every period, on the steady
// clock: for tests, and for a viewer that feeds its mic from a file or tone.

// Generator: fills `frames` (the next n frames of the source).
using GenerateFn = std::function<void(float* frames, uint32_t n)>;

// A capture endpoint whose frames come from `generate` instead of a device.
std::unique_ptr<Endpoint> generated_capture(uint32_t rate, uint32_t channels, uint32_t period_frames,
                                            GenerateFn generate, CaptureFn deliver, std::string name = "generated");
// A playback endpoint that pulls from `pull` and plays nowhere; `played`
// (optional) sees every block after it was pulled, as if heard at t_us.
std::unique_ptr<Endpoint> paced_playback(uint32_t rate, uint32_t channels, uint32_t period_frames, PlaybackFn pull,
                                         CaptureFn played = {}, std::string name = "paced");

// A whole backend of paced endpoints, for tests: captures come from
// `generate` (per source), playback goes to `played` (per sink).
class PacedBackend final : public Backend {
public:
    std::function<void(Source, float* frames, uint32_t n, uint32_t channels, uint32_t rate)> generate;
    std::function<void(Sink, const float* frames, uint32_t n, uint32_t channels, uint32_t rate, int64_t t_us)> played;
    uint32_t period_frames = 0;  // 0: each spec's own

    [[nodiscard]] const char* name() const override { return "paced"; }
    std::unique_ptr<Endpoint> open_capture(const CaptureSpec&, CaptureFn, std::string* err) override;
    std::unique_ptr<Endpoint> open_playback(const PlaybackSpec&, PlaybackFn, std::string* err) override;
};

}  // namespace broremote::audio
