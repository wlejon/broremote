// broremote_api, the way bro uses it: a bronze realm, setHostHooks and
// installRemote, then bro.remote driven from JavaScript — host, status,
// codecs, the attach / detach events as a real Client comes and goes, stop,
// option errors, and shutdownRemote. The test stands in for the host: its
// hook keeps the server, and it submits a CPU frame the way bro's adapter
// does, so status() can report the stream.
#include "check.h"

#include "broremote/api.h"
#include "broremote/client.h"
#include "broremote/stream.h"

#include "embed/embed.h"
#include "eval/eval.h"

#include <atomic>
#include <random>
#include <string>
#include <vector>

using namespace broremote;
namespace ev = bronze::embed;

namespace {

// Runs `source`; returns its value as a string ("" and a failure when it threw).
std::string run(const std::string& source) {
    auto r = bronze::eval::evalScript(source);
    if (r.thrown) {
        std::string why = ev::isString(r.value) ? ev::toUtf8(r.value) : std::string("(non-string throw)");
        check::fail(__FILE__, __LINE__, "script threw: " + why + "\n     in: " + source);
        return "";
    }
    if (ev::isString(r.value)) return ev::toUtf8(r.value);
    if (ev::isBool(r.value)) return ev::toBool(r.value) ? "true" : "false";
    if (ev::isNumber(r.value)) return std::to_string(static_cast<long long>(ev::toDouble(r.value)));
    return ev::isUndefined(r.value) ? "undefined" : "?";
}

// What the host saw through serverChanged.
struct HookLog {
    std::vector<bool> calls;  // true: a server started; false: one is going
    Server* server = nullptr;
};
HookLog g_log;

std::string unique_socket() {
    static std::random_device rd;
    return "api-" + std::to_string(rd() % 1000000);
}

}  // namespace

