// The PipeWire backend (Linux): the host's side of the audio lane.
//
//   Monitor capture   a capture stream with stream.capture.sink, which the
//                     session manager links to the default sink's monitor
//                     (and moves when the default changes): what the
//                     machine is playing.
//   Virtual mic       an output stream whose media.class is Audio/Source,
//                     not linked anywhere by us: to the graph it is a
//                     microphone, which pw-record, bro.mic or a browser can
//                     record from. It exists while the endpoint runs.
//                     make_default sets the "default" metadata's
//                     default.configured.audio.source to it, and the value
//                     it replaced (or its absence) comes back when the last
//                     such endpoint stops.
//
// One pw_thread_loop and core connection per backend; streams process on
// PipeWire's realtime data thread (PW_STREAM_FLAG_RT_PROCESS). Streams ask
// the graph for their period through node.latency, and opt out of
// WirePlumber's per-stream state (state.restore-*), so a session leaves
// nothing behind in its state files.
//
// The daemon restarting (`systemctl --user restart pipewire`) kills the
// connection: the core reports EPIPE, and every stream on it is dead. A
// timer on the loop then reconnects (twice a second until a daemon answers),
// makes every running endpoint's stream again on the new core, and once the
// new "default" metadata is bound claims the default source again for the
// virtual mics that held it. Until then status() says what is wrong.
#include "broremote/audio.h"
#include "broremote/audio_device.h"

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>
#include <sys/resource.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

namespace broremote::audio {

namespace {

constexpr const char* kDefaultSourceKey = "default.configured.audio.source";

class PwEndpoint;

// The loop lock, RAII.
struct LoopLock {
    pw_thread_loop* l;
    explicit LoopLock(pw_thread_loop* loop) : l(loop) { pw_thread_loop_lock(l); }
    ~LoopLock() { pw_thread_loop_unlock(l); }
};

class PwBackend final : public Backend, public std::enable_shared_from_this<PwBackend> {
public:
    static std::shared_ptr<PwBackend> create(std::string* err);
    ~PwBackend() override;

    [[nodiscard]] const char* name() const override { return "pipewire"; }
    std::unique_ptr<Endpoint> open_capture(const CaptureSpec&, CaptureFn, std::string* err) override;
    std::unique_ptr<Endpoint> open_playback(const PlaybackSpec&, PlaybackFn, std::string* err) override;
    [[nodiscard]] std::string status() const override {
        std::lock_guard<std::mutex> lk(status_m_);
        return status_;
    }

    // Loop lock held.
    bool claim_default(const std::string& node);
    void release_default(const std::string& node);
    [[nodiscard]] bool connected() const { return core != nullptr && !lost_; }
    void add_endpoint(PwEndpoint* e) { endpoints_.push_back(e); }
    void remove_endpoint(PwEndpoint* e) { endpoints_.erase(std::remove(endpoints_.begin(), endpoints_.end(), e), endpoints_.end()); }

    pw_thread_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;

private:
    bool sync();  // a core round trip (loop lock held, not on the loop thread)
    bool connect_core(std::string* err);  // loop lock held
    void drop_core();                     // loop lock held
    void reconnect();                     // the loop thread
    // Nothing wrong now still reports a standing condition (rt_note_).
    void set_status(std::string s) {
        std::lock_guard<std::mutex> lk(status_m_);
        status_ = s.empty() ? rt_note_ : std::move(s);
    }

    static void on_core_done(void* data, uint32_t id, int seq);
    static void on_core_error(void* data, uint32_t id, int seq, int res, const char* message);
    static void on_global(void* data, uint32_t id, uint32_t permissions, const char* type, uint32_t version,
                          const spa_dict* props);
    static int on_property(void* data, uint32_t subject, const char* key, const char* type, const char* value);
    static void on_timer(void* data, uint64_t expirations);

