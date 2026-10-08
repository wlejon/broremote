#pragma once
// SDL scancodes (USB HID usages, the same on every platform) to Linux evdev
// KEY_* codes, and SDL mouse buttons to evdev BTN_* codes: input goes to the
// server in its own terms.

#include <SDL3/SDL.h>

#include <cstdint>

namespace broremote::view {

// The evdev KEY_* code for a scancode; 0 for one with no evdev equivalent.
uint32_t evdev_key(SDL_Scancode sc);

// The evdev BTN_* code for an SDL button (SDL_BUTTON_LEFT, ...); 0 if none.
uint32_t evdev_button(uint8_t sdl_button);

inline constexpr uint32_t kBtnLeft = 0x110, kBtnRight = 0x111, kBtnMiddle = 0x112, kBtnSide = 0x113,
                          kBtnExtra = 0x114;

}  // namespace broremote::view