int main() {
    check::watchdog(120);

    check::phase("install");
    api::HostHooks hooks;
    hooks.serverChanged = [](Server* s, const ServerConfig&) {
        g_log.calls.push_back(s != nullptr);
        g_log.server = s;
    };
    api::setHostHooks(hooks);
    api::installRemote();
    CHECK_EQ(run("typeof bro.remote"), std::string("object"));
    CHECK_EQ(run("bro.remote.available"), std::string("true"));
    for (const char* fn : {"host", "stop", "status", "codecs", "on", "off", "addEventListener",
                           "removeEventListener"}) {
        CHECK_EQ(run(std::string("typeof bro.remote.") + fn), std::string("function"));
    }

    check::phase("status and codecs before hosting");
    CHECK_EQ(run("JSON.stringify(bro.remote.status())"),
             std::string(R"({"hosting":false,"socket":null,"socketPath":null,"clients":0,"codec":null,"width":0,"height":0})"));
    CHECK_EQ(run("bro.remote.codecs().includes('raw')"), std::string("true"));
    CHECK_EQ(run("bro.remote.stop()"), std::string("false"));
    CHECK(g_log.calls.empty());

    check::phase("option errors");
    // (A script's completion value is its last expression statement, so each
    // try sits in a function that returns what it found.)
    CHECK_EQ(run("(() => { try { bro.remote.host({codecs: ['vp9']}); return 'no' } catch (e) { return e instanceof "
                 "TypeError && e.message.includes('vp9') ? 'ok' : e.message } })()"),
             std::string("ok"));
    CHECK_EQ(run("(() => { try { bro.remote.host({fps: 0}); return 'no' } catch (e) { return e instanceof TypeError "
                 "? 'ok' : e.message } })()"),
             std::string("ok"));
    CHECK_EQ(run("(() => { try { bro.remote.host({socket: '../x', codecs: 'raw'}); return 'no' } catch (e) { "
                 "return e.message } })()")
                 .find("bro.remote.host:") == 0,
             true);
    CHECK_EQ(run("bro.remote.status().hosting"), std::string("false"));
    CHECK(g_log.calls.empty());

    check::phase("host");
    const std::string sock = unique_socket();
    const std::string opts = "{socket: '" + sock + "', codecs: ['raw'], fps: 30, bitrateKbps: 5000}";
    CHECK_EQ(run("const s = bro.remote.host(" + opts + "); [s.hosting, s.socket, s.clients, s.codec, s.fps, "
                 "s.codecs.join()].join()"),
             "true," + sock + ",0,,30,raw");
    CHECK_EQ(g_log.calls.size(), size_t(1));
    CHECK(g_log.server != nullptr);
    CHECK(api::activeServer() == g_log.server);
    // The same options again: the running server is kept.
    run("bro.remote.host(" + opts + ")");
    CHECK_EQ(g_log.calls.size(), size_t(1));

    check::phase("attach event, stream status");
    run("globalThis.seen = []; bro.remote.on('attach', e => seen.push(e.type + ':' + e.clients));"
        "bro.remote.ondetach = e => seen.push('on' + e.type + ':' + e.clients);");
    std::atomic<int> videos{0};
    std::unique_ptr<Client> client;
    {
        ClientHandlers h;
        h.on_video = [&](const VideoPacket&) { ++videos; };
        std::string err;
        client = Client::connect(connect_local(sock, &err), std::move(h), &err);
        CHECK(client != nullptr);
    }
    WAIT((api::tickRemote(), run("seen.join()") == "attach:1"), 5000);
    CHECK_EQ(run("bro.remote.status().clients"), std::string("1"));
    // The host's part: a CPU frame to the server it was handed.
    std::vector<uint8_t> pixels(32 * 16 * 4, 0x80);
    std::atomic<int> released{0};
    if (g_log.server) {
        Frame f;
        f.width = 32;
        f.height = 16;
        f.cpu = pixels.data();
        g_log.server->submit(f, [&] { ++released; });
    }
    WAIT(videos.load() >= 1, 5000);
    CHECK_EQ(released.load(), 1);
    CHECK_EQ(run("const t = bro.remote.status(); [t.codec, t.width, t.height, t.bitrateKbps, t.stats.encoded, "
                 "t.stats.keyframes].join()"),
             std::string("raw,32,16,5000,1,1"));

    check::phase("detach event");
    client.reset();
    WAIT((api::tickRemote(), run("seen.join()") == "attach:1,ondetach:0"), 5000);

    check::phase("listeners off");
    run("const f = () => seen.push('x'); bro.remote.addEventListener('attach', f); "
        "bro.remote.removeEventListener('attach', f); seen.length = 0;"
        "bro.remote.ondetach = null;");
    CHECK_EQ(run("(() => { try { bro.remote.on('frame', () => {}); return 'no' } catch (e) { return e instanceof "
                 "TypeError ? 'ok' : 'x' } })()"),
             std::string("ok"));

    check::phase("replace and stop");
    run("bro.remote.host({socket: '" + sock + "', codecs: 'raw', fps: 60})");
    CHECK_EQ(g_log.calls.size(), size_t(3));  // stopped, started again
    CHECK_EQ(run("bro.remote.status().fps"), std::string("60"));
    CHECK_EQ(run("bro.remote.stop()"), std::string("true"));
    CHECK_EQ(g_log.calls.size(), size_t(4));
    CHECK(g_log.server == nullptr);
    CHECK(api::activeServer() == nullptr);
    CHECK_EQ(run("bro.remote.status().hosting"), std::string("false"));

    check::phase("GC stress");
    CHECK_EQ(run("let n = 0; for (let i = 0; i < 300; ++i) { const s = bro.remote.status(); n += s.clients;"
                 " const c = bro.remote.codecs(); const f = () => {}; bro.remote.on('detach', f);"
                 " bro.remote.off('detach', f); } n"),
             std::string("0"));

    check::phase("shutdownRemote");
    run("bro.remote.host({socket: '" + sock + "', codecs: ['raw']})");
    CHECK(api::activeServer() != nullptr);
    api::shutdownRemote();
    CHECK(api::activeServer() == nullptr);
    CHECK_EQ(g_log.calls.size(), size_t(6));
    CHECK(!g_log.calls.back());

    return check::finish();
}
