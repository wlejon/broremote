#pragma once
// Where a frame's pixels come from: a host dmabuf imported as a VA surface
// (no copy), or a CPU frame uploaded into a surface the encoder owns. Either
// way the result is an RGB surface the VPP pass converts.

#include "broremote/frame.h"

#include <va/va.h>

#include <string>

namespace broremote::vaapi {

// Waits for a sync_file to signal; true at once for -1. False (with *err)
// on timeout or error.
bool wait_fence(int fd, int timeout_ms, std::string* err);

// A host dmabuf imported through DRM PRIME 2 (with its modifier). The surface
// refers to the host's memory; it is destroyed with this object, before the
// frame is released.
class ImportedSurface {
public:
    ImportedSurface() = default;
    ~ImportedSurface() { reset(); }
    ImportedSurface(const ImportedSurface&) = delete;
    ImportedSurface& operator=(const ImportedSurface&) = delete;

    bool import(VADisplay dpy, const Frame& frame, std::string* err);
    void reset();
    [[nodiscard]] VASurfaceID id() const { return id_; }

private:
    VADisplay dpy_ = nullptr;
    VASurfaceID id_ = VA_INVALID_SURFACE;
};

// An RGBA surface CPU frames are copied into, kept for the encoder's life.
class UploadSurface {
public:
    UploadSurface() = default;
    ~UploadSurface() { reset(); }
    UploadSurface(const UploadSurface&) = delete;
    UploadSurface& operator=(const UploadSurface&) = delete;

    // Copies the frame's rows into the surface. When this returns, the
    // frame's memory is no longer needed.
    bool upload(VADisplay dpy, const Frame& frame, std::string* err);
    void reset();
    [[nodiscard]] VASurfaceID id() const { return id_; }

private:
    bool create(VADisplay dpy, uint32_t width, uint32_t height, std::string* err);

    VADisplay dpy_ = nullptr;
    VASurfaceID id_ = VA_INVALID_SURFACE;
    VAImage image_{};            // staging image when the surface cannot be mapped directly
    bool derive_ = false;        // map the surface itself (vaDeriveImage) instead
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

// The VA fourcc of a supported dmabuf DRM format, 0 if unsupported.
uint32_t va_fourcc_for_drm(uint32_t drm_format);

}  // namespace broremote::vaapi
