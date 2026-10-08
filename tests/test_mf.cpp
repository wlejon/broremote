// The Media Foundation decoder (Windows), through the codec factory.
//
// With ffmpeg on PATH, the test pattern is encoded by libx264 / libx265 /
// libaom (I and P only, as the VA-API encoder makes them) at two sizes
// joined into one stream, then decoded: every picture must be the right size,
// carry the right frame counter, and match its source (luma PSNR against the
// ideal BT.709 luma, RGB PSNR, the colour swatches). Also: starting at a
// later keyframe (predicted frames before it are a lost sync), garbage in
// the middle (no crash; the next keyframe recovers), software decoding, and
// the decode time per frame at 1920x1080.
//
// Without ffmpeg those parts are skipped; the committed fixtures recorded
// from the VA-API encoder (tests/fixtures/halo_*) are decoded either way.
#include "broremote/codec.h"
#include "check.h"
#include "elementary.h"
#include "ffmpeg_oracle.h"
#include "picture.h"
#include "test_pattern.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <random>

#include <process.h>

using namespace broremote;
namespace fs = std::filesystem;

namespace {

fs::path g_scratch;

bool decodes(Codec c) {
    const auto v = available_decoders();
    return std::find(v.begin(), v.end(), c) != v.end();
}

void set_hardware(bool on) { _putenv_s("BROREMOTE_MF_HARDWARE", on ? "1" : "0"); }

// ---- fixtures made with ffmpeg ---------------------------------------------------

const char* encoder_args(Codec c) {
    switch (c) {
        case Codec::H264:
            return "-c:v libx264 -preset veryfast -qp 16 -bf 0 -sc_threshold 0 -x264-params repeat-headers=1 -f h264";
        case Codec::HEVC:
            return "-c:v libx265 -preset veryfast -x265-params "
                   "bframes=0:scenecut=0:repeat-headers=1:info=0:log-level=error:qp=16 -f hevc";
        case Codec::AV1:
            return "-c:v libaom-av1 -usage realtime -cpu-used 8 -lag-in-frames 0 -crf 16 -b:v 0 -f obu";
        default: return "";
    }
}

// Pattern frames first .. first+count-1 at w x h, a keyframe every `gop`.
bool encode_segment(Codec c, const fs::path& out, uint32_t w, uint32_t h, uint32_t first, uint32_t count,
                    uint32_t gop) {
    const std::string cmd = "ffmpeg -hide_banner -nostdin -v error -y -f rawvideo -pix_fmt rgba -s " +
                            std::to_string(w) + "x" + std::to_string(h) +
                            " -r 60 -i - -sws_flags bicubic+accurate_rnd+full_chroma_int"
                            " -vf scale=out_color_matrix=bt709:out_range=limited,format=yuv420p"
                            " -color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv -g " +
                            std::to_string(gop) + " -keyint_min " + std::to_string(gop) + " " + encoder_args(c) +
                            " " + oracle::quote(out.string());
    std::FILE* p = popen(cmd.c_str(), oracle::kPipeWrite);
    if (!p) return false;
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    for (uint32_t n = first; n < first + count; ++n) {
        tools::draw_test_pattern(rgba.data(), w, h, n);
        if (std::fwrite(rgba.data(), 1, rgba.size(), p) != rgba.size()) break;
    }
    return pclose(p) == 0 && fs::exists(out) && fs::file_size(out) > 0;
}

struct Expected {
    uint64_t n;
    uint32_t w, h;
    bool key;
};

struct Fixture {
    std::vector<elementary::Packet> packets;
    std::vector<Expected> expect;
};

struct Segment {
    uint32_t w, h, count;
};

// The segments encoded separately and joined into one stream: a size change
// mid-stream, at a keyframe, as the server makes one.
bool make_fixture(Codec c, const std::string& name, const std::vector<Segment>& segments, uint32_t gop,
                  Fixture& fx) {
    std::vector<uint8_t> all;
    uint32_t first = 0;
    for (size_t i = 0; i < segments.size(); ++i) {
        const Segment& s = segments[i];
        const fs::path part = g_scratch / (name + "." + std::to_string(i));
        if (!encode_segment(c, part, s.w, s.h, first, s.count, gop)) {
            std::printf("   ffmpeg could not encode %s (no %s encoder?)\n", name.c_str(), codec_name(c));
            return false;
        }
        const auto bytes = elementary::read_file(part.string());
        all.insert(all.end(), bytes.begin(), bytes.end());
        for (uint32_t k = 0; k < s.count; ++k) fx.expect.push_back({first + k, s.w, s.h, k % gop == 0});
        first += s.count;
    }
    oracle::write_file((g_scratch / name).string(), all);
    fx.packets = elementary::split(c, all);
    CHECK_EQ(fx.packets.size(), fx.expect.size());
    return fx.packets.size() == fx.expect.size();
}

// ---- checking a picture ----------------------------------------------------------

struct Quality {
    double luma = 99, rgb = 99;
    void take(double l, double r) {
        luma = std::min(luma, l);
        rgb = std::min(rgb, r);
    }
};

double luma_psnr(const DecodedFrame& f, const std::vector<uint8_t>& source) {
    const auto ideal = oracle::ideal_luma(source.data(), f.width, f.height);
    std::vector<uint8_t> y(size_t(f.width) * f.height);
    for (uint32_t r = 0; r < f.height; ++r) {
        std::copy_n(f.data.data() + size_t(r) * f.stride, f.width, y.data() + size_t(r) * f.width);
    }
    return oracle::psnr_luma(y.data(), ideal);
}

// The picture is frame `n` of the pattern at w x h; false (with failures
// recorded) when not.
bool check_picture(const DecodedFrame& f, uint64_t n, uint32_t w, uint32_t h, Quality& q, double min_luma,
                   double min_rgb) {
    CHECK(f.ready);
    CHECK(f.format == PixelFormat::NV12);
    CHECK_EQ(f.width, w);
    CHECK_EQ(f.height, h);
    if (!f.ready || f.width != w || f.height != h) return false;
    CHECK(f.stride >= w);
    CHECK_EQ(f.uv_offset, f.stride * h);
    CHECK(f.data.size() >= size_t(f.stride) * (h + (h + 1) / 2));
    const auto rgba = tools::to_rgba(f);
    CHECK_EQ(tools::read_test_pattern_counter(rgba.data(), w, h, w * 4), int64_t(n));
    std::vector<uint8_t> source(size_t(w) * h * 4);
    tools::draw_test_pattern(source.data(), w, h, n);
    const double l = luma_psnr(f, source);
    const double r = tools::psnr_rgba(rgba.data(), source.data(), w, h);
    q.take(l, r);
    CHECK(l >= min_luma);
    CHECK(r >= min_rgb);
    // The swatches: pure red, green and blue, so a channel swap shows.
    const uint8_t* red = &rgba[(size_t(8) * w + 8) * 4];
    const uint8_t* green = &rgba[(size_t(8) * w + 24) * 4];
    const uint8_t* blue = &rgba[(size_t(8) * w + 40) * 4];
    CHECK(red[0] > 200 && red[1] < 50 && red[2] < 50);
    CHECK(green[0] < 50 && green[1] > 200 && green[2] < 50);
    CHECK(blue[0] < 50 && blue[1] < 50 && blue[2] > 200);
    return l >= min_luma && r >= min_rgb;
}

// ---- the cases -----------------------------------------------------------------

constexpr double kMinLuma = 35.0;
// The pattern's full-swing colour edges through 4:2:0 cap RGB PSNR near
// 30 dB before any coding with this nearest-neighbour chroma upsampling.
constexpr double kMinRgb = 26.0;

void decode_all(Codec c, const Fixture& fx, bool hardware) {
    check::phase(std::string(codec_name(c)) + (hardware ? ": whole stream, hardware" : ": whole stream, software"));
    set_hardware(hardware);
    std::string err;
    auto d = create_decoder(c, &err);
    CHECK(d != nullptr);
    if (!d) {
        std::printf("   %s\n", err.c_str());
        return;
    }
    std::printf("   %s\n", d->describe().c_str());
    Quality q;
    DecodedFrame f;
    for (size_t i = 0; i < fx.packets.size(); ++i) {
        const Expected& e = fx.expect[i];
        const bool ok = d->decode(fx.packets[i], f, &err);
        CHECK(ok);
        if (!ok) {
            std::printf("   frame %zu: %s\n", i, err.c_str());
            continue;
        }
        check_picture(f, e.n, e.w, e.h, q, kMinLuma, kMinRgb);
    }
    std::printf("   %zu frames, sizes %ux%u then %ux%u; min luma PSNR %.1f dB, min RGB PSNR %.1f dB\n",
                fx.packets.size(), fx.expect.front().w, fx.expect.front().h, fx.expect.back().w, fx.expect.back().h,
                q.luma, q.rgb);
}

// Joining mid-stream: predicted frames before the first keyframe are a lost
// sync (false, no picture); from the keyframe on, every frame decodes.
void start_late(Codec c, const Fixture& fx) {
    check::phase(std::string(codec_name(c)) + ": start at a later keyframe");
    set_hardware(true);
    std::string err;
    auto d = create_decoder(c, &err);
    CHECK(d != nullptr);
    if (!d) return;
    size_t start = 3;
    size_t key = start;
    while (key < fx.expect.size() && !fx.expect[key].key) ++key;
    CHECK(key < fx.expect.size());
    Quality q;
    DecodedFrame f;
    for (size_t i = start; i < fx.packets.size(); ++i) {
        const bool ok = d->decode(fx.packets[i], f, &err);
        if (i < key) {
            CHECK(!ok);
            CHECK(!f.ready);
            if (i == start) std::printf("   before the keyframe: %s\n", err.c_str());
        } else {
            CHECK(ok);
            if (ok) check_picture(f, fx.expect[i].n, fx.expect[i].w, fx.expect[i].h, q, kMinLuma, kMinRgb);
        }
    }
    std::printf("   decoded from frame %zu; min luma PSNR %.1f dB\n", key, q.luma);
}

// Garbage mid-stream must never crash the decoder; whatever it makes of it,
// the next keyframe decodes cleanly again.
void garbage(Codec c, const Fixture& fx) {
    check::phase(std::string(codec_name(c)) + ": garbage, then recovery at a keyframe");
    set_hardware(true);
    std::string err;
    auto d = create_decoder(c, &err);
    CHECK(d != nullptr);
    if (!d) return;
    DecodedFrame f;
    for (size_t i = 0; i < 4; ++i) CHECK(d->decode(fx.packets[i], f, &err));
    std::mt19937 rng(7);
    int failures = 0;
    for (int k = 0; k < 20; ++k) {
        std::vector<uint8_t> junk(64 + rng() % 4000);
        for (auto& b : junk) b = uint8_t(rng());
        if (k % 2 == 0 && c != Codec::AV1) {
            junk[0] = 0, junk[1] = 0, junk[2] = 0, junk[3] = 1;  // a start code, then nonsense
        }
        if (!d->decode(junk, f, &err)) ++failures;
    }
    // Truncated real packets.
    for (size_t i = 4; i < 8; ++i) {
        std::vector<uint8_t> half(fx.packets[i].begin(), fx.packets[i].begin() + fx.packets[i].size() / 3);
        if (!d->decode(half, f, &err)) ++failures;
    }
    std::printf("   %d of 24 bad packets reported as failures\n", failures);
    CHECK(d->decode({}, f, &err) == false);
    size_t key = 8;
    while (key < fx.expect.size() && !fx.expect[key].key) ++key;
    Quality q;
    for (size_t i = key; i < fx.packets.size(); ++i) {
        CHECK(d->decode(fx.packets[i], f, &err));
        check_picture(f, fx.expect[i].n, fx.expect[i].w, fx.expect[i].h, q, kMinLuma, kMinRgb);
    }
    std::printf("   recovered at frame %zu; min luma PSNR %.1f dB\n", key, q.luma);
}

void timing(Codec c, bool hardware) {
    check::phase(std::string(codec_name(c)) + ": 1920x1080 decode time" + (hardware ? ", hardware" : ", software"));
    Fixture fx;
    const std::string name = std::string("t1080.") + codec_name(c);
    if (!fs::exists(g_scratch / name)) {
        if (!make_fixture(c, name, {{1920, 1080, 120}}, 120, fx)) return;
    } else {
        fx.packets = elementary::split(c, elementary::read_file((g_scratch / name).string()));
        for (uint32_t k = 0; k < fx.packets.size(); ++k) fx.expect.push_back({k, 1920, 1080, k == 0});
    }
    set_hardware(hardware);
    std::string err;
    auto d = create_decoder(c, &err);
    CHECK(d != nullptr);
    if (!d) return;
    std::vector<double> ms;
    DecodedFrame f;
    Quality q;
    for (size_t i = 0; i < fx.packets.size(); ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = d->decode(fx.packets[i], f, &err);
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        CHECK(ok && f.ready);
        if (i % 30 == 29) check_picture(f, fx.expect[i].n, 1920, 1080, q, kMinLuma, kMinRgb);
    }
    std::vector<double> rest(ms.begin() + 1, ms.end());
    std::sort(rest.begin(), rest.end());
    double sum = 0;
    for (double v : rest) sum += v;
    std::printf("   %s: first %.2f ms; then mean %.2f, p50 %.2f, p99 %.2f, max %.2f ms per frame (to CPU NV12)\n",
                d->describe().c_str(), ms[0], sum / double(rest.size()), rest[rest.size() / 2],
                rest[size_t(0.99 * double(rest.size() - 1))], rest.back());
}

// ---- the VA-API encoder's own output (recorded on the halo) ---------------------

void halo_fixture(Codec c, const fs::path& path) {
    check::phase(std::string(codec_name(c)) + ": VA-API stream " + path.filename().string());
    const auto bytes = elementary::read_file(path.string());
    CHECK(!bytes.empty());
    const auto packets = elementary::split(c, bytes);
    CHECK(packets.size() >= 10);
    std::string err;
    for (bool hw : {true, false}) {
        set_hardware(hw);
        auto d = create_decoder(c, &err);
        CHECK(d != nullptr);
        if (!d) return;
        Quality q;
        DecodedFrame f;
        int64_t last = -1;
        size_t good = 0;
        for (size_t i = 0; i < packets.size(); ++i) {
            const bool ok = d->decode(packets[i], f, &err);
            CHECK(ok && f.ready);
            if (!ok || !f.ready) {
                std::printf("   packet %zu: %s\n", i, err.c_str());
                continue;
            }
            const auto rgba = tools::to_rgba(f);
            const int64_t n = tools::read_test_pattern_counter(rgba.data(), f.width, f.height, f.width * 4);
            CHECK(n > last);  // the counter only moves forward (frames may be skipped, never repeated)
            last = n;
            if (check_picture(f, uint64_t(n), f.width, f.height, q, kMinLuma, kMinRgb)) ++good;
        }
        std::printf("   %s: %zu packets, %ux%u, %zu match the pattern; min luma PSNR %.1f dB, min RGB PSNR %.1f dB\n",
                    d->describe().c_str(), packets.size(), f.width, f.height, good, q.luma, q.rgb);
    }
}

}  // namespace

