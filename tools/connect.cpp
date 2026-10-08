#include "connect.h"

#include <cstring>
#include <vector>

namespace broremote::tools {

std::string ConnectTarget::describe() const { return remote() ? ssh_host : "socket " + socket; }

bool parse_connect_arg(int argc, char** argv, int& i, ConnectTarget& t) {
    const char* a = argv[i];
    if (!std::strcmp(a, "--ssh-pty")) {
        t.pty = true;
        return true;
    }
    if (!std::strcmp(a, "--ssh-no-pty")) {
        t.pty = false;
        return true;
    }
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
    std::string cmd = t.ssh_command;
    if (t.socket_given && !t.command_given) cmd += " --socket " + t.socket;
    if (t.pty) cmd += " --pty";
    return cmd;
}

std::unique_ptr<Stream> open_stream(const ConnectTarget& t, std::string* err) {
    if (!t.remote()) {
        bool not_running = false;
        auto s = connect_local(t.socket, err, &not_running);
        if (!s && not_running && err) *err = "no broremote server is running on socket '" + t.socket + "'";
        return s;
    }
    // BatchMode: ssh runs with no console to prompt on, so a password or
    // host-key question must fail at once, with its reason on stderr, rather
    // than hang.
    if (!t.pty) {
        // -T: no remote tty, so the pipe is binary as it is; but OpenSSH
        // then leaves Nagle on at both ends.
        const std::vector<std::string> argv = {"ssh", "-T", "-o", "BatchMode=yes", t.ssh_host, remote_command(t)};
        return spawn_stream(argv, err);
    }
    // -tt: a remote terminal, which is what makes OpenSSH set TCP_NODELAY
    // (and the low-delay IPQoS) on both ends; the proxy makes the terminal
    // raw and says so before any protocol byte. -e none: no escape
    // character. ObscureKeystrokeTiming=no: OpenSSH 9.5+ would otherwise
    // pace a terminal session's small writes into 20 ms chaff intervals.
    const std::vector<std::string> argv = {"ssh",       "-tt", "-e", "none", "-o", "BatchMode=yes", "-o",
                                           "ObscureKeystrokeTiming=no", t.ssh_host, remote_command(t)};
    return await_proxy_ready(spawn_stream(argv, err));
}

}  // namespace broremote::tools
