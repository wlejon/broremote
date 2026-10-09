#pragma once
// Where a viewer connects: a local server socket, or a remote one through
// `ssh HOST <command>` (the command defaults to `broremote proxy`). Each
// connection of a session (the control connection, the input lane, the audio
// lane) is opened the same way, so over ssh each is an ssh of its own.

#include "broremote/stream.h"

#include <functional>
#include <memory>
#include <string>

namespace broremote {

struct ConnectTarget {
    std::string ssh_host;                         // empty: a local socket
    std::string ssh_command = "broremote proxy";  // run on the host by ssh
    std::string socket = "default";               // the server's socket name
    bool socket_given = false;                    // the socket was named (it goes to the remote proxy)
    bool command_given = false;                   // the command was given (it is used as it is)
    std::string ssh_program;                      // empty: ssh_program() picks
    // ssh -tt and `proxy --pty` (OpenSSH sets TCP_NODELAY only on a terminal
    // session), or -T and a plain pipe.
    bool pty = false;
    // Input on a lane of its own (protocol 1.2): a second connection opened
    // the same way as the first (a second ssh with ssh_host). Off: input
    // goes on the control connection.
    bool input_lane = true;

    [[nodiscard]] bool remote() const { return !ssh_host.empty(); }
    // "halo" or "socket default", for titles and messages.
    [[nodiscard]] std::string describe() const;
};

// The ssh to run: t.ssh_program, else $BROREMOTE_SSH, else on Windows the
// system's own OpenSSH (%SystemRoot%\System32\OpenSSH\ssh.exe) when it is
// there, else `ssh` from PATH. On Windows the PATH's ssh is often Git's
// MSYS build, whose emulated select() on pipes holds each small write from
// the viewer for up to a timer tick: measured, a 5 ms mean and 15 ms worst
// round trip against 0.7 / 1.5 ms for the system's.
std::string ssh_program(const ConnectTarget& t);

// The command ssh runs: with a named socket (and no command of its own) the
// socket name goes to the remote proxy; --pty for a terminal session.
std::string remote_command(const ConnectTarget& t);

// The stream: ssh's stdio, or the local socket. Null with *err on failure.
std::unique_ptr<Stream> open_stream(const ConnectTarget& t, std::string* err);

// ClientOptions::open_input_lane for `t`: another open_stream(t), or empty
// when t.input_lane is off.
std::function<std::unique_ptr<Stream>(std::string*)> input_lane_opener(const ConnectTarget& t);

}  // namespace broremote