int main() {
    check::watchdog(600);
    g_scratch = fs::temp_directory_path() / ("broremote_test_mf_" + std::to_string(_getpid()));
    fs::create_directories(g_scratch);

    check::phase("probe");
    std::printf("   decoders:");
    for (Codec c : available_decoders()) std::printf(" %s", codec_name(c));
    std::printf("\n");
    CHECK(decodes(Codec::H264));  // every Windows since 7 has the H.264 MFT

    const bool ffmpeg = oracle::have_ffmpeg();
    if (!ffmpeg) std::printf("   ffmpeg is not on PATH: skipping the generated fixtures\n");
    for (Codec c : {Codec::H264, Codec::HEVC, Codec::AV1}) {
        if (!ffmpeg) break;
        if (!decodes(c)) {
            std::printf("-- %s: no decoder here, skipped\n", codec_name(c));
            continue;
        }
        Fixture fx;
        check::phase(std::string(codec_name(c)) + ": encoding fixtures with ffmpeg");
        if (!make_fixture(c, std::string("mixed.") + codec_name(c), {{640, 360, 30}, {718, 404, 20}}, 10, fx)) {
            continue;
        }
        decode_all(c, fx, true);
        decode_all(c, fx, false);
        start_late(c, fx);
        garbage(c, fx);
        if (c != Codec::AV1) {
            timing(c, true);
            timing(c, false);
        }
    }

    const fs::path fixtures = BROREMOTE_FIXTURE_DIR;
    for (Codec c : {Codec::H264, Codec::HEVC}) {
        const fs::path p = fixtures / (std::string("halo_vaapi.") + codec_name(c));
        if (!fs::exists(p)) {
            std::printf("-- no %s\n", p.string().c_str());
            continue;
        }
        if (!decodes(c)) {
            std::printf("-- %s: no decoder here, %s skipped\n", codec_name(c), p.filename().string().c_str());
            continue;
        }
        halo_fixture(c, p);
    }
    set_hardware(true);
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return check::finish();
}
