#pragma once
// A test viewer: a Client whose callbacks record everything, decode video
// with the codec of the latest StreamConfig, and (optionally) ack each frame.
// Plus helpers for CPU frames whose release is counted.

#include "broremote/client.h"
#include "broremote/server.h"
#include "check.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace testkit {

using namespace broremote;

struct Picture {
    uint64_t frame_id = 0;
    uint64_t stream_id = 0;
    bool keyframe = false;
    std::vector<uint8_t> pixels;  // RGBA, tightly packed
    uint32_t width = 0, height = 0;
};

class Viewer {
public:
    bool auto_ack = true;

    // Connect to `stream`; false (with the reason printed) on failure.
    bool open(std::unique_ptr<Stream> stream, ClientOptions opts = {}) {
        ClientHandlers h;
        h.on_config = [this](const StreamConfig& sc) {
            std::lock_guard<std::mutex> lk(m_);
            configs.push_back(sc);
            std::string err;
            decoder_ = create_decoder(sc.codec, &err);
            if (!decoder_) decode_errors.push_back(err);
        };
        h.on_video = [this](const VideoPacket& v) {
            Client* c = nullptr;
            {
                std::unique_lock<std::mutex> lk(m_);
                // The first packets can arrive before connect() has returned.
                cv_.wait_for(lk, std::chrono::seconds(5), [&] { return client_ != nullptr; });
                c = client_;
                packets.push_back(v.frame_id);
                Picture p;
                p.frame_id = v.frame_id;
                p.stream_id = v.stream_id;
                p.keyframe = v.keyframe;
                DecodedFrame out;
                std::string err;
                if (decoder_ && decoder_->decode(v.data, out, &err) && out.ready) {
                    p.width = out.width;
                    p.height = out.height;
                    p.pixels = std::move(out.data);
                } else {
                    decode_errors.push_back(err);
                }
                pictures.push_back(std::move(p));
            }
            if (auto_ack && c) c->ack(v.frame_id);
        };
        h.on_cursor = [this](const CursorState& cs) {
            std::lock_guard<std::mutex> lk(m_);
            cursors.push_back(cs);
        };
        h.on_error = [this](ErrorCode code, const std::string& msg) {
            std::lock_guard<std::mutex> lk(m_);
            errors.emplace_back(code, msg);
        };
        h.on_closed = [this](const std::string& why) {
            std::lock_guard<std::mutex> lk(m_);
            closed = true;
            closed_reason = why;
        };
        std::string err;
        owned_ = Client::connect(std::move(stream), std::move(h), opts, &err);
        if (!owned_) {
            connect_error = err;
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            client_ = owned_.get();
        }
        cv_.notify_all();
        return true;
    }

    Client& client() { return *owned_; }
    void close() { owned_.reset(); }

    // Snapshot accessors (callbacks run on the reader thread).
    template <class F>
    auto with(F f) {
        std::lock_guard<std::mutex> lk(m_);
        return f();
    }
    size_t picture_count() {
        return with([&] { return pictures.size(); });
    }
    Picture picture(size_t i) {
        return with([&] { return i < pictures.size() ? pictures[i] : Picture{}; });
    }
    Picture last_picture() {
        return with([&] { return pictures.empty() ? Picture{} : pictures.back(); });
    }
    size_t config_count() {
        return with([&] { return configs.size(); });
    }
    StreamConfig last_config() {
        return with([&] { return configs.empty() ? StreamConfig{} : configs.back(); });
    }
    bool is_closed() {
        return with([&] { return closed; });
    }
    bool has_error(ErrorCode code) {
        return with([&] {
            for (auto& e : errors) {
                if (e.first == code) return true;
            }
            return false;
        });
    }

    // Recorded state (guarded by m_; read through with()).
    std::vector<StreamConfig> configs;
    std::vector<uint64_t> packets;
    std::vector<Picture> pictures;
    std::vector<CursorState> cursors;
    std::vector<std::pair<ErrorCode, std::string>> errors;
    std::vector<std::string> decode_errors;
    bool closed = false;
    std::string closed_reason;
    std::string connect_error;

private:
    std::mutex m_;
    std::condition_variable cv_;
    Client* client_ = nullptr;
    std::unique_ptr<Decoder> decoder_;
    std::unique_ptr<Client> owned_;
};

// CPU frames whose release callbacks are counted: every submitted frame must
// be released exactly once, whatever path it takes.
class FrameSource {
public:
    struct Slot {
        std::vector<uint8_t> pixels;
        uint32_t width = 0, height = 0;
        std::atomic<int> releases{0};
    };

    // Make frame `seed` of the given size and submit it.
    Slot& submit(Server& server, uint32_t w, uint32_t h, uint32_t seed) {
        auto slot = std::make_unique<Slot>();
        slot->width = w;
        slot->height = h;
        slot->pixels.resize(size_t(w) * h * 4);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                uint8_t* p = &slot->pixels[(size_t(y) * w + x) * 4];
                p[0] = uint8_t(x + seed * 3);
                p[1] = uint8_t(y * 2 + seed);
                p[2] = uint8_t(seed * 17 + (x >> 3));
                p[3] = 255;
            }
        }
        Slot& s = *slot;
        {
            std::lock_guard<std::mutex> lk(m_);
            slots_.push_back(std::move(slot));
        }
        Frame f;
        f.width = w;
        f.height = h;
        f.cpu = s.pixels.data();
        f.cpu_stride = w * 4;
        f.pts_ns = int64_t(seed) * 1000;
        server.submit(f, [&s] { s.releases.fetch_add(1); });
        return s;
    }

    // Every frame released exactly once (call once the server is gone or idle).
    bool all_released_once() {
        std::lock_guard<std::mutex> lk(m_);
        bool ok = true;
        for (size_t i = 0; i < slots_.size(); ++i) {
            const int r = slots_[i]->releases.load();
            if (r != 1) {
                std::printf("   frame %zu released %d times\n", i, r);
                ok = false;
            }
        }
        return ok;
    }
    size_t count() {
        std::lock_guard<std::mutex> lk(m_);
        return slots_.size();
    }

private:
    std::mutex m_;
    std::deque<std::unique_ptr<Slot>> slots_;
};

}  // namespace testkit