    spa_hook core_listener_{};
    pw_registry* registry_ = nullptr;
    spa_hook registry_listener_{};
    pw_metadata* meta_ = nullptr;
    spa_hook meta_listener_{};
    spa_source* timer_ = nullptr;
    int pending_seq_ = -1;
    bool done_ = false;
    bool lost_ = false;                  // the daemon went away; the timer reconnects
    std::vector<PwEndpoint*> endpoints_;  // running ones, made again on a new core

    // The "default" metadata's default.configured.audio.source, as it is now.
    std::optional<std::string> configured_source_;
    // Before the first claim: what to put back.
    std::optional<std::string> saved_source_;
    std::vector<std::string> owners_;  // virtual mics that are (in order) the default

    mutable std::mutex status_m_;
    std::string status_;
    std::string rt_note_;  // set once in create(), before the loop runs
};

// The kernel SIGKILLs a process whose realtime thread runs past
// RLIMIT_RTTIME without blocking, and an unprivileged process cannot raise a
// hard limit once lowered. PipeWire's module-rt lowers it, in every context,
// to what xdg-desktop-portal's Realtime interface says rtkit allows; a portal
// that came up before rtkit was activated says 0 (until it is restarted).
// With 0, the first tick that lands while the data thread is processing kills
// the host: a compositor hosting the lane dies when something records its
// mic. Below this budget the data loop runs without realtime instead.
constexpr rlim_t kMinRtTimeUs = 10000;

bool rt_budget_too_small() {
    rlimit rl{};
    if (getrlimit(RLIMIT_RTTIME, &rl) != 0) return false;
    return rl.rlim_max != RLIM_INFINITY && rl.rlim_max < kMinRtTimeUs;
}

// ---- endpoints ------------------------------------------------------------------------------

class PwEndpoint final : public Endpoint {
public:
    PwEndpoint(std::shared_ptr<PwBackend> be, bool capture, uint32_t rate, uint32_t channels, uint32_t period,
               std::string node_name, std::string description, bool virtual_mic, bool make_default, CaptureFn cfn,
               PlaybackFn pfn)
        : be_(std::move(be)), capture_(capture), rate_(rate), channels_(channels),
          period_(period ? period : rate / 100), node_name_(std::move(node_name)),
          description_(std::move(description)), virtual_mic_(virtual_mic), make_default_(make_default),
          cfn_(std::move(cfn)), pfn_(std::move(pfn)) {}
    ~PwEndpoint() override { stop(); }

    bool start(std::string* err) override;
    void stop() override;
    [[nodiscard]] EndpointInfo info() const override {
        EndpointInfo i;
        i.device = description_.empty() ? node_name_ : description_;
        i.period_frames = period_seen_.load(std::memory_order_relaxed);
        i.latency_ms = double(delay_us_.load(std::memory_order_relaxed)) / 1000.0;
        i.is_default = is_default_;
        return i;
    }

    // Loop lock held. The stream on the backend's current core.
    bool connect_stream(std::string* err);
    // Loop lock held. The stream goes (its core died, or the endpoint stops).
    void drop_stream();

private:
    static void on_process(void* data);
    int64_t delay_us();

