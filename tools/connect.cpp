#include "connect.h"

#include <cstring>
#include <vector>

namespace broremote::tools {

std::string ConnectTarget::describe() const { return remote() ? ssh_host : "socket " + socket; }

bool parse_connect_arg(int argc, char** argv, int& i, ConnectTarget& t) {
    const char* a = argv[i];
    if (i + 1 >= argc) return false;
    if (!std::strcmp(a, "--ssh")) {
        t.ssh_host = argv[++i];
    } else if (!std::strcmp(a, "--ssh-command")) {
        t.ssh_command = argv[++i];
        t.command_given = true;
    } else if (!std::strcmp(a, "--socket")) {
        t.socket = argv[++i];
        t.socket_given = true;
    } else {
        return false;
    }
    return true;
}

std::string remote_command(const ConnectTarget& t) {
    if (t.socket_given && !t.command_given) return t.ssh_command + " --socket " + t.socket;
    return t.ssh_command;
}

std::unique_ptr<Stream> open_stream(const ConnectTarget& t, std::string* err) {
    if (!t.remote()) {
        bool not_running = false;
        auto s = connect_local(t.socket, err, &not_running);
        if (!s && not_running && err) *err = "no broremote server is running on socket '" + t.socket + "'";
        return s;
    }
    // -T: no remote tty (the stream is binary). BatchMode: ssh runs with no
    // console to prompt on, so a password or host-key question must fail
    // at once, with its reason on stderr, rather than hang.
    const std::vector<std::string> argv = {"ssh", "-T", "-o", "BatchMode=yes", t.ssh_host, remote_command(t)};
    return spawn_stream(argv, err);
}

}  // namespace broremote::tools
