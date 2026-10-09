// bro.remote.connect(): the viewer side of bro.remote. Each call makes a
// ViewerSession (broremote/viewer.h): its threads connect, decode and run the
// audio lane, and the host shows the pictures and sends the input (bro's
// <remoteview>), reaching the session through viewerSession(). This file
// gives a page the session's state, numbers and controls.
//
// Sessions belong to the realm that made them: a new realm (a reload) and
// shutdownRemote() close them all.
//
// GC: as in api.cpp, every value that must survive an allocating call sits
// in a Persistent.

#include "viewer_api.h"

#include "api.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace broremote::api {

namespace {

ViewerHooks g_viewerHooks;

struct SessionListener {
    std::string event;
    std::shared_ptr<ev::Persistent> fn;
};

struct Entry {
    uint32_t id = 0;
    std::unique_ptr<ViewerSession> session;
    std::unique_ptr<ev::Persistent> object;  // the page's session object
    std::vector<SessionListener> listeners;
    // What the page was last told (the events).
    ViewerState reportedState = ViewerState::Connecting;
    uint64_t reportedStream = 0;
    bool reportedConfig = false;
    // stats(): the previous call's counters, for rates over the interval.
    uint64_t lastDecoded = 0, lastBytes = 0;
    size_t lastDecodeIndex = 0;
    Clock::time_point lastStats = Clock::now();
};

std::vector<std::unique_ptr<Entry>> g_sessions;
uint32_t g_nextId = 1;

Entry* entryById(uint32_t id) {
    for (auto& e : g_sessions) {
        if (e->id == id) return e.get();
    }
    return nullptr;
}

void destroyEntry(uint32_t id) {
    auto it = std::find_if(g_sessions.begin(), g_sessions.end(), [&](const auto& e) { return e->id == id; });
    if (it == g_sessions.end()) return;
    std::unique_ptr<Entry> entry = std::move(*it);
    g_sessions.erase(it);
    if (g_viewerHooks.sessionGone) g_viewerHooks.sessionGone(entry->session.get());
    entry->session.reset();  // closes and joins its threads
}

// ---- options ---------------------------------------------------------------

[[noreturn]] void optionError(const std::string& message) { ev::throwTypeError("bro.remote.connect: " + message); }

std::string strOpt(Value opts, const char* key, const std::string& def, bool* given = nullptr) {
    Value v = ev::getProperty(opts, key);
    if (ev::isUndefined(v) || ev::isNull(v)) return def;
    if (!ev::isString(v)) optionError(std::string(key) + " must be a string");
    if (given) *given = true;
    return ev::toUtf8(v);
}

bool boolOpt(Value opts, const char* key, bool def) {
    Value v = ev::getProperty(opts, key);
    if (ev::isUndefined(v) || ev::isNull(v)) return def;
    if (!ev::isBool(v)) optionError(std::string(key) + " must be true or false");
    return ev::toBool(v);
}

double numOpt(Value opts, const char* key, double def, double lo, double hi) {
    Value v = ev::getProperty(opts, key);
    if (ev::isUndefined(v) || ev::isNull(v)) return def;
    if (!ev::isNumber(v)) optionError(std::string(key) + " must be a number");
    const double d = ev::toDouble(v);
    if (!(d >= lo && d <= hi)) {
        optionError(std::string(key) + " must be between " + std::to_string(lo) + " and " + std::to_string(hi));
    }
    return d;
}

bool socketNameOk(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
               c == '-';
    });
}