    std::shared_ptr<PwBackend> be_;
    bool capture_;
    uint32_t rate_, channels_, period_;
    std::string node_name_, description_;
    bool virtual_mic_, make_default_;
    CaptureFn cfn_;
    PlaybackFn pfn_;
    bool running_ = false;
    pw_stream* stream_ = nullptr;
    spa_hook listener_{};
    bool is_default_ = false;
    std::atomic<uint32_t> period_seen_{0};
    std::atomic<int64_t> delay_us_{0};
};

// ---- the backend ----------------------------------------------------------------------------

void PwBackend::on_core_done(void* data, uint32_t id, int seq) {
    auto* self = static_cast<PwBackend*>(data);
    if (id == PW_ID_CORE && seq == self->pending_seq_) {
        self->done_ = true;
        pw_thread_loop_signal(self->loop, false);
    }
}

void PwBackend::on_core_error(void* data, uint32_t id, int /*seq*/, int res, const char* message) {
    auto* self = static_cast<PwBackend*>(data);
    if (id != PW_ID_CORE || res != -EPIPE) return;
    // The daemon went away: the core and every stream on it are dead. The
    // timer cleans up and reconnects (not here: this is the core's own
    // callback).
    self->lost_ = true;
    self->set_status(std::string("the PipeWire daemon went away (") + (message ? message : "connection error") +
                     "); reconnecting");
    pw_thread_loop_signal(self->loop, false);  // a sync() waiting on the dead core gives up
}

void PwBackend::on_global(void* data, uint32_t id, uint32_t, const char* type, uint32_t, const spa_dict* props) {
    auto* self = static_cast<PwBackend*>(data);
    if (self->meta_ || !type || std::strcmp(type, PW_TYPE_INTERFACE_Metadata) != 0 || !props) return;
    const char* name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
    if (!name || std::strcmp(name, "default") != 0) return;
    self->meta_ = static_cast<pw_metadata*>(
        pw_registry_bind(self->registry_, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0));
    if (!self->meta_) return;
    static const pw_metadata_events events = [] {
        pw_metadata_events e{};
        e.version = PW_VERSION_METADATA_EVENTS;
        e.property = &PwBackend::on_property;
        return e;
    }();
    pw_metadata_add_listener(self->meta_, &self->meta_listener_, &events, self);
    // After a reconnect: the virtual mic that held the default holds it again.
    if (!self->owners_.empty()) {
        const std::string json = "{ \"name\": \"" + self->owners_.back() + "\" }";
        pw_metadata_set_property(self->meta_, 0, kDefaultSourceKey, "Spa:String:JSON", json.c_str());
    }
}

int PwBackend::on_property(void* data, uint32_t subject, const char* key, const char* /*type*/, const char* value) {
    auto* self = static_cast<PwBackend*>(data);
    if (subject != 0) return 0;
    if (!key) {
        self->configured_source_.reset();  // every key cleared
    } else if (std::strcmp(key, kDefaultSourceKey) == 0) {
        if (value) self->configured_source_ = std::string(value);
        else self->configured_source_.reset();
    }
    return 0;
}

void PwBackend::on_timer(void* data, uint64_t) {
    auto* self = static_cast<PwBackend*>(data);
    if (self->lost_) self->reconnect();
}

bool PwBackend::sync() {
    if (!connected()) return false;
    done_ = false;
    pending_seq_ = pw_core_sync(core, PW_ID_CORE, 0);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!done_) {
        if (lost_ || std::chrono::steady_clock::now() >= until) return false;
        pw_thread_loop_timed_wait(loop, 1);
    }
    return true;
}

bool PwBackend::connect_core(std::string* err) {
    core = pw_context_connect(context, nullptr, 0);
    if (!core) {
        if (err) *err = "no PipeWire daemon answers";
        return false;
    }
    static const pw_core_events core_events = [] {
        pw_core_events e{};
        e.version = PW_VERSION_CORE_EVENTS;
        e.done = &PwBackend::on_core_done;
        e.error = &PwBackend::on_core_error;
        return e;
    }();
    pw_core_add_listener(core, &core_listener_, &core_events, this);
    registry_ = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    static const pw_registry_events registry_events = [] {
        pw_registry_events e{};
        e.version = PW_VERSION_REGISTRY_EVENTS;
        e.global = &PwBackend::on_global;
        return e;
    }();
    pw_registry_add_listener(registry_, &registry_listener_, &registry_events, this);
    lost_ = false;
    return true;
}

void PwBackend::drop_core() {
    if (meta_) {
        spa_hook_remove(&meta_listener_);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(meta_));
        meta_ = nullptr;
    }
    if (registry_) {
        spa_hook_remove(&registry_listener_);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
        registry_ = nullptr;
    }
    if (core) {
        spa_hook_remove(&core_listener_);
        pw_core_disconnect(core);
        core = nullptr;
    }
    configured_source_.reset();
}

