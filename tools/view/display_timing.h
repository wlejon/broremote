#pragma once
// When a presented picture actually reached the display, as far as the OS
// can say: on Windows the swap chain's frame statistics give the vblank
// (SyncQPCTime) at which a given Present was shown, after DWM. This is the
// "present -> glass" part SDL_RenderPresent returning does not include (the
// monitor's own processing comes after it). Elsewhere it measures nothing.

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <vector>

namespace broremote::view {

class DisplayTiming {
public:
    // False when the renderer gives no way to know (not D3D11 / D3D12).
    bool init(SDL_Renderer* renderer);
    [[nodiscard]] bool active() const { return swapchain_ != nullptr; }
    // Right after SDL_RenderPresent returned at `t`.
    void presented(std::chrono::steady_clock::time_point t);
    // Collects what the swap chain has shown since; call now and then.
    void poll();
    // Present returned -> shown, ms, one per present the statistics matched.
    std::vector<double> take_samples();
    [[nodiscard]] const std::vector<double>& all_samples() const { return all_; }

private:
    void* swapchain_ = nullptr;  // IDXGISwapChain (not owned)
    struct Pending {
        uint32_t count;
        std::chrono::steady_clock::time_point at;
    };
    std::deque<Pending> pending_;
    std::vector<double> recent_, all_;
};

}  // namespace broremote::view