ViewerOptions optionsFrom(std::span<const Value> args) {
    ViewerOptions o;
    o.client_name = "bro";
    if (args.empty() || ev::isUndefined(args[0]) || ev::isNull(args[0])) return o;
    if (!ev::isObject(args[0])) optionError("the argument must be an options object");
    ev::Persistent opts(args[0]);
    ConnectTarget& t = o.target;
    t.ssh_host = strOpt(opts.get(), "ssh", "");
    t.socket = strOpt(opts.get(), "socket", t.socket, &t.socket_given);
    if (!socketNameOk(t.socket)) optionError("socket must be 1-64 of [A-Za-z0-9._-]");
    t.ssh_command = strOpt(opts.get(), "sshCommand", t.ssh_command, &t.command_given);
    t.ssh_program = strOpt(opts.get(), "sshProgram", "");
    t.pty = boolOpt(opts.get(), "pty", t.pty);
    t.input_lane = boolOpt(opts.get(), "inputLane", t.input_lane);
    o.client_name = strOpt(opts.get(), "name", o.client_name);
    o.negotiate = boolOpt(opts.get(), "negotiate", o.negotiate);
    ViewerAudioOptions& a = o.audio;
    a.enabled = boolOpt(opts.get(), "audio", a.enabled);
    a.mic = boolOpt(opts.get(), "mic", a.mic);
    a.playback = boolOpt(opts.get(), "playback", a.playback);
    a.mic_muted = boolOpt(opts.get(), "micMuted", a.mic_muted);
    a.playback_muted = boolOpt(opts.get(), "playbackMuted", a.playback_muted);
    a.mic_device = strOpt(opts.get(), "micDevice", "");
    a.speaker_device = strOpt(opts.get(), "speakerDevice", "");
    a.mic_tone_hz = numOpt(opts.get(), "micTone", 0, 0, 20000);
    a.jitter_ms = uint32_t(numOpt(opts.get(), "audioBufferMs", a.jitter_ms, 5, 1000));
    return o;
}

// ---- reports ---------------------------------------------------------------

const char* stateName(ViewerState s) {
    switch (s) {
        case ViewerState::Connecting: return "connecting";
        case ViewerState::Connected: return "connected";
        case ViewerState::Closed: return "closed";
    }
    return "closed";
}

Value closedStatus() {
    ObjectBuilder st;
    st.set("state", "closed");
    st.set("message", "closed");
    st.set("failed", false);
    return st.get();
}

Value statusOf(Entry& e) {
    const ViewerStatus s = e.session->status();
    ObjectBuilder st;
    st.set("state", stateName(s.state));
    st.set("message", s.message);
    st.set("failed", s.failed);
    st.set("server", s.server);
    st.set("protocol", s.server.empty() ? std::string() : "1." + std::to_string(s.server_minor));
    if (s.have_config) {
        st.set("codec", codec_name(s.config.codec));
        st.set("width", double(s.config.width));
        st.set("height", double(s.config.height));
        st.set("fps", double(s.config.fps));
        st.set("stream", double(s.config.stream_id));
    } else {
        st.set("codec", ev::null());
        st.set("width", 0.0);
        st.set("height", 0.0);
        st.set("fps", 0.0);
        st.set("stream", 0.0);
    }
    st.set("decoder", s.decoder);
    st.set("hardware", s.hardware);
    st.set("inputLane", s.input_lane);
    st.set("inputLaneError", s.input_lane_error);
    st.set("micMuted", e.session->mic_muted());
    st.set("playbackMuted", e.session->playback_muted());
    CursorState c;
    if (e.session->cursor(c)) {
        ObjectBuilder cur;
        cur.set("visible", c.visible);
        cur.set("x", double(c.x));
        cur.set("y", double(c.y));
        cur.set("shape", c.shape);
        cur.set("locked", c.locked);
        st.set("cursor", cur.get());
    } else {
        st.set("cursor", ev::null());
    }
    return st.get();
}

// stats(): counters, and rates and means over the time since the last call.
Value statsOf(Entry& e) {
    const ViewerStats s = e.session->stats();
    const LatencyWindow w = e.session->latency().take_window();
    const Clock::time_point now = Clock::now();
    const double secs = std::max(1e-3, std::chrono::duration<double>(now - e.lastStats).count());
    ObjectBuilder o;
    o.set("packets", double(s.packets));
    o.set("decoded", double(s.decoded));
    o.set("failed", double(s.failed));
    o.set("keyframeRequests", double(s.keyframe_requests));
    o.set("gpuPictures", double(s.gpu_pictures));
    o.set("bytes", double(s.bytes));
    o.set("seconds", secs);
    o.set("fps", double(s.decoded - std::min(s.decoded, e.lastDecoded)) / secs);
    o.set("mbps", double(s.bytes - std::min(s.bytes, e.lastBytes)) * 8.0 / 1e6 / secs);
    // The decode times of the interval (the list is trimmed now and then: a
    // shorter list than last time starts over).
    size_t from = e.lastDecodeIndex <= s.decode_ms.size() ? e.lastDecodeIndex : 0;
    double sum = 0, mx = 0;
    for (size_t i = from; i < s.decode_ms.size(); ++i) {
        sum += s.decode_ms[i];
        mx = std::max(mx, s.decode_ms[i]);
    }
    const size_t n = s.decode_ms.size() - from;
    o.set("decodeMs", n ? sum / double(n) : 0.0);
    o.set("decodeMaxMs", mx);
    // Where a presented picture's time went (latency.h), means in ms; -1
    // for a figure not known yet.
    ObjectBuilder lat;
    lat.set("frames", double(w.frames));
    lat.set("rtt", w.rtt);
    lat.set("age", w.frames ? w.age : -1.0);
    lat.set("maxAge", w.frames ? w.max_age : -1.0);
    lat.set("queue", w.queue);
    lat.set("encode", w.encode);
    lat.set("wait", w.wait);
    lat.set("net", w.net);
    lat.set("dwait", w.dwait);
    lat.set("decode", w.decode);
    lat.set("present", w.present);
    lat.set("kbytes", w.kbytes);
    o.set("latency", lat.get());
    e.lastDecoded = s.decoded;
    e.lastBytes = s.bytes;
    e.lastDecodeIndex = s.decode_ms.size();
    e.lastStats = now;
    return o.get();
}

