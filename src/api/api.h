#pragma once
// broremote's Bronze JavaScript API: `bro.remote`, the script side of hosting
// a broremote server. A script starts and stops the server and asks how it is
// doing; the host (bro) feeds it — frames, input, the cursor — through the
// hooks below, so this binding names nothing of the host.
//
//   bro.remote.host({socket, codecs, bitrateKbps, fps, name}) -> status
//   bro.remote.stop() -> bool
//   bro.remote.status() -> {hosting, socket, socketPath, clients, codec,
//                           width, height, bitrateKbps, fps, codecs, stats}
//   bro.remote.codecs() -> the codecs this machine can encode
//   bro.remote.on('attach' | 'detach', fn) / off / addEventListener /
//   removeEventListener, and onattach / ondetach
//
// bro's docs/remote-api.js is the annotated reference.
//
// Threads: everything here runs on the host thread that owns the realm (the
// engine thread in bro), as do the hooks.

#include "broremote/server.h"
#include "broremote/viewer.h"

#include "embed/embed.h"

#include <functional>

namespace broremote::api {

// How the binding reaches its host. Set before installRemote(); the host
// sets them once per process.
struct HostHooks {
    // A server started (`server` non-null, with the config it was made
    // with), or is about to be destroyed (`server` null). Called from
    // bro.remote.host() / stop() and from shutdownRemote(). Between the two
    // calls the host submits frames to the server, drains its input and sets
    // its cursor; after the null call it must not touch the server again
    // (frames it submitted are released by the server's destructor, which
    // runs right after the hook returns).
    std::function<void(Server* server, const ServerConfig& config)> serverChanged;
};
void setHostHooks(HostHooks hooks);

// Mounts bro.remote onto the realm's `bro` root (registering one when there
// is none). Once per realm. A running server outlives a realm (a page
// reload): the new realm sees it in status() and can stop it. Event
// listeners belong to the realm that added them, so an install drops them.
void installRemote();

// Host thread, once per frame: delivers `attach` / `detach` for the clients
// that came or went since the last tick. Cheap when nothing is hosted.
void tickRemote();

// Stops the server (the serverChanged(nullptr) hook first) and drops every
// listener. The host calls it before the realm or the host itself goes away.
void shutdownRemote();

// The running server, or null.
[[nodiscard]] Server* activeServer();

// ---- the viewer side: bro.remote.connect() -----------------------------------
//
//   bro.remote.connect({ssh, socket, sshCommand, sshProgram, pty, inputLane,
//                       name, audio, mic, playback, micMuted, playbackMuted,
//                       micDevice, speakerDevice, micTone, audioBufferMs})
//       -> session: status(), stats(), audio(), probe(), probes(),
//          sendInput(ev), setMicMuted(b), setPlaybackMuted(b), close(),
//          on('state' | 'config', fn) / off, onstate / onconfig
//
// The session decodes; the host shows its pictures and sends its input
// (bro: the <remoteview> element), reaching the session through
// viewerSession().

struct ViewerHooks {
    // A session is about to start: the host sets what its display needs on
    // the options (output memory, prepare, read_marker) before start().
    std::function<void(ViewerSession* session, ViewerOptions& options)> sessionStarting;
    // Just before a session is destroyed (close(), a reload, shutdownRemote):
    // the host lets go of it; nothing may reach it after this returns.
    std::function<void(ViewerSession* session)> sessionGone;
    // Given to every session as its wake: any thread, when a picture, the
    // cursor or the status changed.
    std::function<void()> wake;
};
void setViewerHooks(ViewerHooks hooks);

// The session a bro.remote.connect() object stands for (by identity, so a
// look-alike object names nothing), or null.
[[nodiscard]] ViewerSession* viewerSession(bronze::Value sessionObject);

}  // namespace broremote::api
