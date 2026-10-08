// The VA-API encoder: the common half (see va_codec.h for the split). One
// VADisplay per encoder, opened on the first frame so a dmabuf frame can pick
// the render node of the device that exported it.
//
// Per frame: wait on the acquire fence, import the dmabuf (or upload the CPU
// frame), convert it with VideoProc into the NV12 input surface, wait for that
// copy, release the frame, then encode and read the coded buffer.
#include "codec_backends.h"
#include "vaapi/va_codec.h"
#include "vaapi/va_source.h"
#include "vaapi/va_vpp.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>

namespace broremote::vaapi {

// AV1 has no cropping: radeonsi encodes the frame at its surface alignment
// (64x16; 1080 lines become 1082) and keeps the visible size only as
// render_size, so a decoder crops to the StreamConfig size. See docs/design.md.
const std::vector<Codec>& supported_codecs() {
    static const std::vector<Codec> codecs{Codec::H264, Codec::HEVC, Codec::AV1};
    return codecs;
}

std::vector<VAProfile> codec_profiles(Codec codec) {
    switch (codec) {
        case Codec::H264: return {VAProfileH264High, VAProfileH264Main, VAProfileH264ConstrainedBaseline};
        case Codec::HEVC: return {VAProfileHEVCMain};
        case Codec::AV1: return {VAProfileAV1Profile0};
        default: return {};
    }
}

uint32_t codec_alignment(Codec codec) {
    switch (codec) {
        case Codec::AV1: return 64;  // whole superblocks
        default: return 16;          // macroblocks; HEVC's minimum coding block is 8
    }
}

namespace {

constexpr int kFenceTimeoutMs = 1000;

uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

// Calls the release callback exactly once: when asked, or on destruction.
class ReleaseGuard {
public:
    explicit ReleaseGuard(const std::function<void()>& f) : f_(f) {}
    ~ReleaseGuard() { run(); }
    void run() {
        if (!done_) {
            done_ = true;
            if (f_) f_();
        }
    }

private:
    const std::function<void()>& f_;
    bool done_ = false;
};

class VaEncoder final : public Encoder {
public:
    VaEncoder(Codec codec, const EncoderConfig& cfg) : codec_(codec), cfg_(cfg) {}
    ~VaEncoder() override { close_session(); }

    bool encode(const Frame& frame, bool force_keyframe, const std::function<void()>& release, EncodedPacket& out,
                std::string* err) override {
        ReleaseGuard guard(release);
        out.data.clear();
        out.keyframe = false;
        out.pts_ns = frame.pts_ns;
        if (frame.width == 0 || frame.height == 0) return fail(err, "frame is empty");
        if (!display_ && !open_session(frame, err)) return false;

        // The source surface, then the conversion into the encoder's input.
        VASurfaceID src = VA_INVALID_SURFACE;
        ImportedSurface imported;
        if (frame.is_cpu()) {
            if (!upload_.upload(display_->get(), frame, err)) return false;
            guard.run();  // the pixels are in the upload surface now
            src = upload_.id();
        } else {
            if (!wait_fence(frame.acquire_fence_fd, kFenceTimeoutMs, err)) return false;
            if (!imported.import(display_->get(), frame, err)) return false;
            src = imported.id();
        }
        if (!vpp_.convert(src, frame.width, frame.height, input_, sp_.width, sp_.height, err)) return false;
        imported.reset();
        guard.run();

        const bool keyframe = force_keyframe || need_keyframe_;
        if (!encode_picture(keyframe, out, err)) {
            need_keyframe_ = true;  // the reference state is unknown now
            return false;
        }
        return true;
    }

private:
    static bool fail(std::string* err, const std::string& what) {
        if (err) *err = what;
        return false;
    }

    const NodeCaps* pick_node(const Frame& frame) const {
        const auto& nodes = probe_nodes();
        const NodeCaps* first = nullptr;
        for (const NodeCaps& n : nodes) {
            if (n.find(codec_) && !first) first = &n;
        }
        if (!frame.is_cpu() && frame.planes[0].fd >= 0) {
            const std::string exporter = dmabuf_exporter(frame.planes[0].fd);
            for (const NodeCaps& n : nodes) {
                if (n.find(codec_) && !exporter.empty() && n.driver == exporter) return &n;
            }
        }
        return first;
    }

