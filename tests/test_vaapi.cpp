// The VA-API encoder against ffmpeg as the oracle (Linux with a VA encoder;
// skipped otherwise). For every codec the machine reports:
//   - CPU frames: a 300-frame pattern sequence with forced keyframes, which
//     runs frame_num / POC past their wrap points; ffmpeg decodes the whole
//     stream and each keyframe-led segment alone, every picture is compared
//     with its source by PSNR, and the frame count must match.
//   - A size change (a new encoder, as the server makes), at a size that
//     needs cropping in both directions.
//   - dmabuf frames from GBM, each with a real sync_file acquire fence:
//     linear XRGB8888, ARGB8888 with GBM's pick of the primary plane's
//     modifiers (as a compositor allocates scanout buffers), and a short
//     run on every XRGB8888 modifier the plane offers.
//   - release is called exactly once per frame, before encode() returns, and
//     the frame's memory is scribbled over in the release callback, so any
//     read after release would show as a PSNR failure.
//   - A Server streaming to a Client over the local socket, decoded with
//     ffmpeg, including a keyframe request; and `broremote serve-test` in
//     its own process streaming to a Client here.
//   - Per-frame encode latency of dmabuf frames at 1920x1080 and 2560x1440.
#include "broremote/client.h"
#include "broremote/server.h"
#include "broremote/stream.h"
#include "check.h"
#include "ffmpeg_oracle.h"
#include "gbm_source.h"
#include "test_pattern.h"

#include <dirent.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <tuple>
#include <cstring>
#include <mutex>
#include <random>
#include <set>
#include <thread>

using namespace broremote;
using broremote::tools::draw_test_pattern;