Value audioOf(Entry& e) {
    AudioSession::Stats a;
    if (!e.session->audio_stats(a)) return ev::null();
    ObjectBuilder o;
    o.set("connected", a.connected);
    o.set("closed", a.closed);
    o.set("playback", a.playback);
    o.set("mic", a.mic);
    o.set("micNode", a.mic_node);
    o.set("micDevice", a.mic_device);
    o.set("speakerDevice", a.speaker_device);
    o.set("echoCancel", a.echo_cancel);
    o.set("notes", a.notes);
    o.set("hostStatus", a.host_status);
    o.set("rttMs", a.rtt_ms);
    o.set("micLatencyMs", a.mic_latency_ms);
    o.set("playbackLatencyMs", a.playback_latency_ms);
    o.set("hostMicBufferMs", a.host_mic_buffer_ms);
    o.set("playbackBufferMs", a.playback_buffer_ms);
    o.set("playbackUnderruns", double(a.playback_underruns));
    o.set("hostMicUnderruns", double(a.host_mic_underruns));
    o.set("micPackets", double(a.mic_packets));
    o.set("playbackPackets", double(a.playback_packets));
    o.set("micMuted", e.session->mic_muted());
    o.set("playbackMuted", e.session->playback_muted());
    return o.get();
}

// probes(): the closed latency probes, summarised (ms).
Value probesOf(Entry& e) {
    const std::vector<Probe> ps = e.session->latency().probes();
    ObjectBuilder o;
    o.set("count", double(ps.size()));
    o.set("lost", double(e.session->latency().probes_lost()));
    auto summary = [&](const char* name, auto field) {
        std::vector<double> v;
        for (const Probe& p : ps) v.push_back(field(p));
        std::sort(v.begin(), v.end());
        ObjectBuilder s;
        double sum = 0;
        for (double x : v) sum += x;
        auto pct = [&](double q) { return v.empty() ? -1.0 : v[std::min(v.size() - 1, size_t(q * double(v.size())))]; };
        s.set("mean", v.empty() ? -1.0 : sum / double(v.size()));
        s.set("p50", pct(0.5));
        s.set("p90", pct(0.9));
        s.set("min", v.empty() ? -1.0 : v.front());
        s.set("max", v.empty() ? -1.0 : v.back());
        o.set(name, s.get());
    };
    summary("presented", [](const Probe& p) { return p.total_presented; });
    summary("decoded", [](const Probe& p) { return p.total_decoded; });
    ObjectBuilder parts;
    auto mean = [&](auto field) {
        double s = 0;
        for (const Probe& p : ps) s += field(p);
        return ps.empty() ? -1.0 : s / double(ps.size());
    };
    parts.set("uplink", mean([](const Probe& p) { return p.uplink; }));
    parts.set("queue", mean([](const Probe& p) { return p.queue; }));
    parts.set("encode", mean([](const Probe& p) { return p.encode; }));
    parts.set("wait", mean([](const Probe& p) { return p.wait; }));
    parts.set("net", mean([](const Probe& p) { return p.net; }));
    parts.set("dwait", mean([](const Probe& p) { return p.dwait; }));
    parts.set("decode", mean([](const Probe& p) { return p.decode; }));
    parts.set("present", mean([](const Probe& p) { return p.present; }));
    parts.set("rtt", mean([](const Probe& p) { return p.rtt; }));
    o.set("parts", parts.get());
    return o.get();
}

