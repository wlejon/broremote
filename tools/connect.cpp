#include "connect.h"

#include <cstring>

namespace broremote::tools {

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
    if (!std::strcmp(a, "--no-input-lane")) {
        t.input_lane = false;
        return true;
    }
    if (i + 1 >= argc) return false;
    if (!std::strcmp(a, "--ssh")) {
        t.ssh_host = argv[++i];
    } else if (!std::strcmp(a, "--ssh-program")) {
        t.ssh_program = argv[++i];
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

}  // namespace broremote::tools