namespace {

constexpr double kMinPsnr = 35.0;          // dB, luma, per picture, at the bitrates below
constexpr double kRgbBelowCeiling = 5.0;   // dB RGB PSNR may sit under the uncoded 4:2:0 ceiling
constexpr double kMaxBias = 1.0;           // code values of mean error per channel
constexpr int kSkip = 77;          // ctest SKIP_RETURN_CODE

bool g_ffmpeg = false;
std::string g_dir;

std::string path_for(const std::string& name) { return g_dir + "/" + name; }

std::vector<uint8_t> pattern(uint32_t w, uint32_t h, uint64_t n) {
    std::vector<uint8_t> px(size_t(w) * h * 4);
    draw_test_pattern(px.data(), w, h, n);
    return px;
}

std::vector<uint8_t> concat(const std::vector<EncodedPacket>& pkts, size_t from, size_t to) {
    std::vector<uint8_t> out;
    for (size_t i = from; i < to; ++i) out.insert(out.end(), pkts[i].data.begin(), pkts[i].data.end());
    return out;
}

struct Quality {
    double luma_min = 99, luma_mean = 0;  // decoded Y against the ideal BT.709 luma
    double rgb_min = 99, rgb_mean = 0;    // decoded RGB against the RGBA source
    double ceiling = 0;                   // RGB PSNR of an uncoded 4:2:0 round trip of the first source
    double bias_max = 0;                  // largest per-channel mean error (a range or transfer mistake)
};

// Decodes packets [from, to) and compares each picture with the pattern
// frame it was made from (`source[i]`).
//
// The bar is luma PSNR: the test pattern wraps every channel each 256 pixels,
// full-swing colour edges that 4:2:0 cannot carry, so RGB PSNR is capped near
// 33 dB before any coding (the ceiling, measured with ffmpeg's own
// conversion). RGB must stay within kRgbBelowCeiling of that ceiling, which
// catches a wrong matrix, range or channel order; luma must reach kMinPsnr.
Quality verify_range(Codec codec, const std::vector<EncodedPacket>& pkts, const std::vector<uint64_t>& source,
                     size_t from, size_t to, uint32_t w, uint32_t h, const std::string& name) {
    Quality q;
    const std::string path = path_for(name);
    oracle::write_file(path, concat(pkts, from, to));
    std::vector<std::vector<uint8_t>> rgb, luma;
    std::string log;
    CHECK(oracle::decode(path, codec, w, h, rgb, log));
    CHECK(oracle::decode(path, codec, w, h, luma, log, true));
    CHECK_EQ(rgb.size(), to - from);
    CHECK_EQ(luma.size(), to - from);
    CHECK_EQ(log, std::string());
    if (!log.empty()) std::printf("   ffmpeg (%s): %s\n", name.c_str(), log.substr(0, 600).c_str());
    const auto first = pattern(w, h, source[from]);
    const auto round = oracle::roundtrip_420(path + ".src", first, w, h);
    q.ceiling = round.empty() ? 0 : oracle::psnr(round.data(), first.data(), w, h);
    int reported = 0;
    const size_t n = std::min(rgb.size(), luma.size());
    for (size_t i = 0; i < n; ++i) {
        const auto src = pattern(w, h, source[from + i]);
        const double pr = oracle::psnr(rgb[i].data(), src.data(), w, h);
        const double py = oracle::psnr_luma(luma[i].data(), oracle::ideal_luma(src.data(), w, h));
        q.rgb_min = std::min(q.rgb_min, pr);
        q.luma_min = std::min(q.luma_min, py);
        q.rgb_mean += pr / double(n);
        q.luma_mean += py / double(n);
        for (double e : oracle::mean_error(rgb[i].data(), src.data(), w, h)) q.bias_max = std::max(q.bias_max, std::abs(e));
        if ((py < kMinPsnr || pr < q.ceiling - kRgbBelowCeiling) && ++reported <= 3) {
            std::printf("   %s: picture %zu (source %llu) luma %.2f dB, RGB %.2f dB, %s\n", name.c_str(), i,
                        static_cast<unsigned long long>(source[from + i]), py, pr,
                        oracle::channel_bias(rgb[i].data(), src.data(), w, h).c_str());
        }
    }
    CHECK(q.luma_min >= kMinPsnr);
    CHECK(q.ceiling > 0);
    CHECK(q.rgb_min >= q.ceiling - kRgbBelowCeiling);
    CHECK(q.bias_max <= kMaxBias);
    return q;
}

// The whole stream, then each keyframe-led segment on its own.
void verify_stream(Codec codec, const std::vector<EncodedPacket>& pkts, const std::vector<uint64_t>& source,
                   uint32_t w, uint32_t h, const std::string& name) {
    if (!g_ffmpeg) return;
    uint32_t pw = 0, ph = 0;
    oracle::write_file(path_for(name + ".probe"), concat(pkts, 0, std::min<size_t>(pkts.size(), 1)));
    CHECK(oracle::probe_size(path_for(name + ".probe"), codec, pw, ph));
    if (codec == Codec::AV1) {
        // AV1 (opt-in, BROREMOTE_VAAPI_AV1=1) cannot crop: the frame may be
        // padded, with the visible size in render_size (applied by decode()).
        CHECK(pw >= w && ph >= h);
        if (pw != w || ph != h) std::printf("   %s: AV1 frame %ux%u for a %ux%u picture\n", name.c_str(), pw, ph, w, h);
    } else {
        CHECK_EQ(pw, w);
        CHECK_EQ(ph, h);
    }
    const Quality q = verify_range(codec, pkts, source, 0, pkts.size(), w, h, name);
    std::printf("   %s: %zu pictures decoded; PSNR luma min %.2f mean %.2f dB; RGB min %.2f mean %.2f dB "
                "(uncoded 4:2:0 ceiling %.2f); max channel bias %.2f\n",
                name.c_str(), pkts.size(), q.luma_min, q.luma_mean, q.rgb_min, q.rgb_mean, q.ceiling, q.bias_max);
    std::vector<size_t> keys;
    for (size_t i = 0; i < pkts.size(); ++i) {
        if (pkts[i].keyframe) keys.push_back(i);
    }
    for (size_t k = 0; k < keys.size(); ++k) {
        const size_t end = k + 1 < keys.size() ? keys[k + 1] : pkts.size();
        verify_range(codec, pkts, source, keys[k], end, w, h, name + ".from" + std::to_string(keys[k]));
    }
    std::printf("   %s: %zu keyframe segments each decode alone\n", name.c_str(), keys.size());
}

// ---------------------------------------------------------------------------------

void test_cpu_sequence(Codec codec) {
    check::phase(std::string(codec_name(codec)) + ": CPU frames, forced keyframes, wraps");
    const uint32_t w = 1280, h = 720, n_frames = 300;
    const std::set<uint32_t> forced = {100, 217, 218};
    EncoderConfig ec;
    ec.width = w;
    ec.height = h;
    ec.bitrate_kbps = 12000;
    std::string err;
    auto enc = create_encoder(codec, ec, &err);
    CHECK(enc != nullptr);
    if (!enc) {
        std::printf("   create_encoder: %s\n", err.c_str());
        return;
    }
    std::vector<EncodedPacket> pkts;
    std::vector<uint64_t> source;
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (uint32_t n = 0; n < n_frames; ++n) {
        draw_test_pattern(px.data(), w, h, n);
        Frame f;
        f.width = w;
        f.height = h;
        f.cpu = px.data();
        f.pts_ns = int64_t(n) * 16666667;
        int releases = 0;
        // Scribble over the pixels on release: a read after it would show.
        const auto release = [&] {
            ++releases;
            std::memset(px.data(), 0x5a, px.size());
        };
        EncodedPacket pkt;
        const bool ok = enc->encode(f, forced.count(n) > 0, release, pkt, &err);
        CHECK(ok);
        CHECK_EQ(releases, 1);
        if (!ok) {
            std::printf("   frame %u: %s\n", n, err.c_str());
            return;
        }
        CHECK_EQ(pkt.keyframe, n == 0 || forced.count(n) > 0);
        CHECK(!pkt.data.empty());
        CHECK_EQ(pkt.pts_ns, f.pts_ns);
        pkts.push_back(std::move(pkt));
        source.push_back(n);
    }
    verify_stream(codec, pkts, source, w, h, std::string(codec_name(codec)) + "-cpu");
}

void test_size_change(Codec codec) {
    check::phase(std::string(codec_name(codec)) + ": size change (cropped size)");
    for (auto [w, h] : {std::pair<uint32_t, uint32_t>{640, 360}, {1366, 770}}) {
        EncoderConfig ec;
        ec.width = w;
        ec.height = h;
        ec.bitrate_kbps = 12000;
        std::string err;
        auto enc = create_encoder(codec, ec, &err);
        CHECK(enc != nullptr);
        if (!enc) return;
        std::vector<EncodedPacket> pkts;
        std::vector<uint64_t> source;
        for (uint32_t n = 0; n < 40; ++n) {
            auto px = pattern(w, h, n + 1000);
            Frame f;
            f.width = w;
            f.height = h;
            f.cpu = px.data();
            int releases = 0;
            EncodedPacket pkt;
            CHECK(enc->encode(f, n == 20, [&] { ++releases; }, pkt, &err));
            CHECK_EQ(releases, 1);
            pkts.push_back(std::move(pkt));
            source.push_back(n + 1000);
        }
        verify_stream(codec, pkts, source, w, h,
                      std::string(codec_name(codec)) + "-" + std::to_string(w) + "x" + std::to_string(h));
    }
}

std::string render_node() {
    if (const char* forced = std::getenv("BROREMOTE_VAAPI_DEVICE"); forced && *forced) return forced;
    std::vector<std::string> nodes;
    if (DIR* d = opendir("/dev/dri")) {
        while (dirent* e = readdir(d)) {
            if (std::strncmp(e->d_name, "renderD", 7) == 0) nodes.push_back(std::string("/dev/dri/") + e->d_name);
        }
        closedir(d);
    }
    std::sort(nodes.begin(), nodes.end());
    return nodes.empty() ? std::string() : nodes.front();
}

struct Latency {
    double mean = 0, p99 = 0, max = 0;
};

Latency summarize(std::vector<double> ms) {
    Latency l;
    if (ms.empty()) return l;
    std::sort(ms.begin(), ms.end());
    for (double v : ms) l.mean += v;
    l.mean /= double(ms.size());
    l.p99 = ms[size_t(0.99 * double(ms.size() - 1))];
    l.max = ms.back();
    return l;
}

// Encodes `n_frames` dmabuf frames from a pool of three GBM buffers.
// Returns false when the buffers cannot be allocated (reported).
// `modifiers` empty: linear.
bool run_dmabuf(Codec codec, testkit::GbmDevice& gbm, uint32_t w, uint32_t h, uint32_t format,
                const std::vector<uint64_t>& modifiers, uint32_t n_frames, uint32_t bitrate_kbps, bool verify,
                const std::string& name, Latency* lat) {
    std::string err;
    std::unique_ptr<testkit::GbmBuffer> bufs[3];
    for (auto& b : bufs) {
        b = std::make_unique<testkit::GbmBuffer>();
        if (!b->alloc(gbm.get(), w, h, format, modifiers, &err)) {
            // GBM declining one modifier on its own means a compositor on
            // this GPU cannot render to it either: nothing to encode.
            const bool single = modifiers.size() == 1 && modifiers[0] != DRM_FORMAT_MOD_LINEAR;
            std::printf("   %s: %s%s\n", name.c_str(), err.c_str(), single ? " (GBM cannot allocate it: skipped)" : "");
            CHECK(single);
            return false;
        }
    }
    std::printf("   %s: modifier 0x%016llx, %u plane(s)\n", name.c_str(),
                static_cast<unsigned long long>(bufs[0]->frame.modifier), bufs[0]->frame.plane_count);
    CHECK(bufs[0]->frame.modifier != DRM_FORMAT_MOD_INVALID);
    EncoderConfig ec;
    ec.width = w;
    ec.height = h;
    ec.bitrate_kbps = bitrate_kbps;
    auto enc = create_encoder(codec, ec, &err);
    CHECK(enc != nullptr);
    if (!enc) return false;
    std::vector<EncodedPacket> pkts;
    std::vector<uint64_t> source;
    std::vector<double> ms;
    const auto scribble = pattern(w, h, 999999);
    int fences = 0;
    for (uint32_t n = 0; n < n_frames; ++n) {
        testkit::GbmBuffer& b = *bufs[n % 3];
        CHECK(b.fill(pattern(w, h, n).data()));
        Frame f = b.frame;
        f.acquire_fence_fd = b.export_fence();
        fences += f.acquire_fence_fd >= 0;
        f.pts_ns = int64_t(n) * 16666667;
        int releases = 0;
        const auto release = [&] {
            ++releases;
            if (verify) b.fill(scribble.data());  // the host reuses the buffer at once
        };
        EncodedPacket pkt;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = enc->encode(f, verify && n == n_frames / 2, release, pkt, &err);
        const double t = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (n > 0) ms.push_back(t);  // the first frame opens the device
        if (f.acquire_fence_fd >= 0) ::close(f.acquire_fence_fd);
        CHECK(ok);
        CHECK_EQ(releases, 1);
        if (!ok) {
            std::printf("   %s frame %u: %s\n", name.c_str(), n, err.c_str());
            return false;
        }
        pkts.push_back(std::move(pkt));
        source.push_back(n);
    }
    CHECK(fences > 0);
    if (lat) *lat = summarize(ms);
    if (verify) verify_stream(codec, pkts, source, w, h, name);
    return true;
}

void test_dmabuf(Codec codec) {
    check::phase(std::string(codec_name(codec)) + ": dmabuf frames (GBM)");
    testkit::GbmDevice gbm;
    const std::string node = render_node();
    if (node.empty() || !gbm.open(node)) {
        std::printf("   cannot open a GBM device on %s\n", node.c_str());
        CHECK(false);
        return;
    }
    const std::string c = codec_name(codec);
    run_dmabuf(codec, gbm, 1280, 720, DRM_FORMAT_XRGB8888, {}, 90, 12000, true, c + "-dmabuf-linear-xrgb", nullptr);
    const auto argb = testkit::scanout_modifiers(DRM_FORMAT_ARGB8888);
    const auto xrgb = testkit::scanout_modifiers(DRM_FORMAT_XRGB8888);
    if (argb.empty() || xrgb.empty()) {
        std::printf("   NOTE: no KMS plane modifiers readable (no /dev/dri/card*): tiled buffers not tested\n");
        return;
    }
    // GBM's pick, as a compositor allocates its scanout buffers.
    run_dmabuf(codec, gbm, 1280, 720, DRM_FORMAT_ARGB8888, argb, 90, 12000, true, c + "-dmabuf-scanout-argb", nullptr);
    // And each modifier the plane offers on its own.
    std::printf("   %zu XRGB8888 scanout modifiers\n", xrgb.size());
    for (uint64_t mod : xrgb) {
        if (mod == DRM_FORMAT_MOD_INVALID) continue;
        char hex[24];
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(mod));
        run_dmabuf(codec, gbm, 640, 360, DRM_FORMAT_XRGB8888, {mod}, 12, 8000, true, c + "-mod-" + hex, nullptr);
    }
}