    bool open_session(const Frame& frame, std::string* err) {
        const NodeCaps* node = pick_node(frame);
        if (!node) return fail(err, std::string("no VA device can encode ") + codec_name(codec_));
        const CodecCaps& caps = *node->find(codec_);
        const uint32_t align = codec_alignment(codec_);
        sp_.width = align_up(cfg_.width, 2);
        sp_.height = align_up(cfg_.height, 2);
        sp_.coded_width = align_up(sp_.width, std::max(align, caps.align_width));
        sp_.coded_height = align_up(sp_.height, std::max(align, caps.align_height));
        sp_.fps = cfg_.fps ? cfg_.fps : 60;
        sp_.bitrate_bps = uint32_t(std::min<uint64_t>(uint64_t(cfg_.bitrate_kbps) * 1000, 0xffffffffu));
        sp_.caps = caps;
        if ((caps.max_width && sp_.coded_width > caps.max_width) ||
            (caps.max_height && sp_.coded_height > caps.max_height)) {
            return fail(err, std::to_string(cfg_.width) + "x" + std::to_string(cfg_.height) + " exceeds the " +
                                 codec_name(codec_) + " encoder's maximum " + std::to_string(caps.max_width) + "x" +
                                 std::to_string(caps.max_height));
        }
        switch (codec_) {
            case Codec::H264: impl_ = make_h264(sp_); break;
            case Codec::HEVC: impl_ = make_hevc(sp_); break;
            case Codec::AV1: impl_ = make_av1(sp_); break;
            default: return fail(err, std::string(codec_name(codec_)) + " is not implemented by the VA-API backend");
        }

        display_ = Display::open(node->path, err);
        if (!display_) return false;
        VADisplay dpy = display_->get();
        debug_log("%s encoder on %s (%s): %ux%u coded %ux%u, %u fps, %u bps", codec_name(codec_), node->path.c_str(),
                  display_->vendor().c_str(), sp_.width, sp_.height, sp_.coded_width, sp_.coded_height, sp_.fps,
                  sp_.bitrate_bps);

        std::vector<VAConfigAttrib> attrs = {{VAConfigAttribRTFormat, VA_RT_FORMAT_YUV420},
                                             {VAConfigAttribRateControl, VA_RC_CBR}};
        if (caps.packed_headers) attrs.push_back({VAConfigAttribEncPackedHeaders, impl_->packed_headers()});
        if (!va_check(vaCreateConfig(dpy, caps.profile, caps.entrypoint, attrs.data(), int(attrs.size()), &config_),
                      "vaCreateConfig (encode)", err)) {
            return close_session(), false;
        }
        VASurfaceAttrib fmt{};
        fmt.type = VASurfaceAttribPixelFormat;
        fmt.flags = VA_SURFACE_ATTRIB_SETTABLE;
        fmt.value.type = VAGenericValueTypeInteger;
        fmt.value.value.i = VA_FOURCC_NV12;
        VASurfaceID surfaces[3];
        if (!va_check(vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, sp_.coded_width, sp_.coded_height, surfaces, 3, &fmt, 1),
                      "vaCreateSurfaces (NV12)", err)) {
            return close_session(), false;
        }
        input_ = surfaces[0];
        recon_[0] = surfaces[1];
        recon_[1] = surfaces[2];
        if (!va_check(vaCreateContext(dpy, config_, int(sp_.coded_width), int(sp_.coded_height), VA_PROGRESSIVE,
                                      surfaces, 3, &context_),
                      "vaCreateContext (encode)", err)) {
            return close_session(), false;
        }
        // A coded picture is far below the raw size at any sane bitrate;
        // the raw size bounds even a worst-case keyframe.
        const uint32_t coded_size = sp_.coded_width * sp_.coded_height * 3 / 2 + (1u << 16);
        if (!va_check(vaCreateBuffer(dpy, context_, VAEncCodedBufferType, coded_size, 1, nullptr, &coded_),
                      "vaCreateBuffer (coded)", err)) {
            return close_session(), false;
        }
        if (!vpp_.init(dpy, sp_.coded_width, sp_.coded_height, err)) return close_session(), false;
        need_keyframe_ = true;
        return true;
    }

    void close_session() {
        if (!display_) return;
        VADisplay dpy = display_->get();
        vpp_.reset();
        upload_.reset();
        if (coded_ != VA_INVALID_ID) vaDestroyBuffer(dpy, coded_);
        if (context_ != VA_INVALID_ID) vaDestroyContext(dpy, context_);
        VASurfaceID surfaces[3] = {input_, recon_[0], recon_[1]};
        for (VASurfaceID s : surfaces) {
            if (s != VA_INVALID_SURFACE) vaDestroySurfaces(dpy, &s, 1);
        }
        if (config_ != VA_INVALID_ID) vaDestroyConfig(dpy, config_);
        coded_ = VA_INVALID_ID;
        context_ = VA_INVALID_ID;
        config_ = VA_INVALID_ID;
        input_ = recon_[0] = recon_[1] = VA_INVALID_SURFACE;
        impl_.reset();
        display_.reset();
    }

