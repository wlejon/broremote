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
//
// The primitives and the framing are brolink's (brolink/wire.h); broremote
// fixes its own message bound.

#include <brolink/wire.h>

#include <cstddef>

namespace broremote::wire {

// Largest message either side accepts (type + body).
inline constexpr size_t kMaxMessage = 64u << 20;
using brolink::wire::kHeaderBytes;
using brolink::wire::kLengthBytes;

using brolink::wire::Reader;
using brolink::wire::Writer;
using brolink::wire::frame_message;
using brolink::wire::make_message;

// Splits a byte stream into messages, bounded by kMaxMessage. A length
// beyond it (or below the type field) is a framing error: the stream cannot
// be resynchronised and must be closed.
class MessageSplitter : public brolink::wire::MessageSplitter {
public:
    MessageSplitter() : brolink::wire::MessageSplitter(kMaxMessage) {}
};

}  // namespace broremote::wire
