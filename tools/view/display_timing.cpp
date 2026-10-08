#include "display_timing.h"

#if defined(_WIN32)
#include <dxgi.h>
#endif

namespace broremote::view {

#if defined(_WIN32)

namespace {

// steady_clock is QueryPerformanceCounter on Windows; put QPC ticks on it.
std::chrono::steady_clock::time_point from_qpc(LARGE_INTEGER ticks) {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return int64_t(f.QuadPart);
    }();
    const int64_t whole = ticks.QuadPart / freq, part = ticks.QuadPart % freq;
    const int64_t ns = whole * 1000000000ll + part * 1000000000ll / freq;
    return std::chrono::steady_clock::time_point(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(ns)));
}

}  // namespace

bool DisplayTiming::init(SDL_Renderer* renderer) {
    SDL_PropertiesID p = SDL_GetRendererProperties(renderer);
    void* sc = SDL_GetPointerProperty(p, SDL_PROP_RENDERER_D3D11_SWAPCHAIN_POINTER, nullptr);
    if (!sc) sc = SDL_GetPointerProperty(p, SDL_PROP_RENDERER_D3D12_SWAPCHAIN_POINTER, nullptr);
    swapchain_ = sc;
    return sc != nullptr;
}

void DisplayTiming::presented(std::chrono::steady_clock::time_point t) {
    if (!swapchain_) return;
    UINT count = 0;
    if (FAILED(static_cast<IDXGISwapChain*>(swapchain_)->GetLastPresentCount(&count))) return;
    pending_.push_back({count, t});
    while (pending_.size() > 64) pending_.pop_front();
}

void DisplayTiming::poll() {
    if (!swapchain_ || pending_.empty()) return;
    DXGI_FRAME_STATISTICS st{};
    if (FAILED(static_cast<IDXGISwapChain*>(swapchain_)->GetFrameStatistics(&st))) return;
    // st.PresentCount was shown at st.SyncQPCTime; everything before it is
    // past (shown or replaced), and its own sample is the one we can know.
    while (!pending_.empty() && pending_.front().count <= st.PresentCount) {
        if (pending_.front().count == st.PresentCount) {
            const double ms =
                std::chrono::duration<double, std::milli>(from_qpc(st.SyncQPCTime) - pending_.front().at).count();
            if (ms > -1 && ms < 1000) {
                recent_.push_back(ms);
                all_.push_back(ms);
            }
        }
        pending_.pop_front();
    }
}

#else

bool DisplayTiming::init(SDL_Renderer*) { return false; }
void DisplayTiming::presented(std::chrono::steady_clock::time_point) {}
void DisplayTiming::poll() {}

#endif

std::vector<double> DisplayTiming::take_samples() {
    std::vector<double> out;
    out.swap(recent_);
    return out;
}

}  // namespace broremote::view
