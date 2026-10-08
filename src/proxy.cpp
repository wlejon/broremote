// `broremote proxy`: the remote end of `ssh host broremote proxy`. A byte
// relay: the protocol runs end to end between the viewer and the server, so
// the proxy reads none of it.
#include "broremote/stream.h"

#include <memory>
#include <thread>

namespace broremote {

int run_proxy(std::string_view name, std::string* err) {
    std::shared_ptr<Stream> server(connect_local(name, err));
    if (!server) return 1;
    std::shared_ptr<Stream> io(stdio_stream());

    // stdin -> server on its own thread (detached: a blocked stdin read must
    // not hold up exit once the server side has gone).
    std::thread([server, io] {
        std::unique_ptr<char[]> buf(new char[64u << 10]);
        for (;;) {
            size_t n = io->read(buf.get(), 64u << 10);
            if (n == 0 || !server->write(std::string_view(buf.get(), n))) break;
        }
        server->shutdown();  // ends the other direction too
    }).detach();

    // server -> stdout here.
    std::unique_ptr<char[]> buf(new char[256u << 10]);
    for (;;) {
        size_t n = server->read(buf.get(), 256u << 10);
        if (n == 0 || !io->write(std::string_view(buf.get(), n))) break;
    }
    server->shutdown();
    return 0;
}

}  // namespace broremote