void test_latency(Codec codec) {
    check::phase(std::string(codec_name(codec)) + ": dmabuf encode latency");
    testkit::GbmDevice gbm;
    if (!gbm.open(render_node())) {
        CHECK(false);
        return;
    }
    for (auto [w, h, kbps] : {std::tuple<uint32_t, uint32_t, uint32_t>{1920, 1080, 20000}, {2560, 1440, 30000}}) {
        Latency lat;
        const std::string name = std::string(codec_name(codec)) + "-" + std::to_string(w) + "x" + std::to_string(h);
        if (!run_dmabuf(codec, gbm, w, h, DRM_FORMAT_XRGB8888, testkit::scanout_modifiers(DRM_FORMAT_XRGB8888), 240,
                        kbps, false, name, &lat)) {
            continue;
        }
        std::printf("   LATENCY %s scanout dmabuf -> packet: mean %.2f ms, p99 %.2f ms, max %.2f ms\n", name.c_str(),
                    lat.mean, lat.p99, lat.max);
        CHECK(lat.mean < 16.6);  // well inside a 60 Hz frame
    }
}

std::string unique_socket(const char* what) {
    static std::random_device rd;
    return std::string("t-vaapi-") + what + "-" + std::to_string(rd() % 1000000);
}

// A viewer that keeps every packet and acks it (as a decoding viewer would).
class Receiver {
public:
    bool connect(std::unique_ptr<Stream> stream) {
        ClientHandlers hd;
        hd.on_config = [this](const StreamConfig& sc) {
            std::lock_guard<std::mutex> lk(m_);
            configs_.push_back(sc);
        };
        hd.on_video = [this](const VideoPacket& v) {
            Client* c = nullptr;
            {
                std::lock_guard<std::mutex> lk(m_);
                EncodedPacket p;
                p.data = v.data;
                p.keyframe = v.keyframe;
                p.pts_ns = v.pts_ns;
                packets_.push_back(std::move(p));
                c = client_;
            }
            if (c) c->ack(v.frame_id);
        };
        std::string err;
        owned_ = Client::connect(std::move(stream), std::move(hd), ClientOptions{}, &err);
        if (!owned_) {
            std::printf("   Client::connect: %s\n", err.c_str());
            return false;
        }
        std::lock_guard<std::mutex> lk(m_);
        client_ = owned_.get();
        return true;
    }
    Client& client() { return *owned_; }
    size_t count() {
        std::lock_guard<std::mutex> lk(m_);
        return packets_.size();
    }
    // Closes the client and returns what it received.
    std::vector<EncodedPacket> finish(std::vector<StreamConfig>* configs) {
        {
            std::lock_guard<std::mutex> lk(m_);
            client_ = nullptr;
        }
        owned_.reset();
        std::lock_guard<std::mutex> lk(m_);
        if (configs) *configs = configs_;
        return std::move(packets_);
    }

private:
    std::mutex m_;
    std::vector<EncodedPacket> packets_;
    std::vector<StreamConfig> configs_;
    Client* client_ = nullptr;
    std::unique_ptr<Client> owned_;
};

