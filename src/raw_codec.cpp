// Codec::Raw: CPU RGBA frames, run-length coded, with XOR-delta predicted
// frames between keyframes. It exists so the protocol, the server and the
// client run and are tested everywhere; it is not for real use. The format
// (docs/protocol.md, "The Raw codec"):
//
//   u8 kind            0 = intra, 1 = delta (XOR with the previous picture)
//   varint width, varint height
//   tokens until width * height pixels are covered:
//     varint t, n = (t >> 1) + 1
//     t & 1: a run, one 4-byte pixel repeated n times
//     else:  a literal, n pixels of 4 bytes
//
// A pixel is 4 bytes R G B A; in a delta frame it is XORed with the same
// pixel of the previous picture, so unchanged areas are runs of zero.
#include "codec_backends.h"
#include "broremote/protocol.h"
#include "broremote/wire.h"

#include <cstring>

namespace broremote::detail {

namespace {

constexpr uint8_t kIntra = 0;
constexpr uint8_t kDelta = 1;
// Largest picture the decoder allocates (a 1 GiB picture from a few bytes of
// hostile input is not something a test codec needs to allow).
constexpr uint64_t kMaxPixels = uint64_t(8192) * 8192;
constexpr size_t kMinRun = 3;

inline uint32_t px(const uint8_t* p, size_t i) {
    uint32_t v;
    std::memcpy(&v, p + i * 4, 4);
    return v;
}

void put_varint(std::vector<uint8_t>& out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back(uint8_t(v) | 0x80);
        v >>= 7;
    }
    out.push_back(uint8_t(v));
}

void put_literal(std::vector<uint8_t>& out, const uint8_t* pixels, size_t from, size_t to) {
    if (from == to) return;
    put_varint(out, uint64_t(to - from - 1) << 1);
    out.insert(out.end(), pixels + from * 4, pixels + to * 4);
}

// Run-length code `n` pixels.
void rle(const uint8_t* pixels, size_t n, std::vector<uint8_t>& out) {
    size_t lit = 0;
    size_t i = 0;
    while (i < n) {
        const uint32_t v = px(pixels, i);
        size_t r = 1;
        while (i + r < n && px(pixels, i + r) == v) ++r;
        if (r >= kMinRun) {
            put_literal(out, pixels, lit, i);
            put_varint(out, (uint64_t(r - 1) << 1) | 1);
            out.insert(out.end(), pixels + i * 4, pixels + i * 4 + 4);
            i += r;
            lit = i;
        } else {
            i += r;
        }
    }
    put_literal(out, pixels, lit, n);
}

class RawEncoder final : public Encoder {
public:
    explicit RawEncoder(const EncoderConfig& c) : config_(c) {}

    bool encode(const Frame& frame, bool force_keyframe, const std::function<void()>& release, EncodedPacket& out,
                std::string* err) override {
        if (!frame.is_cpu() || !frame.cpu) {
            release();
            if (err) *err = "the raw codec takes CPU frames only";
            return false;
        }
        if (frame.width != config_.width || frame.height != config_.height) {
            release();
            if (err) *err = "frame size differs from the encoder's";
            return false;
        }
        const size_t w = frame.width, h = frame.height, row = w * 4;
        cur_.resize(row * h);
        for (size_t y = 0; y < h; ++y) std::memcpy(cur_.data() + y * row, frame.cpu + y * frame.cpu_row_bytes(), row);
        release();  // everything below reads our copy

        const bool intra = force_keyframe || prev_.size() != cur_.size();
        const uint8_t* src = cur_.data();
        if (!intra) {
            delta_.resize(cur_.size());
            for (size_t i = 0; i < cur_.size(); ++i) delta_[i] = uint8_t(cur_[i] ^ prev_[i]);
            src = delta_.data();
        }
        out.data.clear();
        out.data.reserve(64 + cur_.size() / 8);
        out.data.push_back(intra ? kIntra : kDelta);
        put_varint(out.data, w);
        put_varint(out.data, h);
        rle(src, w * h, out.data);
        out.keyframe = intra;
        out.pts_ns = frame.pts_ns;
        prev_.swap(cur_);
        return true;
    }

private:
    EncoderConfig config_;
    std::vector<uint8_t> cur_, prev_, delta_;
};

class RawDecoder final : public Decoder {
public:
    bool decode(std::span<const uint8_t> bitstream, DecodedFrame& out, std::string* err) override {
        wire::Reader r(std::string_view(reinterpret_cast<const char*>(bitstream.data()), bitstream.size()));
        const uint8_t kind = r.u8();
        const uint64_t w = r.varint_max(kMaxDimension);
        const uint64_t h = r.varint_max(kMaxDimension);
        if (!r.ok() || kind > kDelta || w == 0 || h == 0 || w * h > kMaxPixels) {
            return fail(err, "malformed raw packet header");
        }
        const size_t n = size_t(w * h);
        if (kind == kDelta && (pic_.size() != n * 4 || w != width_)) {
            return fail(err, "raw delta frame without a matching reference (lost sync)");
        }
        next_.resize(n * 4);
        size_t i = 0;
        while (i < n) {
            const uint64_t t = r.varint();
            if (!r.ok()) return fail(err, "truncated raw packet");
            const uint64_t count = (t >> 1) + 1;
            if (count > n - i) return fail(err, "raw token runs past the picture");
            if (t & 1) {
                std::string_view p = r.raw(4);
                if (!r.ok()) return fail(err, "truncated raw packet");
                for (uint64_t k = 0; k < count; ++k) std::memcpy(next_.data() + (i + k) * 4, p.data(), 4);
            } else {
                std::string_view p = r.raw(size_t(count) * 4);
                if (!r.ok()) return fail(err, "truncated raw packet");
                std::memcpy(next_.data() + i * 4, p.data(), p.size());
            }
            i += size_t(count);
        }
        if (kind == kDelta) {
            for (size_t k = 0; k < next_.size(); ++k) next_[k] ^= pic_[k];
        }
        pic_.swap(next_);
        width_ = uint32_t(w);
        height_ = uint32_t(h);
        out.ready = true;
        out.width = width_;
        out.height = height_;
        out.format = PixelFormat::RGBA8;
        out.stride = width_ * 4;
        out.uv_offset = 0;
        out.data = pic_;
        return true;
    }

private:
    bool fail(std::string* err, const char* what) {
        if (err) *err = what;
        pic_.clear();  // a predicted frame after an error must not decode against a stale picture
        return false;
    }

    std::vector<uint8_t> pic_, next_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

}  // namespace

std::unique_ptr<Encoder> create_raw_encoder(const EncoderConfig& c, std::string*) {
    return std::make_unique<RawEncoder>(c);
}

std::unique_ptr<Decoder> create_raw_decoder(std::string*) { return std::make_unique<RawDecoder>(); }

}  // namespace broremote::detail
