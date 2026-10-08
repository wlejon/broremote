// bro.remote: hosting a broremote server from script. The server lives here
// (one per process); the host feeds it through HostHooks, so nothing below
// knows what a frame comes from or where input goes.
//
// GC: bronze's heap moves (embed.h, "THE GC CONTRACT"). Every value that
// must survive an allocating call sits in a Persistent; option values are
// converted to C++ as soon as they are read.

#include "api.h"
#include "object_builder.h"

#include "broremote/codec.h"

#include <algorithm>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace broremote::api {

namespace {

HostHooks g_hooks;
std::unique_ptr<Server> g_server;
ServerConfig g_config;
size_t g_lastClients = 0;  // the client count last reported to listeners

struct Listener {
    std::string event;
    std::shared_ptr<ev::Persistent> fn;
};
std::vector<Listener> g_listeners;
// The realm's bro.remote, for its onattach / ondetach properties.
std::unique_ptr<ev::Persistent> g_namespace;

// ---- options ---------------------------------------------------------------

[[noreturn]] void typeError(const std::string& message) { ev::throwTypeError("bro.remote.host: " + message); }

// A string option, or `def` when absent.
std::string stringOption(Value opts, const char* key, const std::string& def) {
    Value v = ev::getProperty(opts, key);
    if (ev::isUndefined(v) || ev::isNull(v)) return def;
    if (!ev::isString(v)) typeError(std::string(key) + " must be a string");
    return ev::toUtf8(v);
}

// A positive whole-number option, or `def` when absent.
uint32_t countOption(Value opts, const char* key, uint32_t def) {
    Value v = ev::getProperty(opts, key);
    if (ev::isUndefined(v) || ev::isNull(v)) return def;
    if (!ev::isNumber(v)) typeError(std::string(key) + " must be a number");
    const double d = ev::toDouble(v);
    if (!(d >= 1.0 && d <= 4294967295.0)) typeError(std::string(key) + " must be at least 1");
    return static_cast<uint32_t>(d);
}

Codec codecNamed(const std::string& name) {
    auto c = parse_codec(name);
    if (!c) typeError("unknown codec '" + name + "' (raw, h264, hevc or av1)");
    return *c;
}

// `codecs`: a name or an array of names, in preference order.
std::vector<Codec> codecsOption(Value optsIn, const std::vector<Codec>& def) {
    ev::Persistent opts(optsIn);
    ev::Persistent v(ev::getProperty(opts.get(), "codecs"));
    if (ev::isUndefined(v.get()) || ev::isNull(v.get())) return def;
    if (ev::isString(v.get())) return {codecNamed(ev::toUtf8(v.get()))};
    if (!ev::isObject(v.get())) typeError("codecs must be a codec name or an array of them");
    Value lenV = ev::getProperty(v.get(), "length");
    if (!ev::isNumber(lenV)) typeError("codecs must be a codec name or an array of them");
    const double len = ev::toDouble(lenV);
    if (!(len >= 1.0 && len <= 16.0)) typeError("codecs must name 1 to 16 codecs");
    std::vector<Codec> out;
    for (uint32_t i = 0; i < static_cast<uint32_t>(len); ++i) {
        Value e = ev::getElement(v.get(), i);
        if (!ev::isString(e)) typeError("codecs must be strings");
        const Codec c = codecNamed(ev::toUtf8(e));
        if (std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
    }
    return out;
}

ServerConfig configFrom(std::span<const Value> args) {
    ServerConfig cfg;
    cfg.name = "bro";
    if (args.empty() || ev::isUndefined(args[0]) || ev::isNull(args[0])) return cfg;
    if (!ev::isObject(args[0])) typeError("the argument must be an options object");
    ev::Persistent opts(args[0]);
    cfg.socket_name = stringOption(opts.get(), "socket", cfg.socket_name);
    cfg.codecs = codecsOption(opts.get(), cfg.codecs);
    cfg.bitrate_kbps = countOption(opts.get(), "bitrateKbps", cfg.bitrate_kbps);
    cfg.fps = countOption(opts.get(), "fps", cfg.fps);
    cfg.name = stringOption(opts.get(), "name", cfg.name);
    return cfg;
}

bool sameConfig(const ServerConfig& a, const ServerConfig& b) {
    return a.socket_name == b.socket_name && a.codecs == b.codecs && a.bitrate_kbps == b.bitrate_kbps &&
           a.fps == b.fps && a.name == b.name;
}

// ---- the server ------------------------------------------------------------

void stopServer() {
    if (!g_server) return;
    // The host lets go first, then the server releases what it still holds.
    if (g_hooks.serverChanged) g_hooks.serverChanged(nullptr, g_config);
    g_server.reset();
    g_lastClients = 0;
}

Value codecList(const std::vector<Codec>& codecs) {
    ArrayBuilder arr;
    for (Codec c : codecs) arr.push(std::string(codec_name(c)));
    return arr.get();
}

Value statusObject() {
    ObjectBuilder st;
    st.set("hosting", g_server != nullptr);
    if (!g_server) {
        st.set("socket", ev::null());
        st.set("socketPath", ev::null());
        st.set("clients", 0.0);
        st.set("codec", ev::null());
        st.set("width", 0.0);
        st.set("height", 0.0);
        return st.get();
    }
    st.set("socket", g_config.socket_name);
    st.set("socketPath", g_server->socket_path());
    st.set("clients", static_cast<double>(g_server->client_count()));
    const auto stream = g_server->stream();
    if (stream) st.set("codec", codec_name(stream->codec));
    else st.set("codec", ev::null());
    st.set("width", static_cast<double>(stream ? stream->width : 0));
    st.set("height", static_cast<double>(stream ? stream->height : 0));
    st.set("bitrateKbps", static_cast<double>(stream ? stream->bitrate_kbps : g_config.bitrate_kbps));
    st.set("fps", static_cast<double>(g_config.fps));
    {
        ev::Persistent codecs(codecList(g_config.codecs));
        st.set("codecs", codecs.get());
    }
    const Server::Stats s = g_server->stats();
    ObjectBuilder stats;
    stats.set("submitted", static_cast<double>(s.submitted));
    stats.set("encoded", static_cast<double>(s.encoded));
    stats.set("keyframes", static_cast<double>(s.keyframes));
    stats.set("replaced", static_cast<double>(s.replaced));
    stats.set("unwatched", static_cast<double>(s.unwatched));
    stats.set("failed", static_cast<double>(s.failed));
    stats.set("streams", static_cast<double>(s.streams));
    stats.set("windowWaits", static_cast<double>(s.window_waits));
    st.set("stats", stats.get());
    return st.get();
}

// ---- events ----------------------------------------------------------------

bool knownEvent(const std::string& e) { return e == "attach" || e == "detach"; }

void dispatch(const std::string& event, size_t clients) {
    ObjectBuilder payload;
    payload.set("type", event);
    payload.set("clients", static_cast<double>(clients));

    std::vector<std::shared_ptr<ev::Persistent>> targets;
    for (const auto& l : g_listeners) {
        if (l.event == event) targets.push_back(l.fn);
    }
    for (const auto& fn : targets) {
        const Value arg = payload.get();
        ev::CallResult r = ev::call(fn->get(), ev::undefined(), std::span<const Value>(&arg, 1));
        (void)r;  // a listener that throws does not stop the others
    }
    if (g_namespace) {
        ev::Persistent handler(ev::getProperty(g_namespace->get(), "on" + event));
        if (ev::isFunction(handler.get())) {
            const Value arg = payload.get();
            ev::CallResult r = ev::call(handler.get(), g_namespace->get(), std::span<const Value>(&arg, 1));
            (void)r;
        }
    }
}

Value addListener(Value self, std::span<const Value> args) {
    if (args.size() < 2 || !ev::isString(args[0]) || !ev::isFunction(args[1])) {
        return ev::throwTypeError("bro.remote.on(event, fn): event is 'attach' or 'detach', fn a function");
    }
    const std::string event = ev::toUtf8(args[0]);
    if (!knownEvent(event)) return ev::throwTypeError("bro.remote.on: unknown event '" + event + "'");
    for (const auto& l : g_listeners) {
        if (l.event == event && l.fn->get() == args[1]) return self;
    }
    g_listeners.push_back({event, std::make_shared<ev::Persistent>(args[1])});
    return self;
}

Value removeListener(Value self, std::span<const Value> args) {
    if (args.size() < 2 || !ev::isString(args[0])) return self;
    const std::string event = ev::toUtf8(args[0]);
    std::erase_if(g_listeners, [&](const Listener& l) { return l.event == event && l.fn->get() == args[1]; });
    return self;
}

}  // namespace

void setHostHooks(HostHooks hooks) { g_hooks = std::move(hooks); }

Server* activeServer() { return g_server.get(); }

void installRemote() {
    g_listeners.clear();
    g_namespace.reset();

    ev::Persistent broP;
    {
        auto bro = ev::globalValue("bro");
        if (bro.found && ev::isObject(bro.value)) broP.set(bro.value);
    }
    if (!ev::isObject(broP.get())) {
        broP.set(ev::createObject());
        ev::registerGlobal("bro", broP.get());
        auto gt = ev::globalValue("globalThis");
        if (gt.found && ev::isObject(gt.value)) {
            ev::Persistent gtP(gt.value);
            ev::setProperty(gtP.get(), "bro", broP.get());
        }
    }

    ObjectBuilder remote;
    remote.set("available", true);

    // host(options) -> status. Hosting already with the same options is a
    // no-op; with others, the server is replaced.
    remote.def("host", 1, [](Value, std::span<const Value> args) -> Value {
        ServerConfig cfg = configFrom(args);
        if (g_server && sameConfig(cfg, g_config)) return statusObject();
        stopServer();
        std::string err;
        auto server = Server::create(cfg, &err);
        if (!server) return ev::throwError("bro.remote.host: " + err);
        g_server = std::move(server);
        g_config = cfg;
        g_lastClients = 0;
        if (g_hooks.serverChanged) g_hooks.serverChanged(g_server.get(), g_config);
        return statusObject();
    });

    // stop() -> whether a server was running.
    remote.def("stop", 0, [](Value, std::span<const Value>) -> Value {
        const bool was = g_server != nullptr;
        stopServer();
        return ev::fromBool(was);
    });

    remote.def("status", 0, [](Value, std::span<const Value>) -> Value { return statusObject(); });

    // codecs() -> what this machine can encode, in the library's order.
    remote.def("codecs", 0, [](Value, std::span<const Value>) -> Value { return codecList(available_encoders()); });

    remote.def("on", 2, addListener);
    remote.def("off", 2, removeListener);
    remote.def("addEventListener", 2, addListener);
    remote.def("removeEventListener", 2, removeListener);
    remote.set("onattach", ev::null());
    remote.set("ondetach", ev::null());

    g_namespace = std::make_unique<ev::Persistent>(remote.get());
    broP.set(ev::setProperty(broP.get(), "remote", remote.get()));
}

void tickRemote() {
    if (!g_server) return;
    const size_t now = g_server->client_count();
    if (now == g_lastClients) return;
    const bool attached = now > g_lastClients;
    g_lastClients = now;
    dispatch(attached ? "attach" : "detach", now);
}

void shutdownRemote() {
    stopServer();
    g_listeners.clear();
    g_namespace.reset();
}

}  // namespace broremote::api
