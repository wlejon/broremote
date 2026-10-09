#include "broremote/connect.h"

#include <cstdlib>
#include <filesystem>
#include <vector>

namespace broremote {

std::string ConnectTarget::describe() const { return remote() ? ssh_host : "socket " + socket; }

std::string ssh_program(const ConnectTarget& t) {
    if (!t.ssh_program.empty()) return t.ssh_program;
    if (const char* e = std::getenv("BROREMOTE_SSH"); e && *e) return e;
#if defined(_WIN32)
    if (const char* root = std::getenv("SystemRoot"); root && *root) {
        const std::filesystem::path p = std::filesystem::path(root) / "System32" / "OpenSSH" / "ssh.exe";
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return p.string();
    }
#endif
    return "ssh";
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
        const std::vector<std::string> argv = {ssh_program(t), "-T", "-o", "BatchMode=yes", t.ssh_host,
                                               remote_command(t)};
        return spawn_stream(argv, err);
    }
    // -tt: a remote terminal, which is what makes OpenSSH set TCP_NODELAY
    // (and the low-delay IPQoS) on both ends; the proxy makes the terminal
    // raw and says so before any protocol byte. -e none: no escape
    // character. ObscureKeystrokeTiming=no: OpenSSH 9.5+ would otherwise
    // pace a terminal session's small writes into 20 ms chaff intervals.
    const std::vector<std::string> argv = {ssh_program(t), "-tt", "-e", "none", "-o", "BatchMode=yes", "-o",
                                           "ObscureKeystrokeTiming=no", t.ssh_host, remote_command(t)};
    return await_proxy_ready(spawn_stream(argv, err));
}

std::function<std::unique_ptr<Stream>(std::string*)> input_lane_opener(const ConnectTarget& t) {
    if (!t.input_lane) return {};
    return [t](std::string* err) { return open_stream(t, err); };
}

}  // namespace broremote