void PwBackend::reconnect() {
    // The dead core's streams first (they are its proxies), then the core.
    for (PwEndpoint* e : endpoints_) e->drop_stream();
    drop_core();
    std::string err;
    if (!connect_core(&err)) {
        lost_ = true;
        set_status("PipeWire is not running: " + err + "; reconnecting");
        return;
    }
    std::string failed;
    for (PwEndpoint* e : endpoints_) {
        std::string why;
        if (!e->connect_stream(&why)) failed += (failed.empty() ? "" : "; ") + why;
    }
    set_status(failed.empty() ? std::string() : "after PipeWire came back: " + failed);
}

std::shared_ptr<PwBackend> PwBackend::create(std::string* err) {
    static std::once_flag once;
    std::call_once(once, [] { pw_init(nullptr, nullptr); });
    auto be = std::shared_ptr<PwBackend>(new PwBackend());
    be->loop = pw_thread_loop_new("broremote-audio", nullptr);
    if (!be->loop) {
        if (err) *err = "pw_thread_loop_new failed";
        return nullptr;
    }
    // module-rt sets the limit as the context loads it, so look after making
    // one too; the data thread starts with the first stream, later.
    auto make_context = [&](bool rt) {
        pw_properties* props = rt ? nullptr : pw_properties_new(PW_KEY_LOOP_RT_PRIO, "0", nullptr);
        return pw_context_new(pw_thread_loop_get_loop(be->loop), props, 0);
    };
    bool rt = !rt_budget_too_small();
    be->context = make_context(rt);
    if (be->context && rt && rt_budget_too_small()) {
        pw_context_destroy(be->context);
        rt = false;
        be->context = make_context(false);
    }
    if (!rt) {
        rlimit rl{};
        getrlimit(RLIMIT_RTTIME, &rl);
        be->rt_note_ = "audio runs without realtime scheduling: RLIMIT_RTTIME is " + std::to_string(rl.rlim_max) +
                       " us (PipeWire sets it from rtkit or xdg-desktop-portal's Realtime interface, and a "
                       "portal started before rtkit says 0: restart xdg-desktop-portal, then this host)";
        be->set_status({});
    }
    if (!be->context || pw_thread_loop_start(be->loop) < 0) {
        if (err) *err = "cannot start a PipeWire loop";
        return nullptr;
    }
    LoopLock lk(be->loop);
    if (!be->connect_core(err)) return nullptr;
    // One round trip for the globals (binding the metadata), one for its properties.
    if (!be->sync() || !be->sync()) {
        if (err) *err = "the PipeWire daemon did not answer";
        return nullptr;
    }
    // The reconnect timer: twice a second, doing nothing while connected.
    pw_loop* l = pw_thread_loop_get_loop(be->loop);
    be->timer_ = pw_loop_add_timer(l, &PwBackend::on_timer, be.get());
    if (be->timer_) {
        timespec first{0, 500000000}, every{0, 500000000};
        pw_loop_update_timer(l, be->timer_, &first, &every, false);
    }
    return be;
}

PwBackend::~PwBackend() {
    if (loop) pw_thread_loop_stop(loop);
    if (timer_) pw_loop_destroy_source(pw_thread_loop_get_loop(loop), timer_);
    drop_core();
    if (context) pw_context_destroy(context);
    if (loop) pw_thread_loop_destroy(loop);
}

bool PwBackend::claim_default(const std::string& node) {
    // No metadata on a live connection: no session manager to tell. With the
    // daemon away, the claim is made when the new metadata is bound.
    if (!meta_ && !lost_) return false;
    if (owners_.empty()) saved_source_ = configured_source_;
    owners_.push_back(node);
    if (!meta_) return true;
    const std::string json = "{ \"name\": \"" + node + "\" }";
    pw_metadata_set_property(meta_, 0, kDefaultSourceKey, "Spa:String:JSON", json.c_str());
    return true;
}

