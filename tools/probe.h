#pragma once
// `broremote probe`: the library's ViewerSession with no window, for the
// scripted end-to-end checks (pictures and the test pattern, the timing
// breakdown, key press to picture latency, the audio lane).

namespace broremote::tools {

// argv: the arguments after `probe`. `usage` prints the CLI's usage and
// returns its exit code. Returns the exit code.
int cmd_probe(int argc, char** argv, int (*usage)());

}  // namespace broremote::tools