    bool add_rate_control(BufferList& bufs, std::string* err) const {
        VAEncMiscParameterRateControl rc{};
        rc.bits_per_second = sp_.bitrate_bps;
        rc.target_percentage = 100;
        rc.window_size = 1000;
        // No filler: padding a quiet desktop up to the bitrate is wasted
        // bandwidth, and on VCN 4 firmware (ENC 1.24) an AV1 frame that needs
        // more than about 35 KB of padding OBU hangs the encode ring.
        rc.rc_flags.bits.disable_bit_stuffing = 1;
        VAEncMiscParameterHRD hrd{};
        // 50 ms of buffer (three frames at 60 fps): what a frame may borrow
        // from the ones after it. A big change (a window opening, a scene
        // switch) is one packet that must cross the link before it can be
        // shown, so the buffer bounds that spike: with half a second,
        // desktop-like content at 20 Mbit/s made packets of up to 450 kB
        // that took 4-5 ms to arrive; with 50 ms they stay near 110 kB
        // (1.5 ms), mean latency unchanged, at the cost of a softer first
        // frame after the change that sharpens over the next few.
        hrd.buffer_size = uint32_t(uint64_t(sp_.bitrate_bps) * 50 / 1000);
        hrd.initial_buffer_fullness = hrd.buffer_size / 2;
        VAEncMiscParameterFrameRate fr{};
        fr.framerate = sp_.fps;
        return bufs.add_misc(VAEncMiscParameterTypeRateControl, rc, err) &&
               bufs.add_misc(VAEncMiscParameterTypeHRD, hrd, err) &&
               bufs.add_misc(VAEncMiscParameterTypeFrameRate, fr, err);
    }

    bool encode_picture(bool keyframe, EncodedPacket& out, std::string* err) {
        VADisplay dpy = display_->get();
        if (keyframe) {
            index_in_gop_ = 0;
        }
        PictureParams pic;
        pic.keyframe = keyframe;
        pic.index_in_gop = index_in_gop_;
        pic.keyframe_count = keyframe_count_;
        pic.recon = recon_[current_];
        pic.ref = keyframe ? VA_INVALID_SURFACE : recon_[current_ ^ 1];
        pic.coded = coded_;

        if (!va_check(vaBeginPicture(dpy, context_, input_), "vaBeginPicture (encode)", err)) return false;
        BufferList bufs(dpy, context_);
        bool ok = true;
        if (keyframe) ok = impl_->add_sequence(bufs, pic, err) && add_rate_control(bufs, err);
        ok = ok && impl_->add_picture(bufs, pic, err) && bufs.render(err);
        const VAStatus end = vaEndPicture(dpy, context_);
        if (ok) ok = va_check(end, "vaEndPicture (encode)", err);
        bufs.clear();
        if (!ok) return false;
        if (!va_check(vaSyncSurface(dpy, input_), "vaSyncSurface (encode)", err)) return false;

        void* map = nullptr;
        if (!va_check(vaMapBuffer(dpy, coded_, &map), "vaMapBuffer (coded)", err)) return false;
        for (auto* seg = static_cast<VACodedBufferSegment*>(map); seg; seg = static_cast<VACodedBufferSegment*>(seg->next)) {
            if (seg->status & VA_CODED_BUF_STATUS_SLICE_OVERFLOW_MASK) {
                vaUnmapBuffer(dpy, coded_);
                return fail(err, "coded buffer overflow");
            }
            const auto* bytes = static_cast<const uint8_t*>(seg->buf);
            out.data.insert(out.data.end(), bytes, bytes + seg->size);
        }
        vaUnmapBuffer(dpy, coded_);
        if (out.data.empty()) return fail(err, "the encoder produced no data");
        impl_->finish_packet(out.data);

        out.keyframe = keyframe;
        if (keyframe) ++keyframe_count_;
        need_keyframe_ = false;
        ++index_in_gop_;
        current_ ^= 1;
        return true;
    }

    Codec codec_;
    EncoderConfig cfg_;
    StreamParams sp_;
    std::unique_ptr<CodecImpl> impl_;
    std::unique_ptr<Display> display_;
    VAConfigID config_ = VA_INVALID_ID;
    VAContextID context_ = VA_INVALID_ID;
    VASurfaceID input_ = VA_INVALID_SURFACE;
    VASurfaceID recon_[2] = {VA_INVALID_SURFACE, VA_INVALID_SURFACE};
    VABufferID coded_ = VA_INVALID_ID;
    Converter vpp_;
    UploadSurface upload_;
    bool need_keyframe_ = true;
    uint64_t index_in_gop_ = 0;
    uint64_t keyframe_count_ = 0;
    unsigned current_ = 0;
};

}  // namespace
}  // namespace broremote::vaapi

namespace broremote::detail {

std::vector<Codec> vaapi_encoders() {
    std::vector<Codec> out;
    for (Codec c : vaapi::supported_codecs()) {
        for (const vaapi::NodeCaps& n : vaapi::probe_nodes()) {
            if (n.find(c)) {
                out.push_back(c);
                break;
            }
        }
    }
    return out;
}

std::unique_ptr<Encoder> create_vaapi_encoder(Codec codec, const EncoderConfig& config, std::string* err) {
    bool found = false;
    for (const vaapi::NodeCaps& n : vaapi::probe_nodes()) found = found || n.find(codec);
    if (!found) {
        if (err) *err = std::string("no VA device can encode ") + codec_name(codec);
        return nullptr;
    }
    return std::make_unique<vaapi::VaEncoder>(codec, config);
}

}  // namespace broremote::detail