void PwBackend::release_default(const std::string& node) {
    auto it = std::find(owners_.begin(), owners_.end(), node);
    if (it == owners_.end()) return;
    owners_.erase(it);
    if (!meta_ || !connected()) {
        if (owners_.empty()) saved_source_.reset();
        return;
    }
    if (!owners_.empty()) {
        const std::string json = "{ \"name\": \"" + owners_.back() + "\" }";
        pw_metadata_set_property(meta_, 0, kDefaultSourceKey, "Spa:String:JSON", json.c_str());
        return;
    }
    if (saved_source_) pw_metadata_set_property(meta_, 0, kDefaultSourceKey, "Spa:String:JSON", saved_source_->c_str());
    else pw_metadata_set_property(meta_, 0, kDefaultSourceKey, nullptr, nullptr);
    saved_source_.reset();
    sync();  // the daemon has it before the stream (and maybe the process) goes
}

// ---- endpoints ------------------------------------------------------------------------------

int64_t PwEndpoint::delay_us() {
    pw_time t{};
    if (pw_stream_get_time_n(stream_, &t, sizeof t) < 0 || t.rate.denom == 0) return 0;
    const int64_t d = int64_t(t.delay) * int64_t(t.rate.num) * 1000000 / int64_t(t.rate.denom);
    return d > 0 ? d : 0;
}

void PwEndpoint::on_process(void* data) {
    auto* self = static_cast<PwEndpoint*>(data);
    pw_buffer* b = pw_stream_dequeue_buffer(self->stream_);
    if (!b) return;
    spa_buffer* buf = b->buffer;
    spa_data& d = buf->datas[0];
    const uint32_t stride = uint32_t(sizeof(float)) * self->channels_;
    const int64_t delay = self->delay_us();
    self->delay_us_.store(delay, std::memory_order_relaxed);
    if (self->capture_) {
        if (d.data && d.chunk) {
            const uint32_t offset = std::min(d.chunk->offset, d.maxsize);
            const uint32_t size = std::min(d.chunk->size, d.maxsize - offset);
            const uint32_t n = size / stride;
            if (n) {
                self->period_seen_.store(n, std::memory_order_relaxed);
                // The block ended about now (less the path from the device).
                const int64_t t = now_us() - delay - int64_t(n) * 1000000 / self->rate_;
                self->cfn_(reinterpret_cast<const float*>(static_cast<const uint8_t*>(d.data) + offset), n, t);
            }
        }
    } else if (d.data) {
        uint32_t n = d.maxsize / stride;
        if (b->requested) n = std::min<uint32_t>(uint32_t(b->requested), n);
        self->period_seen_.store(n, std::memory_order_relaxed);
        // What we hand over now reaches the graph's consumers this cycle.
        self->pfn_(static_cast<float*>(d.data), n, now_us() + delay);
        d.chunk->offset = 0;
        d.chunk->stride = int32_t(stride);
        d.chunk->size = n * stride;
    }
    pw_stream_queue_buffer(self->stream_, b);
}