// The pattern counter of each decoded picture (the test pattern draws it).
std::vector<int64_t> decoded_counters(Codec codec, const std::string& name, uint32_t w, uint32_t h) {
    std::vector<std::vector<uint8_t>> frames;
    std::string log;
    oracle::decode(path_for(name), codec, w, h, frames, log);
    std::vector<int64_t> out;
    for (const auto& f : frames) {
        const auto rgba = oracle::to_rgba(f);
        out.push_back(broremote::tools::read_test_pattern_counter(rgba.data(), w, h, w * 4));
    }
    return out;
}

// A Server in this process, fed CPU frames one at a time, streams to a Client
// over the local socket; the client requests a keyframe halfway.
void test_server(Codec codec) {
    check::phase(std::string(codec_name(codec)) + ": server to client, decoded by ffmpeg");
    const uint32_t w = 960, h = 540, n_frames = 60;
    ServerConfig cfg;
    cfg.socket_name = unique_socket("server");
    cfg.codecs = {codec};
    cfg.bitrate_kbps = 8000;
    std::string err;
    auto server = Server::create(cfg, &err);
    CHECK(server != nullptr);
    if (!server) {
        std::printf("   Server::create: %s\n", err.c_str());
        return;
    }
    Receiver rx;
    CHECK(rx.connect(connect_local(cfg.socket_name, &err)));
    WAIT(server->wants_frames(), 5000);
    std::vector<std::vector<uint8_t>> pool(n_frames);
    std::atomic<int> releases{0};
    for (uint32_t n = 0; n < n_frames; ++n) {
        if (n == 30) {
            rx.client().request_keyframe();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        pool[n] = pattern(w, h, n);
        Frame f;
        f.width = w;
        f.height = h;
        f.cpu = pool[n].data();
        server->submit(f, [&] { ++releases; });
        WAIT(rx.count() == n + 1, 5000);
    }
    CHECK_EQ(releases.load(), int(n_frames));
    std::vector<StreamConfig> configs;
    auto pkts = rx.finish(&configs);
    server.reset();
    CHECK_EQ(configs.size(), size_t(1));
    CHECK_EQ(pkts.size(), size_t(n_frames));
    if (pkts.size() != n_frames) return;
    CHECK(pkts[0].keyframe);
    CHECK(pkts[30].keyframe);
    std::vector<uint64_t> source(n_frames);
    for (uint32_t i = 0; i < n_frames; ++i) source[i] = i;
    const std::string name = std::string(codec_name(codec)) + "-server";
    verify_stream(codec, pkts, source, w, h, name);
    if (!g_ffmpeg) return;
    const auto counters = decoded_counters(codec, name, w, h);
    for (size_t i = 0; i < counters.size(); ++i) CHECK_EQ(counters[i], int64_t(i));
}

// `broremote serve-test --codec C` in its own process, free-running at 60
// fps, to a Client here. The pattern's counter says which source frame each
// decoded picture is (the server may replace frames while the client acks),
// so each is compared with that frame.
void test_serve_test(Codec codec, const std::string& cli) {
    check::phase(std::string(codec_name(codec)) + ": broremote serve-test to a client, decoded by ffmpeg");
    const uint32_t w = 1280, h = 720, n_packets = 180;
    const std::string sock = unique_socket("servetest");
    std::string err;
    auto proc = Process::spawn({cli, "serve-test", "--socket", sock, "--codec", codec_name(codec), "--size", "1280x720",
                                "--fps", "60", "--seconds", "60"},
                               &err);
    CHECK(proc != nullptr);
    if (!proc) {
        std::printf("   spawn: %s\n", err.c_str());
        return;
    }
    std::unique_ptr<Stream> s;
    CHECK(check::wait_for(
        [&] {
            s = connect_local(sock);
            return s != nullptr;
        },
        10000));
    Receiver rx;
    if (!s || !rx.connect(std::move(s))) {
        CHECK(false);
        proc->kill();
        return;
    }
    WAIT(rx.count() >= n_packets / 2, 20000);
    rx.client().request_keyframe();
    WAIT(rx.count() >= n_packets, 20000);
    std::vector<StreamConfig> configs;
    auto pkts = rx.finish(&configs);
    proc->kill();
    proc->wait_for(std::chrono::seconds(10));
    CHECK_EQ(configs.size(), size_t(1));
    if (!configs.empty()) {
        CHECK(configs[0].codec == codec);
        CHECK_EQ(configs[0].width, w);
        CHECK_EQ(configs[0].height, h);
    }
    CHECK(!pkts.empty() && pkts[0].keyframe);
    size_t keys = 0;
    for (const auto& p : pkts) keys += p.keyframe;
    CHECK(keys >= 2);  // the first, and the one requested
    if (!g_ffmpeg || pkts.empty()) return;
    const std::string name = std::string(codec_name(codec)) + "-servetest";
    oracle::write_file(path_for(name), concat(pkts, 0, pkts.size()));
    const auto counters = decoded_counters(codec, name, w, h);
    CHECK_EQ(counters.size(), pkts.size());
    std::vector<uint64_t> source;
    bool rising = true;
    for (size_t i = 0; i < counters.size(); ++i) {
        rising = rising && counters[i] >= 0 && (i == 0 || counters[i] > counters[i - 1]);
        source.push_back(uint64_t(std::max<int64_t>(0, counters[i])));
    }
    CHECK(rising);
    if (counters.size() == pkts.size() && rising) verify_stream(codec, pkts, source, w, h, name);
    std::printf("   %s: %zu packets, %zu keyframes, source frames %lld..%lld\n", name.c_str(), pkts.size(), keys,
                counters.empty() ? -1ll : static_cast<long long>(counters.front()),
                counters.empty() ? -1ll : static_cast<long long>(counters.back()));
}

void remove_dir(const std::string& dir) {
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] != '.') std::remove((dir + "/" + e->d_name).c_str());
        }
        closedir(d);
    }
    ::rmdir(dir.c_str());
}

}  // namespace

