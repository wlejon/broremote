#pragma once
// dmabuf frames for the VA-API tests, allocated with GBM as a compositor
// allocates its scanout buffers: XRGB8888 / ARGB8888, linear or with GBM's
// pick of the primary plane's modifiers (tiled, possibly compressed). Pixels are
// written through gbm_bo_map, which detiles; the acquire fence is a real
// sync_file exported from the buffer (DMA_BUF_IOCTL_EXPORT_SYNC_FILE).

#include "broremote/frame.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace testkit {

class GbmDevice {
public:
    ~GbmDevice() {
        if (dev_) gbm_device_destroy(dev_);
        if (fd_ >= 0) ::close(fd_);
    }
    bool open(const std::string& node) {
        fd_ = ::open(node.c_str(), O_RDWR | O_CLOEXEC);
        if (fd_ < 0) return false;
        dev_ = gbm_create_device(fd_);
        return dev_ != nullptr;
    }
    gbm_device* get() const { return dev_; }

private:
    int fd_ = -1;
    gbm_device* dev_ = nullptr;
};

// The modifiers the display's first primary plane accepts for `format`, as
// a compositor would allocate its scanout buffers from (IN_FORMATS). Empty
// when no card node can be read.
inline std::vector<uint64_t> scanout_modifiers(uint32_t format) {
    std::vector<uint64_t> mods;
    for (int i = 0; i < 16 && mods.empty(); ++i) {
        const std::string path = "/dev/dri/card" + std::to_string(i);
        const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
        if (drmModePlaneResPtr res = drmModeGetPlaneResources(fd)) {
            for (uint32_t p = 0; p < res->count_planes && mods.empty(); ++p) {
                drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, res->planes[p], DRM_MODE_OBJECT_PLANE);
                if (!props) continue;
                bool primary = false;
                uint32_t blob_id = 0;
                for (uint32_t k = 0; k < props->count_props; ++k) {
                    drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[k]);
                    if (!prop) continue;
                    if (!std::strcmp(prop->name, "type")) primary = props->prop_values[k] == DRM_PLANE_TYPE_PRIMARY;
                    if (!std::strcmp(prop->name, "IN_FORMATS")) blob_id = uint32_t(props->prop_values[k]);
                    drmModeFreeProperty(prop);
                }
                drmModeFreeObjectProperties(props);
                if (!primary || !blob_id) continue;
                if (drmModePropertyBlobPtr blob = drmModeGetPropertyBlob(fd, blob_id)) {
                    drmModeFormatModifierIterator it{};
                    while (drmModeFormatModifierBlobIterNext(blob, &it)) {
                        if (it.fmt == format) mods.push_back(it.mod);
                    }
                    drmModeFreePropertyBlob(blob);
                }
            }
            drmModeFreePlaneResources(res);
        }
        ::close(fd);
    }
    return mods;
}

// One GBM buffer object and the Frame describing it (fds owned here).
class GbmBuffer {
public:
    ~GbmBuffer() {
        for (int& fd : fds_) {
            if (fd >= 0) ::close(fd);
        }
        if (bo_) gbm_bo_destroy(bo_);
    }

    // A rendered scanout buffer with GBM's pick of `modifiers` (as a
    // compositor allocates); an empty list means DRM_FORMAT_MOD_LINEAR.
    bool alloc(gbm_device* dev, uint32_t w, uint32_t h, uint32_t format, const std::vector<uint64_t>& modifiers,
               std::string* err) {
        const uint64_t linear = DRM_FORMAT_MOD_LINEAR;
        const uint64_t* mods = modifiers.empty() ? &linear : modifiers.data();
        const unsigned count = modifiers.empty() ? 1u : unsigned(modifiers.size());
        bo_ = gbm_bo_create_with_modifiers2(dev, w, h, format, mods, count,
                                            GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
        if (!bo_) bo_ = gbm_bo_create_with_modifiers2(dev, w, h, format, mods, count, GBM_BO_USE_RENDERING);
        if (!bo_) {
            *err = std::string("gbm_bo_create failed: ") + std::strerror(errno);
            return false;
        }
        frame.width = w;
        frame.height = h;
        frame.drm_format = format;
        frame.modifier = gbm_bo_get_modifier(bo_);
        frame.plane_count = uint32_t(gbm_bo_get_plane_count(bo_));
        for (uint32_t i = 0; i < frame.plane_count && i < 4; ++i) {
            fds_[i] = gbm_bo_get_fd_for_plane(bo_, int(i));
            if (fds_[i] < 0) {
                *err = "gbm_bo_get_fd_for_plane failed";
                return false;
            }
            frame.planes[i].fd = fds_[i];
            frame.planes[i].offset = gbm_bo_get_offset(bo_, int(i));
            frame.planes[i].pitch = gbm_bo_get_stride_for_plane(bo_, int(i));
        }
        return true;
    }

    // Writes an RGBA picture (R G B A bytes) as the buffer's format.
    bool fill(const uint8_t* rgba) {
        uint32_t stride = 0;
        void* data = nullptr;
        void* map = gbm_bo_map(bo_, 0, 0, frame.width, frame.height, GBM_BO_TRANSFER_WRITE, &stride, &data);
        if (!map) return false;
        const bool bgr = frame.drm_format == DRM_FORMAT_XRGB8888 || frame.drm_format == DRM_FORMAT_ARGB8888;
        for (uint32_t y = 0; y < frame.height; ++y) {
            uint8_t* dst = static_cast<uint8_t*>(map) + size_t(y) * stride;
            const uint8_t* src = rgba + size_t(y) * frame.width * 4;
            for (uint32_t x = 0; x < frame.width; ++x) {
                dst[x * 4 + 0] = bgr ? src[x * 4 + 2] : src[x * 4 + 0];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = bgr ? src[x * 4 + 0] : src[x * 4 + 2];
                dst[x * 4 + 3] = 255;
            }
        }
        gbm_bo_unmap(bo_, data);
        return true;
    }

    // A sync_file that signals when every write to the buffer is done; -1
    // when the kernel cannot export one.
    int export_fence() const {
        dma_buf_export_sync_file arg{};
        arg.flags = DMA_BUF_SYNC_READ;
        arg.fd = -1;
        if (ioctl(fds_[0], DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &arg) != 0) return -1;
        return arg.fd;
    }

    broremote::Frame frame;

private:
    gbm_bo* bo_ = nullptr;
    int fds_[4] = {-1, -1, -1, -1};
};

}  // namespace testkit
