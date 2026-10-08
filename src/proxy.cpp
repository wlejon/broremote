// `broremote proxy`: the remote end of `ssh host broremote proxy`. A byte
// relay (brolink's): the protocol runs end to end between the viewer and the
// server, so the proxy reads none of it. Also the viewer's side of the --pty
// handshake.
#include "broremote/stream.h"

#include <brolink/proxy.h>

namespace broremote {

std::unique_ptr<Stream> await_proxy_ready(std::unique_ptr<Stream> inner) {
    return brolink::await_ready(std::move(inner), std::string(kProxyReady));
}

int run_proxy(std::string_view name, std::string* err, bool pty) {
    brolink::ProxyOptions po;
    po.pty = pty;
    po.name = "broremote proxy";
    po.ready_marker = std::string(kProxyReady);
    const std::string n(name);
    return brolink::run_proxy([&](std::string* e) { return connect_local(n, e); }, po, err);
}

}  // namespace broremote
