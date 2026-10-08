// Wire primitives, every protocol message, and the Raw codec: round trips and
// hostile input (huge lengths and counts, truncated bodies, bad values).
#include "broremote/codec.h"
#include "broremote/protocol.h"
#include "broremote/wire.h"
#include "check.h"

#include <cstring>
#include <limits>
#include <random>

using namespace broremote;

namespace {

// Split a framed message back into (type, payload); checks the framing.
wire::MessageSplitter::Message unframe(const std::string& framed, std::string& storage) {
    storage = framed;
    wire::MessageSplitter sp;
    sp.feed(storage.data(), storage.size());
    wire::MessageSplitter::Message m;
    CHECK(sp.next(m));
    CHECK_EQ(sp.buffered(), size_t(0));
    // The splitter's view dies with it: point into `storage` instead.
    m.payload = std::string_view(storage).substr(wire::kHeaderBytes);
    return m;
}

// Every strict prefix of a body must fail to decode (bodies are self-delimiting
// so a truncation is always detectable), and the full body must succeed.
template <class Msg>
void check_truncations(const std::string& framed) {
    std::string storage;
    auto m = unframe(framed, storage);
    for (size_t n = 0; n < m.payload.size(); ++n) {
        Msg x;
        if (x.decode(m.payload.substr(0, n))) {
            check::fail(__FILE__, __LINE__, "a truncated body decoded (" + std::to_string(n) + " of " +
                                                std::to_string(m.payload.size()) + " bytes, type " +
                                                std::to_string(m.type) + ")");
            return;
        }
    }
    Msg full;
    CHECK(full.decode(m.payload));
}

void test_primitives() {
    check::phase("primitives");
    wire::Writer w;
    w.u8(0xAB);
    w.u16(0xBEEF);
    w.u32(0xDEADBEEF);
    w.u64(0x0123456789ABCDEFull);
    const uint64_t vs[] = {0, 1, 127, 128, 300, 16383, 16384, uint64_t(1) << 35, std::numeric_limits<uint64_t>::max()};
    for (uint64_t v : vs) w.varint(v);
    const int64_t ss[] = {0, -1, 1, -64, 64, std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()};
    for (int64_t v : ss) w.svarint(v);
    w.f32(-1.5f);
    w.boolean(true);
    w.str("hello");
    const uint8_t bytes[] = {0, 1, 2, 255};
    w.bytes(bytes, 4);

    // Little endian on the wire, whatever the host.
    const std::string& d = w.data();
    CHECK_EQ(uint8_t(d[1]), uint8_t(0xEF));
    CHECK_EQ(uint8_t(d[2]), uint8_t(0xBE));
    CHECK_EQ(uint8_t(d[3]), uint8_t(0xEF));
    CHECK_EQ(uint8_t(d[6]), uint8_t(0xDE));

    wire::Reader r(d);
    CHECK_EQ(r.u8(), uint8_t(0xAB));
    CHECK_EQ(r.u16(), uint16_t(0xBEEF));
    CHECK_EQ(r.u32(), 0xDEADBEEFu);
    CHECK_EQ(r.u64(), 0x0123456789ABCDEFull);
    for (uint64_t v : vs) CHECK_EQ(r.varint(), v);
    for (int64_t v : ss) CHECK_EQ(r.svarint(), v);
    CHECK_EQ(r.f32(), -1.5f);
    CHECK_EQ(r.boolean(), true);
    CHECK_EQ(r.str(), std::string("hello"));
    CHECK(r.bytes() == std::vector<uint8_t>(bytes, bytes + 4));
    CHECK(r.ok());
    CHECK(r.at_end());
    CHECK_EQ(r.u8(), uint8_t(0));  // past the end: fails, returns 0
    CHECK(!r.ok());

    check::phase("hostile primitives");
    {
        // An 11-byte varint, and a 10th byte that overflows 64 bits.
        std::string over(10, char(0xFF));
        over.push_back(0x01);
        wire::Reader a(over);
        a.varint();
        CHECK(!a.ok());
        std::string ten(9, char(0xFF));
        ten.push_back(0x02);
        wire::Reader b(ten);
        b.varint();
        CHECK(!b.ok());
    }
    {
        // A string claiming 2^62 bytes.
        wire::Writer h;
        h.varint(uint64_t(1) << 62);
        h.raw("abc");
        wire::Reader a(h.data());
        CHECK(a.str_view().empty());
        CHECK(!a.ok());
    }
    {
        // A count larger than the remaining bytes could hold.
        wire::Writer h;
        h.varint(1000000);
        h.raw("ab");
        wire::Reader a(h.data());
        CHECK_EQ(a.count(1), size_t(0));
        CHECK(!a.ok());
    }
    {
        wire::Writer h;
        h.varint(uint64_t(1) << 33);
        wire::Reader a(h.data());
        a.varint32();
        CHECK(!a.ok());
        wire::Writer s;
        s.svarint(int64_t(1) << 40);
        wire::Reader b(s.data());
        b.svarint32();
        CHECK(!b.ok());
        wire::Reader c(std::string_view("\x02", 1));
        c.boolean();
        CHECK(!c.ok());
    }
}

void test_splitter() {
    check::phase("splitter");
    std::string stream = wire::make_message(7, "abc") + wire::make_message(8, "") + wire::make_message(9, "xyz");
    // Byte by byte: messages come out whole and in order.
    wire::MessageSplitter sp;
    std::vector<std::pair<uint16_t, std::string>> got;
    for (char c : stream) {
        sp.feed(&c, 1);
        wire::MessageSplitter::Message m;
        while (sp.next(m)) got.emplace_back(m.type, std::string(m.payload));
    }
    CHECK_EQ(got.size(), size_t(3));
    if (got.size() == 3) {
        CHECK_EQ(got[0].first, uint16_t(7));
        CHECK_EQ(got[0].second, std::string("abc"));
        CHECK_EQ(got[1].second, std::string(""));
        CHECK_EQ(got[2].second, std::string("xyz"));
    }
    CHECK(!sp.error());

    // Lengths below 2 or above kMaxMessage are framing errors, even before
    // the body arrives (no buffering a 4 GiB message).
    for (uint32_t bad : {0u, 1u, uint32_t(wire::kMaxMessage + 1), 0xFFFFFFFFu}) {
        wire::MessageSplitter s;
        char hdr[4] = {char(bad), char(bad >> 8), char(bad >> 16), char(bad >> 24)};
        s.feed(hdr, 4);
        wire::MessageSplitter::Message m;
        CHECK(!s.next(m));
        CHECK(s.error());
    }
    // The largest legal length is accepted (and waits for its body).
    {
        wire::MessageSplitter s;
        const uint32_t len = uint32_t(wire::kMaxMessage);
        char hdr[6] = {char(len), char(len >> 8), char(len >> 16), char(len >> 24), 1, 0};
        s.feed(hdr, 6);
        wire::MessageSplitter::Message m;
        CHECK(!s.next(m));
        CHECK(!s.error());
    }
}

void test_messages() {
    check::phase("messages");
    std::string st;
    {
        HelloMsg a;
        a.name = "viewer";
        a.minor = 3;
        auto m = unframe(a.encode(), st);
        CHECK_EQ(m.type, uint16_t(MsgType::Hello));
        HelloMsg b;
        CHECK(b.decode(m.payload));
        CHECK_EQ(b.major, kProtocolMajor);
        CHECK_EQ(b.minor, uint16_t(3));
        CHECK_EQ(b.name, std::string("viewer"));
        check_truncations<HelloMsg>(a.encode());
        // Wrong magic.
        std::string bad(m.payload);
        bad[0] = 'X';
        CHECK(!b.decode(bad));
        // Trailing bytes (a newer minor's fields) are ignored.
        CHECK(b.decode(std::string(m.payload) + "future"));
        // An oversized name is refused.
        HelloMsg big;
        big.name = std::string(kMaxNameBytes + 1, 'n');
        auto bm = unframe(big.encode(), st);
        CHECK(!b.decode(bm.payload));
    }
    {
        WelcomeMsg a{1, 2, "srv"};
        auto m = unframe(a.encode(), st);
        CHECK_EQ(m.type, uint16_t(MsgType::Welcome));
        WelcomeMsg b;
        CHECK(b.decode(m.payload));
        CHECK_EQ(b.minor, uint16_t(2));
        CHECK_EQ(b.name, std::string("srv"));
        check_truncations<WelcomeMsg>(a.encode());
    }
    {
        AckMsg a{uint64_t(1) << 40};
        auto m = unframe(a.encode(), st);
        AckMsg b;
        CHECK(b.decode(m.payload));
        CHECK_EQ(b.frame_id, uint64_t(1) << 40);
        check_truncations<AckMsg>(a.encode());
    }
    {
        auto m = unframe(RequestKeyframeMsg{}.encode(), st);
        CHECK_EQ(m.type, uint16_t(MsgType::RequestKeyframe));
        CHECK(m.payload.empty());
    }
    {
        const InputEvent evs[] = {InputEvent::key(30, true), InputEvent::key(30, false), InputEvent::motion(12.25f, -3.5f),
                                  InputEvent::button(0x110, true), InputEvent::wheel(-120, 360)};
        for (const InputEvent& e : evs) {
            InputMsg a{e};
            auto m = unframe(a.encode(), st);
            CHECK_EQ(m.type, uint16_t(MsgType::Input));
            InputMsg b;
            CHECK(b.decode(m.payload));
            CHECK(b.event == e);
            check_truncations<InputMsg>(a.encode());
        }
        InputMsg b;
        bool unknown = false;
        CHECK(!b.decode(std::string_view("\x09\x01\x02", 3), &unknown));
        CHECK(unknown);
        CHECK(!b.decode(std::string_view("\x01\x05\x07", 3), &unknown));  // pressed = 7
        CHECK(!unknown);
        // NaN positions are refused.
        wire::Writer w;
        w.u8(uint8_t(InputKind::PointerMotion));
        w.f32(std::numeric_limits<float>::quiet_NaN());
        w.f32(1.0f);
        CHECK(!b.decode(w.data()));
    }
    {
        SetCodecMsg a;
        a.codecs = {Codec::HEVC, Codec::H264, Codec::Raw};
        a.max_bitrate_kbps = 8000;
        auto m = unframe(a.encode(), st);
        SetCodecMsg b;
        CHECK(b.decode(m.payload));
        CHECK(b.codecs == a.codecs);
        CHECK_EQ(b.max_bitrate_kbps, 8000u);
        check_truncations<SetCodecMsg>(a.encode());
        // Unknown codec values are dropped, not errors.
        wire::Writer w;
        w.varint(2);
        w.u8(200);
        w.u8(uint8_t(Codec::AV1));
        w.varint(0);
        CHECK(b.decode(w.data()));
        CHECK(b.codecs == std::vector<Codec>{Codec::AV1});
        // A huge count is refused without allocating.
        wire::Writer h;
        h.varint(uint64_t(1) << 50);
        CHECK(!b.decode(h.data()));
        wire::Writer many;
        many.varint(kMaxCodecList + 1);
        for (size_t i = 0; i < kMaxCodecList + 1; ++i) many.u8(0);
        many.varint(0);
        CHECK(!b.decode(many.data()));
    }
    {
        StreamConfig a;
        a.stream_id = 9;
        a.codec = Codec::H264;
        a.width = 1920;
        a.height = 1080;
        a.fps = 60;
        auto m = unframe(a.encode(), st);
        CHECK_EQ(m.type, uint16_t(MsgType::StreamConfig));
        StreamConfig b;
        CHECK(b.decode(m.payload));
        CHECK(b == a);
        check_truncations<StreamConfig>(a.encode());
        StreamConfig z = a;
        z.width = 0;
        CHECK(!b.decode(unframe(z.encode(), st).payload));
        z.width = kMaxDimension + 1;
        CHECK(!b.decode(unframe(z.encode(), st).payload));
    }
    {
        VideoPacket a;
        a.stream_id = 3;
        a.frame_id = 77;
        a.pts_ns = -5;
        a.keyframe = true;
        a.data = {1, 2, 3, 4, 5};
        auto m = unframe(a.encode(), st);
        CHECK_EQ(m.type, uint16_t(MsgType::Video));
        VideoPacket b;
        CHECK(b.decode(m.payload));
        CHECK_EQ(b.stream_id, uint64_t(3));
        CHECK_EQ(b.frame_id, uint64_t(77));
        CHECK_EQ(b.pts_ns, int64_t(-5));
        CHECK(b.keyframe);
        CHECK(b.data == a.data);
        check_truncations<VideoPacket>(a.encode());
        // A bitstream length beyond the body.
        wire::Writer w;
        w.varint(1);
        w.varint(1);
        w.svarint(0);
        w.u8(0);
        w.varint(uint64_t(1) << 40);
        w.raw("xx");
        CHECK(!b.decode(w.data()));
        CHECK(!b.timing.valid);  // a 1.0 body: no timing
    }
    {
        // 1.1: timing after the bitstream. Whole or malformed; trailing bytes after it ignored.
        VideoPacket a;
        a.stream_id = 1;
        a.frame_id = 2;
        a.data = {9, 9, 9};
        a.timing.valid = true;
        a.timing.submit_us = 123456789012ull;
        a.timing.queue_us = 300;
        a.timing.encode_us = 4100;
        const std::string framed = a.encode();
        auto m = unframe(framed, st);
        VideoPacket b;
        CHECK(b.decode(m.payload));
        CHECK(b.timing == a.timing);
        CHECK(b.data == a.data);
        VideoPacket plain = a;
        plain.timing = FrameTiming{};
        std::string st2;
        const size_t body10 = unframe(plain.encode(), st2).payload.size();
        for (size_t n = body10 + 1; n < m.payload.size(); ++n) CHECK(!b.decode(m.payload.substr(0, n)));
        std::string longer(m.payload);
        longer += "later minor";
        CHECK(b.decode(longer));
        CHECK(b.timing == a.timing);
    }
    {
        PingMsg a{0x0102030405060708ull};
        auto m = unframe(a.encode(), st);
        CHECK_EQ(m.type, uint16_t(MsgType::Ping));
        PingMsg b;
        CHECK(b.decode(m.payload));
        CHECK_EQ(b.token, a.token);
        check_truncations<PingMsg>(a.encode());
        PongMsg c{77, 1234567};
        m = unframe(c.encode(), st);
        CHECK_EQ(m.type, uint16_t(MsgType::Pong));
        PongMsg d;
        CHECK(d.decode(m.payload));
        CHECK_EQ(d.token, uint64_t(77));
        CHECK_EQ(d.server_time_us, uint64_t(1234567));
        check_truncations<PongMsg>(c.encode());
        FrameSentMsg e{42, 1500, 80};
        m = unframe(e.encode(), st);
        CHECK_EQ(m.type, uint16_t(MsgType::FrameSent));
        FrameSentMsg f;
        CHECK(f.decode(m.payload));
        CHECK_EQ(f.frame_id, uint64_t(42));
        CHECK_EQ(f.wait_us, uint64_t(1500));
        CHECK_EQ(f.write_us, uint64_t(80));
        check_truncations<FrameSentMsg>(e.encode());
    }
    {
        CursorMsg a;
        a.state.visible = false;
        a.state.x = -4;
        a.state.y = 100000;
        a.state.hotspot_x = 3;
        a.state.hotspot_y = 7;
        a.state.shape = "text";
        auto m = unframe(a.encode(), st);
        CursorMsg b;
        CHECK(b.decode(m.payload));
        CHECK(b.state == a.state);
        check_truncations<CursorMsg>(a.encode());
    }
    {
        ErrorMsg a{ErrorCode::NoCommonCodec, "nope"};
        auto m = unframe(a.encode(), st);
        CHECK_EQ(m.type, uint16_t(MsgType::Error));
        ErrorMsg b;
        CHECK(b.decode(m.payload));
        CHECK_EQ(b.code, ErrorCode::NoCommonCodec);
        CHECK_EQ(b.message, std::string("nope"));
        check_truncations<ErrorMsg>(a.encode());
    }
}

// Random bytes into every decoder: no crash, no hang, no huge allocation.
void test_fuzz() {
    check::phase("fuzz");
    std::mt19937 rng(1234);
    for (int i = 0; i < 20000; ++i) {
        std::string body(size_t(rng() % 48), '\0');
        for (char& c : body) c = char(rng());
        HelloMsg().decode(body);
        WelcomeMsg().decode(body);
        AckMsg().decode(body);
        InputMsg().decode(body);
        SetCodecMsg().decode(body);
        StreamConfig().decode(body);
        VideoPacket().decode(body);
        CursorMsg().decode(body);
        ErrorMsg().decode(body);
        PingMsg().decode(body);
        PongMsg().decode(body);
        FrameSentMsg().decode(body);
    }
    CHECK(true);
}

std::vector<uint8_t> pattern(uint32_t w, uint32_t h, uint32_t seed) {
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = &px[(size_t(y) * w + x) * 4];
            // Flat areas (runs) and noisy ones (literals).
            const bool flat = ((x / 8) + (y / 8) + seed) % 3 == 0;
            p[0] = flat ? 10 : uint8_t(x * 7 + seed);
            p[1] = flat ? 20 : uint8_t(y * 13 + seed * 3);
            p[2] = flat ? 30 : uint8_t((x ^ y) + seed);
            p[3] = 255;
        }
    }
    return px;
}

