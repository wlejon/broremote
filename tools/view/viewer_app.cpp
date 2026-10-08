#include "viewer_app.h"

#include "picture.h"
#include "png.h"
#include "test_pattern.h"

#include <algorithm>
#include <cstdio>

namespace broremote::view {

namespace {

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[size_t(p * double(v.size() - 1))];
}

double mean(const std::vector<double>& v) {
    double s = 0;
    for (double x : v) s += x;
    return v.empty() ? 0 : s / double(v.size());
}

}  // namespace

Viewer::Viewer(ViewerOptions options) : opt_(std::move(options)) {}

Viewer::~Viewer() {
    session_.reset();  // joins its threads; their wake events go nowhere after this
    if (texture_) SDL_DestroyTexture(texture_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

bool Viewer::init(std::string* err) {
    SDL_SetHint(SDL_HINT_APP_NAME, "broremote-view");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        if (err) *err = std::string("SDL_Init: ") + SDL_GetError();
        return false;
    }
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (opt_.hidden) flags |= SDL_WINDOW_HIDDEN;
    title_ = "broremote-view - connecting to " + opt_.session.target.describe() + "...";
    window_ = SDL_CreateWindow(title_.c_str(), opt_.width > 0 ? opt_.width : 1280,
                               opt_.height > 0 ? opt_.height : 720, flags);
    if (!window_) {
        if (err) *err = std::string("SDL_CreateWindow: ") + SDL_GetError();
        return false;
    }
    fitted_ = opt_.width > 0 && opt_.height > 0;
    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (!renderer_) {
        if (err) *err = std::string("SDL_CreateRenderer: ") + SDL_GetError();
        return false;
    }
    SDL_SetRenderVSync(renderer_, opt_.vsync ? 1 : SDL_RENDERER_VSYNC_DISABLED);
    if (opt_.fullscreen) toggle_fullscreen();
    wake_type_ = SDL_RegisterEvents(1);
    session_ = std::make_unique<Session>([this] {
        // One wake event in the queue at a time is enough.
        if (!wake_pending_.exchange(true)) {
            SDL_Event e{};
            e.type = wake_type_;
            SDL_PushEvent(&e);
        }
    });
    start_ = title_time_ = Clock::now();
    session_->start(opt_.session);
    return true;
}

ViewGeometry Viewer::geometry() const {
    ViewGeometry g;
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    g.window_w = float(w);
    g.window_h = float(h);
    // The picture on screen defines the mapping (the stream's size once a
    // picture of it is shown).
    g.stream_w = have_frame_ ? frame_.width : 0;
    g.stream_h = have_frame_ ? frame_.height : 0;
    return g;
}

void Viewer::toggle_fullscreen() {
    const bool fs = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) == 0;
    SDL_SetWindowFullscreen(window_, fs);
    // In fullscreen the system's own shortcuts (Alt+Tab, the Windows key)
    // go to the remote session too.
    SDL_SetWindowKeyboardGrab(window_, fs);
    need_draw_ = true;
}

void Viewer::fit_window_to_stream(uint32_t w, uint32_t h) {
    fitted_ = true;
    if (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) return;
    SDL_Rect usable{};
    if (!SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window_), &usable) || usable.w <= 0) return;
    // The stream at 1:1 in window units when it fits, else scaled to 90% of
    // the usable area.
    const float scale = std::min({1.0f, 0.9f * float(usable.w) / float(w), 0.9f * float(usable.h) / float(h)});
    SDL_SetWindowSize(window_, int(float(w) * scale), int(float(h) * scale));
    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

void Viewer::handle(const SDL_Event& e) {
    if (e.type == wake_type_) {
        wake_pending_ = false;
        return;
    }
    switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            quit_ = true;
            session_->close();
            return;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_EXPOSED:
            need_draw_ = true;
            break;
        case SDL_EVENT_KEY_DOWN:
            // Ctrl+Alt+Enter toggles fullscreen and is not sent.
            if (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_CTRL) && (e.key.mod & SDL_KMOD_ALT)) {
                if (!e.key.repeat) toggle_fullscreen();
                swallow_enter_ = true;
                return;
            }
            break;
        case SDL_EVENT_KEY_UP:
            if (swallow_enter_ && e.key.scancode == SDL_SCANCODE_RETURN) {
                swallow_enter_ = false;
                return;
            }
            break;
        default: break;
    }
    events_.clear();
    mapper_.handle(e, geometry(), events_);
    for (const InputEvent& ie : events_) session_->send_input(ie);
}

