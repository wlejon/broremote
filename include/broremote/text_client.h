#pragma once
// The client side of text (protocol 1.6): command lines to the host, each
// answered by a reply, several in flight at once.
//
// Two ways in, one protocol:
//   open()  a session of its own: Hello with NoVideo. The server sends it no
//           video and does not count it as a viewer, so an agent alone runs
//           no encoder. Its requests go on this one connection.
//   join()  the text lane of an existing session (a viewer's), with the
//           grant from that session's Welcome: text never queues behind the
//           viewer's video.
//
// What a request means is the host's (bro: its agent control commands); this
// only carries them.

#include "broremote/protocol.h"
#include "broremote/stream.h"

#include <brolink/lanes.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace broremote {

struct TextClientOptions {
    std::string name = "broremote text";  // the session's name in Hello (open only)
    uint32_t timeout_ms = 10000;          // for Welcome / Joined
};

struct TextReply {
    bool ok = false;
    std::string payload;  // the reply's text, or why it failed
    std::string file;     // the out_arg file's bytes (empty when none)
};

class TextClient {
public:
    using Callback = std::function<void(TextReply)>;

    // Null with *err on a refusal, a timeout, a broken stream, or a server
    // older than 1.6.
    static std::unique_ptr<TextClient> open(std::unique_ptr<Stream> stream, const TextClientOptions& options,
                                            std::string* err);
    static std::unique_ptr<TextClient> join(std::unique_ptr<Stream> stream, const brolink::lanes::Grant& grant,
                                            const TextClientOptions& options, std::string* err);
    // Closes the stream and joins the reader; requests still open are
    // answered (not ok, "closed") first.
    ~TextClient();
    TextClient(const TextClient&) = delete;
    TextClient& operator=(const TextClient&) = delete;

    // Any thread. `done` runs once, on the reader thread when the reply
    // comes, or at once (on this thread) when the connection is gone.
    // out_arg: 0, or the index (>= 1) of an argument naming an output file
    // whose bytes the host returns in TextReply::file.
    void request(std::vector<std::string> argv, size_t out_arg, Callback done);
    // Blocks for the reply (or until the connection ends).
    TextReply request_sync(std::vector<std::string> argv, size_t out_arg = 0);

    // open() only: the server's Welcome.
    [[nodiscard]] const WelcomeMsg& welcome() const;
    [[nodiscard]] bool connected() const;
    // Why the connection ended (empty while connected).
    [[nodiscard]] std::string closed_reason() const;
    // Called once (on the reader thread) when the connection ends. Set it
    // right after open() / join(); an end before then is in closed_reason().
    void on_closed(std::function<void(const std::string&)> f);
    void close();

    struct Impl;

private:
    explicit TextClient(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

}  // namespace broremote