InputEvent inputFrom(Value evIn) {
    if (!ev::isObject(evIn)) ev::throwTypeError("sendInput: the argument must be an object");
    ev::Persistent obj(evIn);
    const std::string kind = strOpt(obj.get(), "kind", "");
    auto num = [&](const char* key) {
        Value v = ev::getProperty(obj.get(), key);
        if (!ev::isNumber(v) || !std::isfinite(ev::toDouble(v))) ev::throwTypeError(std::string("sendInput: ") + key + " must be a number");
        return ev::toDouble(v);
    };
    auto pressed = [&] { return boolOpt(obj.get(), "pressed", true); };
    if (kind == "key") return InputEvent::key(uint32_t(num("code")), pressed());
    if (kind == "button") return InputEvent::button(uint32_t(num("code")), pressed());
    if (kind == "motion") return InputEvent::motion(float(num("x")), float(num("y")));
    if (kind == "relative") return InputEvent::relative(float(num("x")), float(num("y")));
    if (kind == "wheel") return InputEvent::wheel(int32_t(num("x")), int32_t(num("y")));
    ev::throwTypeError("sendInput: kind is 'key', 'button', 'motion', 'relative' or 'wheel'");
}

// ---- events ----------------------------------------------------------------

bool knownSessionEvent(const std::string& e) { return e == "state" || e == "config"; }

void dispatchSession(Entry& e, const std::string& event, Value payloadIn) {
    ev::Persistent payload(payloadIn);
    ev::Persistent target(e.object->get());
    std::vector<std::shared_ptr<ev::Persistent>> targets;
    for (const auto& l : e.listeners) {
        if (l.event == event) targets.push_back(l.fn);
    }
    const uint32_t id = e.id;
    for (const auto& fn : targets) {
        const Value arg = payload.get();
        ev::CallResult r = ev::call(fn->get(), target.get(), std::span<const Value>(&arg, 1));
        (void)r;  // a listener that throws does not stop the others
        if (!entryById(id)) return;  // it closed the session
    }
    ev::Persistent handler(ev::getProperty(target.get(), "on" + event));
    if (ev::isFunction(handler.get())) {
        const Value arg = payload.get();
        ev::CallResult r = ev::call(handler.get(), target.get(), std::span<const Value>(&arg, 1));
        (void)r;
    }
}

}  // namespace

void setViewerHooks(ViewerHooks hooks) { g_viewerHooks = std::move(hooks); }

ViewerSession* viewerSession(Value object) {
    if (!ev::isObject(object)) return nullptr;
    for (auto& e : g_sessions) {
        if (e->object && e->object->get() == object) return e->session.get();
    }
    return nullptr;
}

