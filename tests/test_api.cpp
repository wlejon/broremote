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
    // Audio: on by default, nobody on an audio lane yet.
    CHECK_EQ(run("const u = bro.remote.status().audio; [u.enabled, u.micAsDefault, u.viewers.length, "
                 "bro.remote.status().stats.audioLanes].join()"),
             std::string("true,false,0,0"));

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
    run("bro.remote.host({socket: '" + sock + "', codecs: 'raw', fps: 60, audio: false, micAsDefault: true})");
    CHECK_EQ(g_log.calls.size(), size_t(3));  // stopped, started again
    CHECK_EQ(run("bro.remote.status().fps"), std::string("60"));
    CHECK_EQ(run("const w = bro.remote.status().audio; [w.enabled, w.micAsDefault].join()"),
             std::string("false,true"));
    CHECK_EQ(run("(() => { try { bro.remote.host({socket: '" + sock +
                 "', audio: 'yes'}); return 'no' } catch (e) { return e instanceof TypeError ? 'ok' : 'x' } })()"),
             std::string("ok"));
    CHECK_EQ(g_log.calls.size(), size_t(3));  // a bad option changes nothing
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

    check::phase("connect: option errors");
    CHECK_EQ(run("typeof bro.remote.connect"), std::string("function"));
    CHECK_EQ(run("(() => { try { bro.remote.connect({socket: '../x'}); return 'no' } catch (e) { "
                 "return e instanceof TypeError && e.message.startsWith('bro.remote.connect:') ? 'ok' : e.message } })()"),
             std::string("ok"));
    CHECK_EQ(run("(() => { try { bro.remote.connect({audio: 1}); return 'no' } catch (e) { "
                 "return e instanceof TypeError ? 'ok' : e.message } })()"),
             std::string("ok"));

    check::phase("connect: a session to an in-process server");
    struct ViewerLog {
        std::vector<ViewerSession*> starting, gone;
        brovideo::PictureMemory output = brovideo::PictureMemory::D3D11;
        std::atomic<int> prepared{0};
    };
    static ViewerLog vlog;
    {
        api::ViewerHooks vh;
        vh.sessionStarting = [](ViewerSession* s, ViewerOptions& o) {
            vlog.starting.push_back(s);
            o.output = vlog.output;  // CPU pictures (Raw has no other kind)
            o.prepare = [](DecodedFrame&) { ++vlog.prepared; };
        };
        vh.sessionGone = [](ViewerSession* s) { vlog.gone.push_back(s); };
        api::setViewerHooks(vh);
    }
    vlog.output = brovideo::PictureMemory::Cpu;
    const std::string vsock = unique_socket();
    std::unique_ptr<Server> vserver;
    {
        ServerConfig cfg;
        cfg.socket_name = vsock;
        cfg.codecs = {Codec::Raw};
        cfg.audio.enabled = false;
        std::string err;
        vserver = Server::create(cfg, &err);
        CHECK(vserver != nullptr);
    }
    run("globalThis.vs = bro.remote.connect({socket: '" + vsock + "', audio: false, name: 'api-test'});"
        "globalThis.vev = []; vs.on('state', e => vev.push(e.state)); vs.onconfig = e => vev.push('config:' + "
        "e.width + 'x' + e.height);");
    CHECK_EQ(vlog.starting.size(), size_t(1));
    CHECK_EQ(run("vs.target"), "socket " + vsock);
    WAIT((api::tickRemote(), run("vs.status().state") == "connected"), 5000);
    WAIT(vserver->client_count() == 1, 5000);
    {
        ev::CallResult r = bronze::eval::evalScript("vs");
        CHECK(!r.thrown && api::viewerSession(r.value) == vlog.starting[0]);
        ev::CallResult other = bronze::eval::evalScript("({id: vs.id})");
        CHECK(!other.thrown && api::viewerSession(other.value) == nullptr);
    }
    std::vector<uint8_t> vpixels(64 * 32 * 4, 0x40);
    for (int i = 0; i < 3; ++i) {
        Frame f;
        f.width = 64;
        f.height = 32;
        f.cpu = vpixels.data();
        vserver->submit(f, [] {});
        WAIT(run("vs.stats().decoded >= " + std::to_string(i + 1)) == "true", 5000);
    }
    WAIT((api::tickRemote(), run("vev.join()") == "connected,config:64x32"), 5000);
    CHECK_EQ(run("vev.join()"), std::string("connected,config:64x32"));
    CHECK_EQ(run("const vt = vs.status(); [vt.codec, vt.width, vt.height, vt.server, vt.protocol, vt.inputLane].join()"),
             std::string("raw,64,32,broremote,1.4,true"));
    CHECK(vlog.prepared.load() >= 3);
    run("vs.sendInput({kind: 'key', code: 30, pressed: true}); vs.sendInput({kind: 'relative', x: 2, y: -1});");
    {
        std::vector<InputEvent> got;
        WAIT((vserver->drain_input(got), got.size() >= 2), 5000);
        CHECK(got.size() >= 2 && got[0] == InputEvent::key(30, true) && got[1] == InputEvent::relative(2, -1));
    }
    CHECK_EQ(run("(() => { try { vs.sendInput({kind: 'jump'}); return 'no' } catch (e) { return e instanceof "
                 "TypeError ? 'ok' : 'x' } })()"),
             std::string("ok"));
    CHECK_EQ(run("vs.audio()"), std::string("?"));  // null: no audio lane
    {
        CursorState c;
        c.x = 5;
        c.y = 6;
        c.shape = "text";
        c.locked = true;
        vserver->set_cursor(c);
    }
    WAIT(run("(vs.status().cursor || {}).shape") == "text", 5000);
    CHECK_EQ(run("const vc = vs.status().cursor; [vc.x, vc.y, vc.locked].join()"), std::string("5,6,true"));

    check::phase("connect: the server goes, close");
    vserver.reset();
    WAIT((api::tickRemote(), run("vev.join()") == "connected,config:64x32,closed"), 5000);
    CHECK_EQ(run("vs.close()"), std::string("true"));
    CHECK_EQ(vlog.gone.size(), size_t(1));
    CHECK_EQ(run("vs.close()"), std::string("false"));
    CHECK_EQ(run("vs.status().state"), std::string("closed"));

    check::phase("connect: nothing listening, and shutdown closes sessions");
    run("globalThis.vn = bro.remote.connect({socket: '" + unique_socket() + "', audio: false});");
    WAIT((api::tickRemote(), run("vn.status().state") == "closed"), 5000);
    CHECK_EQ(run("vn.status().failed"), std::string("true"));
    run("globalThis.vk = bro.remote.connect({socket: '" + unique_socket() + "', audio: false});");

    check::phase("shutdownRemote");
    run("bro.remote.host({socket: '" + sock + "', codecs: ['raw']})");
    CHECK(api::activeServer() != nullptr);
    api::shutdownRemote();
    CHECK(api::activeServer() == nullptr);
    CHECK_EQ(g_log.calls.size(), size_t(6));
    CHECK(!g_log.calls.back());
    CHECK_EQ(vlog.gone.size(), size_t(3));  // vs (closed above), vn and vk

    return check::finish();
}