// test_vaapi [path to the broremote executable, for the serve-test phase]
int main(int argc, char** argv) {
    check::watchdog(900);
    const std::string cli = argc > 1 ? argv[1] : "";
    std::vector<Codec> codecs;
    // $BROREMOTE_TEST_CODEC narrows the run to one codec.
    const char* only = std::getenv("BROREMOTE_TEST_CODEC");
    for (Codec c : available_encoders()) {
        if (c != Codec::Raw && (!only || !*only || parse_codec(only) == c)) codecs.push_back(c);
    }
    if (codecs.empty()) {
        std::printf("SKIP: no VA-API encoder on this machine (broremote encode lists only raw)\n");
        return kSkip;
    }
    g_ffmpeg = oracle::have_ffmpeg();
    if (!g_ffmpeg) std::printf("NOTE: ffmpeg is not on PATH: bitstreams are encoded but not decoded or compared\n");
    char tmpl[] = "/tmp/broremote-vaapi-XXXXXX";
    if (!mkdtemp(tmpl)) {
        std::printf("FAIL: mkdtemp\n");
        return 1;
    }
    g_dir = tmpl;
    std::printf("codecs:");
    for (Codec c : codecs) std::printf(" %s", codec_name(c));
    std::printf("; work dir %s\n", g_dir.c_str());

    for (Codec c : codecs) {
        test_cpu_sequence(c);
        test_size_change(c);
        test_dmabuf(c);
        test_server(c);
        if (!cli.empty()) test_serve_test(c, cli);
        test_latency(c);
    }
    const int rc = check::finish();
    if (rc == 0) remove_dir(g_dir);
    else std::printf("bitstreams kept in %s\n", g_dir.c_str());
    return rc;
}
