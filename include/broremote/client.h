#pragma once
// The viewer side: speaks the protocol over any Stream (a local socket from
// connect_local, or the stdio of `ssh host broremote proxy` from
// spawn_stream), delivers what the server sends through callbacks, and sends
// acks, keyframe requests and input back.

#include "broremote/protocol.h"
#include "broremote/stream.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace broremote {

struct ClientOptions {
    std::string name = "broremote";
    // Sent as SetCodec right after the handshake when not empty: the codecs
    // this viewer can decode, in preference order. Empty: any (the server
    // picks from its own list).
    std::vector<Codec> codecs;
    uint32_t max_bitrate_kbps = 0;
    // How long connect() waits for Welcome before giving up.
    uint32_t connect_timeout_ms = 10000;
};

// Callbacks run on the client's reader thread, in message order. They are
// fixed at connect(), so none can be missed between connecting and setting
// them. Any may be empty.
struct ClientHandlers {
    std::function<void(const StreamConfig&)> on_config;
    std::function<void(const VideoPacket&)> on_video;
    std::function<void(const CursorState&)> on_cursor;
    // Every Error the server sends (the fatal ones are followed by on_closed).
    std::function<void(ErrorCode, const std::string&)> on_error;
    // Once, when the connection ends for any reason, including the Client's
    // destruction, with what ended it.
    std::function<void(const std::string&)> on_closed;
    // 1.1: the answer to ping(): when it was sent and received (this side's
    // steady clock) and the server's clock when it answered.
    std::function<void(std::chrono::steady_clock::time_point sent, std::chrono::steady_clock::time_point received,
                       uint64_t server_time_us)>
        on_pong;
    // 1.1: a Video message was handed whole to the server's socket.
    std::function<void(const FrameSentMsg&)> on_frame_sent;
};

class Client {
public:
    // Sends Hello and waits for Welcome. Null (with *err) on a refusal (the
    // server's Error message), a timeout or a broken stream.
    static std::unique_ptr<Client> connect(std::unique_ptr<Stream> stream, ClientHandlers handlers,
                                           const ClientOptions& options, std::string* err);
    static std::unique_ptr<Client> connect(std::unique_ptr<Stream> stream, ClientHandlers handlers,
                                           std::string* err) {
        return connect(std::move(stream), std::move(handlers), ClientOptions{}, err);
    }
    // Closes the stream and joins the reader thread (on_closed runs first).
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    [[nodiscard]] const WelcomeMsg& welcome() const;

    // Any thread. The viewer acks the highest frame id it has finished with:
    // decoded, or given up on after a decode error. The server stops encoding
    // while a client has max_frames_in_flight unacked frames, so a viewer
    // that stops acking stops the stream.
    void ack(uint64_t frame_id);
    // The decoder lost sync: the next frame will be a keyframe.
    void request_keyframe();
    void send_input(const InputEvent& event);
    void set_codecs(const std::vector<Codec>& codecs, uint32_t max_bitrate_kbps = 0);
    // Measures the transport's round trip (on_pong). False, sending nothing,
    // when the server speaks 1.0 (it would answer Error(UnknownMessage)).
    bool ping();

    // False once the connection has ended.
    [[nodiscard]] bool connected() const;
    // Ends the connection (on_closed runs on the reader thread). Idempotent.
    void close();

    struct Impl;

private:
    explicit Client(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace broremote