void defineConnect(ObjectBuilder& remote) {
    remote.def("connect", 1, [](Value, std::span<const Value> args) -> Value {
        ViewerOptions options = optionsFrom(args);
        auto entry = std::make_unique<Entry>();
        entry->id = g_nextId++;
        entry->session = std::make_unique<ViewerSession>(g_viewerHooks.wake);
        if (g_viewerHooks.sessionStarting) g_viewerHooks.sessionStarting(entry->session.get(), options);
        const uint32_t id = entry->id;

        ObjectBuilder obj;
        obj.set("id", double(id));
        obj.set("target", options.target.describe());
        obj.def("status", 0, [id](Value, std::span<const Value>) -> Value {
            Entry* e = entryById(id);
            return e ? statusOf(*e) : closedStatus();
        });
        obj.def("stats", 0, [id](Value, std::span<const Value>) -> Value {
            Entry* e = entryById(id);
            return e ? statsOf(*e) : ev::null();
        });
        obj.def("audio", 0, [id](Value, std::span<const Value>) -> Value {
            Entry* e = entryById(id);
            return e ? audioOf(*e) : ev::null();
        });
        obj.def("probe", 0, [id](Value, std::span<const Value>) -> Value {
            Entry* e = entryById(id);
            return ev::fromBool(e && e->session->probe());
        });
        obj.def("probes", 0, [id](Value, std::span<const Value>) -> Value {
            Entry* e = entryById(id);
            return e ? probesOf(*e) : ev::null();
        });
        obj.def("sendInput", 1, [id](Value, std::span<const Value> a) -> Value {
            const InputEvent in = inputFrom(a.empty() ? ev::undefined() : a[0]);
            if (Entry* e = entryById(id)) e->session->send_input(in);
            return ev::undefined();
        });
        obj.def("setMicMuted", 1, [id](Value, std::span<const Value> a) -> Value {
            if (a.empty() || !ev::isBool(a[0])) return ev::throwTypeError("setMicMuted(muted): a boolean");
            if (Entry* e = entryById(id)) e->session->set_mic_muted(ev::toBool(a[0]));
            return ev::undefined();
        });
        obj.def("setPlaybackMuted", 1, [id](Value, std::span<const Value> a) -> Value {
            if (a.empty() || !ev::isBool(a[0])) return ev::throwTypeError("setPlaybackMuted(muted): a boolean");
            if (Entry* e = entryById(id)) e->session->set_playback_muted(ev::toBool(a[0]));
            return ev::undefined();
        });
        obj.def("close", 0, [id](Value, std::span<const Value>) -> Value {
            const bool was = entryById(id) != nullptr;
            destroyEntry(id);
            return ev::fromBool(was);
        });
        auto addL = [id](Value self, std::span<const Value> a) -> Value {
            if (a.size() < 2 || !ev::isString(a[0]) || !ev::isFunction(a[1])) {
                return ev::throwTypeError("session.on(event, fn): event is 'state' or 'config', fn a function");
            }
            const std::string event = ev::toUtf8(a[0]);
            if (!knownSessionEvent(event)) return ev::throwTypeError("session.on: unknown event '" + event + "'");
            Entry* e = entryById(id);
            if (!e) return self;
            for (const auto& l : e->listeners) {
                if (l.event == event && l.fn->get() == a[1]) return self;
            }
            e->listeners.push_back({event, std::make_shared<ev::Persistent>(a[1])});
            return self;
        };
        auto removeL = [id](Value self, std::span<const Value> a) -> Value {
            if (a.size() < 2 || !ev::isString(a[0])) return self;
            const std::string event = ev::toUtf8(a[0]);
            if (Entry* e = entryById(id)) {
                std::erase_if(e->listeners, [&](const SessionListener& l) { return l.event == event && l.fn->get() == a[1]; });
            }
            return self;
        };
        obj.def("on", 2, addL);
        obj.def("addEventListener", 2, addL);
        obj.def("off", 2, removeL);
        obj.def("removeEventListener", 2, removeL);
        obj.set("onstate", ev::null());
        obj.set("onconfig", ev::null());

        entry->object = std::make_unique<ev::Persistent>(obj.get());
        ViewerSession* session = entry->session.get();
        g_sessions.push_back(std::move(entry));
        session->start(options);
        return obj.get();
    });
}

void tickViewers() {
    // A listener may close sessions (any of them): walk by id.
    std::vector<uint32_t> ids;
    for (auto& e : g_sessions) ids.push_back(e->id);
    for (uint32_t id : ids) {
        Entry* e = entryById(id);
        if (!e) continue;
        const ViewerStatus s = e->session->status();
        // In the order it happened: connected, then the stream, then closed.
        auto stateEvent = [&] {
            e->reportedState = s.state;
            ObjectBuilder p;
            p.set("type", "state");
            p.set("state", stateName(s.state));
            p.set("message", s.message);
            p.set("failed", s.failed);
            dispatchSession(*e, "state", p.get());
            return (e = entryById(id)) != nullptr;
        };
        if (s.state != e->reportedState && s.state != ViewerState::Closed && !stateEvent()) continue;
        if (s.have_config && (!e->reportedConfig || s.config.stream_id != e->reportedStream)) {
            e->reportedConfig = true;
            e->reportedStream = s.config.stream_id;
            ObjectBuilder p;
            p.set("type", "config");
            p.set("codec", codec_name(s.config.codec));
            p.set("width", double(s.config.width));
            p.set("height", double(s.config.height));
            p.set("decoder", s.decoder);
            p.set("hardware", s.hardware);
            dispatchSession(*e, "config", p.get());
            if (!(e = entryById(id))) continue;
        }
        if (s.state != e->reportedState) stateEvent();
    }
}

void closeAllViewers() {
    while (!g_sessions.empty()) destroyEntry(g_sessions.back()->id);
}

}  // namespace broremote::api