void Viewer::upload() {
    const bool nv12 = frame_.format == PixelFormat::NV12;
    if (!texture_ || tex_w_ != frame_.width || tex_h_ != frame_.height || tex_format_ != frame_.format) {
        if (texture_) SDL_DestroyTexture(texture_);
        SDL_PropertiesID p = SDL_CreateProperties();
        SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
                              nv12 ? SDL_PIXELFORMAT_NV12 : SDL_PIXELFORMAT_RGBA32);
        SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STREAMING);
        SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, frame_.width);
        SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, frame_.height);
        // What the encoders signal: BT.709, limited range.
        SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER,
                              nv12 ? SDL_COLORSPACE_BT709_LIMITED : SDL_COLORSPACE_SRGB);
        texture_ = SDL_CreateTextureWithProperties(renderer_, p);
        SDL_DestroyProperties(p);
        if (!texture_) {
            std::fprintf(stderr, "broremote-view: cannot create a %ux%u texture: %s\n", frame_.width, frame_.height,
                         SDL_GetError());
            return;
        }
        SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_LINEAR);
        tex_w_ = frame_.width;
        tex_h_ = frame_.height;
        tex_format_ = frame_.format;
    }
    const uint8_t* d = frame_.data.data();
    const bool ok = nv12 ? SDL_UpdateNVTexture(texture_, nullptr, d, int(frame_.stride), d + frame_.uv_offset,
                                               int(frame_.stride))
                         : SDL_UpdateTexture(texture_, nullptr, d, int(frame_.stride));
    if (!ok) std::fprintf(stderr, "broremote-view: texture upload failed: %s\n", SDL_GetError());
}