bool PwEndpoint::connect_stream(std::string* err) {
    char latency[32];
    std::snprintf(latency, sizeof latency, "%u/%u", period_, rate_);
    pw_properties* props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_APP_NAME, "broremote",
                                             PW_KEY_NODE_NAME, node_name_.c_str(), PW_KEY_NODE_LATENCY, latency,
                                             "state.restore-props", "false", "state.restore-target", "false",
                                             nullptr);
    if (!description_.empty()) pw_properties_set(props, PW_KEY_NODE_DESCRIPTION, description_.c_str());
    if (capture_) {
        pw_properties_set(props, PW_KEY_MEDIA_CATEGORY, "Capture");
        pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
        pw_properties_set(props, PW_KEY_MEDIA_NAME, "desktop audio for a remote viewer");
    } else if (virtual_mic_) {
        pw_properties_set(props, PW_KEY_MEDIA_CLASS, "Audio/Source");
        pw_properties_set(props, PW_KEY_MEDIA_NAME, description_.c_str());
    } else {
        pw_properties_set(props, PW_KEY_MEDIA_CATEGORY, "Playback");
    }
    stream_ = pw_stream_new(be_->core, node_name_.c_str(), props);
    if (!stream_) {
        if (err) *err = node_name_ + ": pw_stream_new failed";
        return false;
    }
    static const pw_stream_events events = [] {
        pw_stream_events e{};
        e.version = PW_VERSION_STREAM_EVENTS;
        e.process = &PwEndpoint::on_process;
        return e;
    }();
    pw_stream_add_listener(stream_, &listener_, &events, this);

    uint8_t pod[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(pod, sizeof pod);
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = rate_;
    info.channels = channels_;
    if (channels_ == 1) {
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    } else if (channels_ == 2) {
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
    } else {
        info.flags = SPA_AUDIO_FLAG_UNPOSITIONED;
    }
    const spa_pod* params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
    // A virtual mic is not linked by us: it waits for something to record from it.
    const auto flags = pw_stream_flags(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS |
                                       (virtual_mic_ ? 0 : PW_STREAM_FLAG_AUTOCONNECT));
    const int r = pw_stream_connect(stream_, capture_ ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT, PW_ID_ANY, flags,
                                    params, 1);
    if (r < 0) {
        if (err) *err = node_name_ + ": pw_stream_connect: " + spa_strerror(r);
        drop_stream();
        return false;
    }
    return true;
}

void PwEndpoint::drop_stream() {
    if (!stream_) return;
    spa_hook_remove(&listener_);
    pw_stream_disconnect(stream_);
    pw_stream_destroy(stream_);
    stream_ = nullptr;
}

bool PwEndpoint::start(std::string* err) {
    LoopLock lk(be_->loop);
    if (running_) return true;
    // With the daemon away the endpoint still starts: its stream is made
    // when the backend reconnects.
    if (be_->connected() && !connect_stream(err)) return false;
    running_ = true;
    be_->add_endpoint(this);
    if (virtual_mic_ && make_default_) is_default_ = be_->claim_default(node_name_);
    return true;
}

void PwEndpoint::stop() {
    LoopLock lk(be_->loop);
    if (!running_) return;
    running_ = false;
    be_->remove_endpoint(this);
    if (is_default_) {
        be_->release_default(node_name_);
        is_default_ = false;
    }
    drop_stream();
}

std::unique_ptr<Endpoint> PwBackend::open_capture(const CaptureSpec& spec, CaptureFn fn, std::string* err) {
    if (spec.source != Source::Monitor) {
        if (err) *err = "the host captures only what it plays (Source::Monitor)";
        return nullptr;
    }
    return std::make_unique<PwEndpoint>(shared_from_this(), true, spec.rate, spec.channels, spec.period_frames,
                                        spec.name.empty() ? "broremote.desktop" : spec.name,
                                        "broremote: desktop audio", false, false, std::move(fn), PlaybackFn{});
}

std::unique_ptr<Endpoint> PwBackend::open_playback(const PlaybackSpec& spec, PlaybackFn fn, std::string* err) {
    if (spec.sink == Sink::VirtualMic && spec.name.empty()) {
        if (err) *err = "a virtual mic needs a node name";
        return nullptr;
    }
    const bool mic = spec.sink == Sink::VirtualMic;
    return std::make_unique<PwEndpoint>(shared_from_this(), false, spec.rate, spec.channels, spec.period_frames,
                                        mic ? spec.name : "broremote.playback", spec.description, mic,
                                        spec.make_default, CaptureFn{}, std::move(fn));
}

}  // namespace

std::shared_ptr<Backend> platform_backend(std::string* err) { return PwBackend::create(err); }

}  // namespace broremote::audio