void test_raw_codec() {
    check::phase("raw codec");
    std::string err;
    const uint32_t w = 61, h = 37;  // odd sizes
    auto enc = create_encoder(Codec::Raw, {w, h, 60, 1000}, &err);
    auto dec = create_decoder(Codec::Raw, &err);
    CHECK(enc && dec);
    if (!enc || !dec) return;
    for (uint32_t i = 0; i < 6; ++i) {
        // A padded stride on odd frames.
        const uint32_t stride = (i & 1) ? w * 4 + 12 : 0;
        std::vector<uint8_t> src = pattern(w, h, i < 3 ? 0 : i);  // frames 1, 2 repeat frame 0
        std::vector<uint8_t> padded;
        const uint8_t* cpu = src.data();
        if (stride) {
            padded.assign(size_t(stride) * h, 0xEE);
            for (uint32_t y = 0; y < h; ++y) std::memcpy(&padded[size_t(y) * stride], &src[size_t(y) * w * 4], w * 4);
            cpu = padded.data();
        }
        Frame f;
        f.width = w;
        f.height = h;
        f.cpu = cpu;
        f.cpu_stride = stride;
        f.pts_ns = i;
        int released = 0;
        EncodedPacket pkt;
        CHECK(enc->encode(f, i == 4, [&] { ++released; }, pkt, &err));
        CHECK_EQ(released, 1);
        CHECK_EQ(pkt.keyframe, i == 0 || i == 4);
        CHECK_EQ(pkt.pts_ns, int64_t(i));
        if (i == 1 || i == 2) CHECK(pkt.data.size() < 16);  // an unchanged frame is a single run
        DecodedFrame out;
        CHECK(dec->decode(pkt.data, out, &err));
        CHECK(out.ready);
        CHECK_EQ(out.width, w);
        CHECK_EQ(out.height, h);
        CHECK_EQ(out.stride, w * 4);
        CHECK(out.data == src);
    }
    // Wrong size and non-CPU frames fail, still releasing exactly once.
    {
        std::vector<uint8_t> px(size_t(w + 1) * h * 4);
        Frame f;
        f.width = w + 1;
        f.height = h;
        f.cpu = px.data();
        int released = 0;
        EncodedPacket pkt;
        CHECK(!enc->encode(f, false, [&] { ++released; }, pkt, &err));
        CHECK_EQ(released, 1);
        Frame d;
        d.width = w;
        d.height = h;
        d.plane_count = 1;
        CHECK(!enc->encode(d, false, [&] { ++released; }, pkt, &err));
        CHECK_EQ(released, 2);
    }
    check::phase("raw codec hostile");
    {
        auto d2 = create_decoder(Codec::Raw, &err);
        DecodedFrame out;
        // A delta frame with no reference: lost sync.
        std::vector<uint8_t> delta = {1, 2, 2, 0x07, 0, 0, 0, 0};
        CHECK(!d2->decode(delta, out, &err));
        // Truncated, oversized and overrunning packets.
        CHECK(!d2->decode(std::vector<uint8_t>{}, out, &err));
        CHECK(!d2->decode(std::vector<uint8_t>{0, 2}, out, &err));
        std::vector<uint8_t> huge = {0, 0x80, 0x80, 0x04, 0x80, 0x80, 0x04};  // 65536 x 65536
        CHECK(!d2->decode(huge, out, &err));
        std::vector<uint8_t> overrun = {0, 2, 2, 0x0F, 1, 2, 3, 4};  // a run of 8 into 4 pixels
        CHECK(!d2->decode(overrun, out, &err));
        std::vector<uint8_t> shortlit = {0, 2, 2, 0x06, 1, 2, 3};  // a literal of 4 pixels, 3 bytes
        CHECK(!d2->decode(shortlit, out, &err));
        std::vector<uint8_t> ok = {0, 2, 2, 0x07, 9, 8, 7, 6};
        CHECK(d2->decode(ok, out, &err));
        CHECK_EQ(out.data.size(), size_t(16));
        CHECK_EQ(out.data[15], uint8_t(6));
        // After an error, a delta frame must not decode against the old picture.
        CHECK(!d2->decode(overrun, out, &err));
        CHECK(!d2->decode(delta, out, &err));
        std::mt19937 rng(99);
        for (int i = 0; i < 5000; ++i) {
            std::vector<uint8_t> junk(size_t(rng() % 32));
            for (auto& b : junk) b = uint8_t(rng());
            if (!junk.empty()) junk[0] &= 1;
            if (junk.size() > 2) junk[1] &= 0x3F, junk[2] &= 0x3F;
            d2->decode(junk, out, &err);
        }
        CHECK(true);
    }
    check::phase("factory");
    {
        auto encs = available_encoders();
        auto decs = available_decoders();
        CHECK(!encs.empty() && encs.front() == Codec::Raw);
        CHECK(!decs.empty() && decs.front() == Codec::Raw);
        CHECK(!create_encoder(Codec::Raw, {0, 10, 60, 1}, &err));
        CHECK(parse_codec("h265") == Codec::HEVC);
        CHECK(parse_codec("raw") == Codec::Raw);
        CHECK(!parse_codec("vp9"));
    }
}

}  // namespace

int main() {
    check::watchdog(120);
    test_primitives();
    test_splitter();
    test_messages();
    test_fuzz();
    test_raw_codec();
    return check::finish();
}