void Viewer::draw() {
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    int ow = 0, oh = 0;
    SDL_GetCurrentRenderOutputSize(renderer_, &ow, &oh);
    if (texture_ && have_frame_) {
        const Rect r = letterbox(float(ow), float(oh), tex_w_, tex_h_);
        const SDL_FRect dst{r.x, r.y, r.w, r.h};
        SDL_RenderTexture(renderer_, texture_, nullptr, &dst);
    } else {
        // No picture yet: say what is happening.
        const SessionStatus& s = shown_status_;
        std::string text = s.state == SessionState::Connecting ? "connecting to " + opt_.session.target.describe() + "..."
                           : s.state == SessionState::Connected ? "connected, waiting for the first picture"
                                                                : s.message;
        SDL_SetRenderDrawColor(renderer_, 200, 200, 200, 255);
        float y = 16;
        size_t pos = 0;
        while (pos <= text.size()) {
            const size_t nl = text.find('\n', pos);
            const std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            SDL_RenderDebugText(renderer_, 16, y, line.c_str());
            y += 12;
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    }
    SDL_RenderPresent(renderer_);
}

void Viewer::update_title(bool force) {
    const auto now = Clock::now();
    const double since = std::chrono::duration<double>(now - title_time_).count();
    if (!force && since < 1.0) return;
    const SessionStatus& s = shown_status_;
    const std::string where = opt_.session.target.describe();
    std::string t = "broremote-view - " + where;
    if (s.state == SessionState::Connecting) {
        t += " - connecting...";
    } else if (s.state == SessionState::Closed) {
        const std::string first_line = s.message.substr(0, s.message.find('\n'));
        t += s.failed ? " - error: " + first_line : " - disconnected: " + first_line;
    } else if (!s.have_config) {
        t += " - connected, waiting for a stream";
    } else {
        char buf[160];
        const double fps = since > 0 ? double(displayed_ - title_displayed_) / since : 0;
        const SessionStats st = session_->stats();
        const size_t n = std::min<size_t>(st.decode_ms.size(), 120);
        const std::vector<double> recent(st.decode_ms.end() - std::ptrdiff_t(n), st.decode_ms.end());
        std::snprintf(buf, sizeof buf, " - %s %ux%u - %.1f fps - decode %.1f ms", codec_name(s.config.codec),
                      s.config.width, s.config.height, fps, mean(recent));
        t += buf;
        if (!s.decoder.empty()) t += " - " + s.decoder;
        if (!s.message.empty()) t += " - " + s.message;
    }
    title_time_ = now;
    title_displayed_ = displayed_;
    if (t != title_) {
        title_ = t;
        SDL_SetWindowTitle(window_, title_.c_str());
    }
}

bool Viewer::step(int timeout_ms) {
    SDL_Event e;
    if (SDL_WaitEventTimeout(&e, timeout_ms)) {
        handle(e);
        while (SDL_PollEvent(&e)) handle(e);
    }
    const SessionStatus st = session_->status();
    const bool status_changed = st.state != shown_status_.state || st.message != shown_status_.message ||
                                st.config.stream_id != shown_status_.config.stream_id ||
                                st.decoder != shown_status_.decoder;
    shown_status_ = st;
    if (status_changed) {
        need_draw_ = true;
        update_title(true);
    }
    if (st.have_config && !fitted_) fit_window_to_stream(st.config.width, st.config.height);

    if (session_->take_frame(frame_, info_)) {
        have_frame_ = true;
        upload();
        need_draw_ = true;
        ++displayed_;
    }
    if (need_draw_) {
        draw();
        need_draw_ = false;
        if (have_frame_ && info_.sequence) {
            latency_ms_.push_back(std::chrono::duration<double, std::milli>(Clock::now() - info_.received).count());
            info_.sequence = 0;  // count each picture once
        }
    }
    update_title(false);

    if (quit_ || st.state == SessionState::Closed) return false;
    if (opt_.frames && displayed_ >= opt_.frames) return false;
    if (opt_.timeout_s > 0 &&
        std::chrono::duration<double>(Clock::now() - start_).count() > opt_.timeout_s) {
        timed_out_ = true;
        return false;
    }
    return true;
}

int Viewer::finish() {
    int rc = 0;
    const SessionStatus st = session_->status();
    const SessionStats stats = session_->stats();
    if (st.state == SessionState::Closed && !quit_ && !(opt_.frames && displayed_ >= opt_.frames)) {
        std::fprintf(stderr, "broremote-view: %s\n", st.message.c_str());
        if (st.failed) rc = 1;
    }
    if (timed_out_) {
        std::fprintf(stderr, "broremote-view: gave up after %.0f s with %llu of %u pictures shown\n", opt_.timeout_s,
                     static_cast<unsigned long long>(displayed_), opt_.frames);
        rc = 1;
    }
    if (stats.decoded > 0) {
        const double span = std::chrono::duration<double>(stats.last_decoded - stats.first_decoded).count();
        const double fps = span > 0 ? double(stats.decoded - 1) / span : 0;
        const double mbps = span > 0 ? double(stats.bytes) * 8 / span / 1e6 : 0;
        std::fprintf(stderr,
                     "broremote-view: %s %ux%u, %s\n"
                     "  %llu pictures decoded (%llu failed, %llu keyframe requests), %llu shown, %.1f fps decoded, "
                     "%.1f Mbit/s\n"
                     "  decode: mean %.2f, p50 %.2f, p99 %.2f ms; packet received to presented: mean %.1f, p50 %.1f, "
                     "p99 %.1f ms\n",
                     codec_name(st.config.codec), st.config.width, st.config.height, st.decoder.c_str(),
                     static_cast<unsigned long long>(stats.decoded), static_cast<unsigned long long>(stats.failed),
                     static_cast<unsigned long long>(stats.keyframe_requests),
                     static_cast<unsigned long long>(displayed_), fps, mbps, mean(stats.decode_ms),
                     percentile(stats.decode_ms, 0.5), percentile(stats.decode_ms, 0.99), mean(latency_ms_),
                     percentile(latency_ms_, 0.5), percentile(latency_ms_, 0.99));
    }
    if (opt_.frames && displayed_ < opt_.frames && !timed_out_) rc = 1;
    if ((!opt_.dump_png.empty() || opt_.check_pattern) && !have_frame_) {
        std::fprintf(stderr, "broremote-view: no picture was shown\n");
        return 1;
    }
    if (have_frame_) {
        const std::vector<uint8_t> rgba = tools::to_rgba(frame_);
        if (!opt_.dump_png.empty()) {
            if (write_png(opt_.dump_png, rgba.data(), frame_.width, frame_.height)) {
                std::fprintf(stderr, "broremote-view: wrote frame %llu (%ux%u) to %s\n",
                             static_cast<unsigned long long>(info_.frame_id), frame_.width, frame_.height,
                             opt_.dump_png.c_str());
            } else {
                std::fprintf(stderr, "broremote-view: cannot write %s\n", opt_.dump_png.c_str());
                rc = 1;
            }
        }
        if (opt_.check_pattern) {
            const int64_t n = tools::read_test_pattern_counter(rgba.data(), frame_.width, frame_.height,
                                                               frame_.width * 4);
            std::vector<uint8_t> source(rgba.size());
            tools::draw_test_pattern(source.data(), frame_.width, frame_.height, uint64_t(n < 0 ? 0 : n));
            const double rgb = tools::psnr_rgba(rgba.data(), source.data(), frame_.width, frame_.height);
            const double luma = frame_.format == PixelFormat::NV12 ? tools::psnr_luma(frame_, source.data()) : rgb;
            const bool good = n >= 0 && luma >= 35.0;
            std::fprintf(stderr, "broremote-view: pattern frame %lld: luma PSNR %.1f dB, RGB PSNR %.1f dB: %s\n",
                         static_cast<long long>(n), luma, rgb, good ? "match" : "MISMATCH");
            if (!good) rc = 1;
        }
    }
    return rc;
}

int run_viewer(const ViewerOptions& options) {
    Viewer v(options);
    std::string err;
    if (!v.init(&err)) {
        std::fprintf(stderr, "broremote-view: %s\n", err.c_str());
        return 1;
    }
    while (v.step(100)) {
    }
    return v.finish();
}

}  // namespace broremote::view
