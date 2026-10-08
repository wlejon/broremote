#pragma once
// Wire primitives: the byte-level encoding every broremote message uses.
//
// A message on the stream is
//     u32 length (little endian)  -- bytes that follow: the type and the body
//     u16 type   (little endian)  -- MsgType (protocol.h)
//     body                        -- type-specific, built from the primitives below
// Fixed-width integers are little endian; `varint` is unsigned LEB128 (at most
// 10 bytes), `svarint` is zigzag + LEB128; `f32` is an IEEE-754 single as its
// u32 bit pattern; `str` / `bytes` are a varint length followed by that many
// bytes. Readers never trust a length: every read is bounds-checked and a
// malformed body marks the Reader failed instead of reading past it. Structs
// are never copied onto the wire. docs/protocol.md has the full catalogue.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace broremote::wire {

// Largest message either side accepts (type + body).
inline constexpr size_t kMaxMessage = 64u << 20;
inline constexpr size_t kLengthBytes = 4;
inline constexpr size_t kHeaderBytes = 6;  // length + type

class Writer {
public:
    Writer() = default;
    explicit Writer(std::string& out) : ext_(&out) {}

    void u8(uint8_t v) { buf().push_back(char(v)); }
    void u16(uint16_t v) {
        char b[2] = {char(v), char(v >> 8)};
        buf().append(b, 2);
    }
    void u32(uint32_t v) {
        char b[4];
        for (int i = 0; i < 4; ++i) b[i] = char(v >> (8 * i));
        buf().append(b, 4);
    }
    void u64(uint64_t v) {
        char b[8];
        for (int i = 0; i < 8; ++i) b[i] = char(v >> (8 * i));
        buf().append(b, 8);
    }
    void varint(uint64_t v) {
        char b[10];
        int n = 0;
        while (v >= 0x80) {
            b[n++] = char(uint8_t(v) | 0x80);
            v >>= 7;
        }
        b[n++] = char(v);
        buf().append(b, size_t(n));
    }
    void svarint(int64_t v) { varint((uint64_t(v) << 1) ^ uint64_t(v >> 63)); }
    void f32(float v) { u32(std::bit_cast<uint32_t>(v)); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void str(std::string_view s) {
        varint(s.size());
        buf().append(s.data(), s.size());
    }
    void bytes(const uint8_t* p, size_t n) {
        varint(n);
        buf().append(reinterpret_cast<const char*>(p), n);
    }
    void raw(std::string_view s) { buf().append(s.data(), s.size()); }

    [[nodiscard]] std::string& data() { return buf(); }
    [[nodiscard]] size_t size() const { return ext_ ? ext_->size() : own_.size(); }
    [[nodiscard]] std::string take() { return std::move(buf()); }

private:
    std::string& buf() { return ext_ ? *ext_ : own_; }
    std::string own_;
    std::string* ext_{nullptr};
};

class Reader {
public:
    Reader() = default;
    explicit Reader(std::string_view data) : d_(data) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] bool at_end() const noexcept { return pos_ >= d_.size(); }
    [[nodiscard]] size_t remaining() const noexcept { return ok_ ? d_.size() - pos_ : 0; }
    // Marks the reader failed (a semantic check of the caller failed).
    void fail() noexcept { ok_ = false; }

    uint8_t u8() {
        if (!need(1)) return 0;
        return uint8_t(d_[pos_++]);
    }
    uint16_t u16() {
        if (!need(2)) return 0;
        uint16_t v = uint16_t(uint8_t(d_[pos_]) | (uint8_t(d_[pos_ + 1]) << 8));
        pos_ += 2;
        return v;
    }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(uint8_t(d_[pos_ + size_t(i)])) << (8 * i);
        pos_ += 4;
        return v;
    }
    uint64_t u64() {
        if (!need(8)) return 0;
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= uint64_t(uint8_t(d_[pos_ + size_t(i)])) << (8 * i);
        pos_ += 8;
        return v;
    }
    uint64_t varint() {
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (!need(1)) return 0;
            uint8_t b = uint8_t(d_[pos_++]);
            if (shift == 63 && b > 1) break;  // overflow
            v |= uint64_t(b & 0x7F) << shift;
            if (!(b & 0x80)) return v;
        }
        ok_ = false;
        return 0;
    }
    // A varint that must fit `limit` (inclusive); larger marks the reader failed.
    uint64_t varint_max(uint64_t limit) {
        uint64_t v = varint();
        if (v > limit) {
            ok_ = false;
            return 0;
        }
        return v;
    }
    uint32_t varint32() { return uint32_t(varint_max(0xFFFFFFFFu)); }
    int64_t svarint() {
        uint64_t v = varint();
        return int64_t(v >> 1) ^ -int64_t(v & 1);
    }
    // An svarint that must fit an int32.
    int32_t svarint32() {
        int64_t v = svarint();
        if (v < INT32_MIN || v > INT32_MAX) {
            ok_ = false;
            return 0;
        }
        return int32_t(v);
    }
    float f32() { return std::bit_cast<float>(u32()); }
    bool boolean() {
        uint8_t v = u8();
        if (v > 1) ok_ = false;
        return v == 1;
    }
    std::string_view str_view() {
        uint64_t n = varint();
        if (!ok_ || !need(n)) return {};
        std::string_view s = d_.substr(pos_, size_t(n));
        pos_ += size_t(n);
        return s;
    }
    std::string str() { return std::string(str_view()); }
    // A str at most `limit` bytes long.
    std::string str_max(size_t limit) {
        std::string_view s = str_view();
        if (s.size() > limit) {
            ok_ = false;
            return {};
        }
        return std::string(s);
    }
    std::vector<uint8_t> bytes() {
        std::string_view s = str_view();
        return std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(s.data()),
                                    reinterpret_cast<const uint8_t*>(s.data()) + s.size());
    }
    std::string_view raw(size_t n) {
        if (!need(n)) return {};
        std::string_view s = d_.substr(pos_, n);
        pos_ += n;
        return s;
    }
    // A count of elements that each take at least `min_bytes`: rejected when
    // the remaining input could not possibly hold them (no huge reserve()).
    size_t count(size_t min_bytes = 1) {
        uint64_t n = varint();
        if (!ok_) return 0;
        if (min_bytes && n > remaining() / min_bytes) {
            ok_ = false;
            return 0;
        }
        return size_t(n);
    }

private:
    bool need(uint64_t n) {
        if (!ok_ || n > d_.size() - pos_) {
            ok_ = false;
            return false;
        }
        return true;
    }
    std::string_view d_;
    size_t pos_{0};
    bool ok_{true};
};

// Append a framed message (length, type, payload) to `out`.
void frame_message(std::string& out, uint16_t type, std::string_view payload);
[[nodiscard]] std::string make_message(uint16_t type, std::string_view payload);

// Splits a byte stream into messages. Feed bytes as they arrive; next()
// yields each complete message (type, payload) in order. A length beyond
// kMaxMessage (or below the type field) is a framing error: the stream
// cannot be resynchronised and must be closed.
class MessageSplitter {
public:
    void feed(const char* data, size_t n);
    struct Message {
        uint16_t type{0};
        std::string_view payload;  // valid until the next feed()
    };
    // True and fills `m` when a message is ready.
    bool next(Message& m);
    [[nodiscard]] bool error() const noexcept { return error_; }
    [[nodiscard]] size_t buffered() const noexcept { return buf_.size() - head_; }

private:
    std::string buf_;
    size_t head_{0};
    bool error_{false};
};

}  // namespace broremote::wire
