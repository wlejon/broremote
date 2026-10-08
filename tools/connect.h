#pragma once
// Where a viewer-side tool connects: a local server socket, or a remote one
// through `ssh -T HOST <command>` (the command defaults to `broremote
// proxy`). Shared by `broremote record` and `broremote-view`.

#include "broremote/stream.h"

#include <memory>
#include <string>

namespace broremote::tools {

struct ConnectTarget {
    std::string ssh_host;                         // empty: a local socket
    std::string ssh_command = "broremote proxy";  // run on the host by ssh
    std::string socket = "default";               // the local socket's name
    bool socket_given = false;                    // --socket was passed
    bool command_given = false;                   // --ssh-command was passed

    [[nodiscard]] bool remote() const { return !ssh_host.empty(); }
    // "halo" or "socket default", for titles and messages.
    [[nodiscard]] std::string describe() const;
};

// Consumes --ssh HOST, --ssh-command CMD or --socket NAME at argv[i]
// (advancing i past the value). False when argv[i] is none of them.
bool parse_connect_arg(int argc, char** argv, int& i, ConnectTarget& t);

// With --ssh and --socket together, the socket name goes to the remote
// proxy (unless --ssh-command was given, which is used as it is).
std::string remote_command(const ConnectTarget& t);

// The stream: ssh's stdio, or the local socket. Null with *err on failure.
std::unique_ptr<Stream> open_stream(const ConnectTarget& t, std::string* err);

}  // namespace broremote::tools
