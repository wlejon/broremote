#pragma once
// The command-line half of where a viewer-side tool connects (the target
// itself is the library's, broremote/connect.h). Shared by `broremote
// record` and `broremote probe`.

#include "broremote/connect.h"

namespace broremote::tools {

using broremote::ConnectTarget;
using broremote::input_lane_opener;
using broremote::open_stream;
using broremote::remote_command;
using broremote::ssh_program;

// Consumes --ssh HOST, --ssh-command CMD, --socket NAME, --ssh-program P,
// --ssh-pty, --ssh-no-pty or --no-input-lane at argv[i]
// (advancing i past the value). False when argv[i] is none of them.
bool parse_connect_arg(int argc, char** argv, int& i, ConnectTarget& t);

}  // namespace broremote::tools
