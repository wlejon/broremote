#include "broremote/wire.h"

namespace broremote::wire {

void frame_message(std::string& out, uint16_t type, std::string_view payload) {
    const uint32_t len = uint32_t(2 + payload.size());
    char hdr[kHeaderBytes];
    for (int i = 0; i < 4; ++i) hdr[i] = char(len >> (8 * i));
    hdr[4] = char(type);
    hdr[5] = char(type >> 8);
    out.append(hdr, kHeaderBytes);
    out.append(payload.data(), payload.size());
}

std::string make_message(uint16_t type, std::string_view payload) {
    std::string out;
    out.reserve(payload.size() + kHeaderBytes);
    frame_message(out, type, payload);
    return out;
}

void MessageSplitter::feed(const char* data, size_t n) {
    if (error_ || n == 0) return;
    // Compact consumed bytes before growing.
    if (head_ > 0 && (head_ >= buf_.size() / 2 || head_ > (1u << 20))) {
        buf_.erase(0, head_);
        head_ = 0;
    }
    buf_.append(data, n);
}

bool MessageSplitter::next(Message& m) {
    if (error_) return false;
    const size_t avail = buf_.size() - head_;
    if (avail < kLengthBytes) return false;
    const auto* p = reinterpret_cast<const unsigned char*>(buf_.data() + head_);
    const uint32_t len = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    if (len < 2 || len > kMaxMessage) {
        error_ = true;
        return false;
    }
    if (avail < kLengthBytes + len) return false;
    m.type = uint16_t(p[4] | (p[5] << 8));
    m.payload = std::string_view(buf_.data() + head_ + kHeaderBytes, len - 2);
    // The bytes stay put until the next feed() (which compacts), so the view
    // stays valid across further next() calls.
    head_ += kLengthBytes + len;
    return true;
}

}  // namespace broremote::wire
