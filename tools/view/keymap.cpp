#include "keymap.h"

namespace broremote::view {

namespace {

// Linux evdev codes (include/uapi/linux/input-event-codes.h). Spelled out
// here so the viewer builds where that header does not exist.
enum : uint32_t {
    KEY_ESC = 1, KEY_1 = 2, KEY_2 = 3, KEY_3 = 4, KEY_4 = 5, KEY_5 = 6, KEY_6 = 7, KEY_7 = 8, KEY_8 = 9, KEY_9 = 10,
    KEY_0 = 11, KEY_MINUS = 12, KEY_EQUAL = 13, KEY_BACKSPACE = 14, KEY_TAB = 15, KEY_Q = 16, KEY_W = 17, KEY_E = 18,
    KEY_R = 19, KEY_T = 20, KEY_Y = 21, KEY_U = 22, KEY_I = 23, KEY_O = 24, KEY_P = 25, KEY_LEFTBRACE = 26,
    KEY_RIGHTBRACE = 27, KEY_ENTER = 28, KEY_LEFTCTRL = 29, KEY_A = 30, KEY_S = 31, KEY_D = 32, KEY_F = 33,
    KEY_G = 34, KEY_H = 35, KEY_J = 36, KEY_K = 37, KEY_L = 38, KEY_SEMICOLON = 39, KEY_APOSTROPHE = 40,
    KEY_GRAVE = 41, KEY_LEFTSHIFT = 42, KEY_BACKSLASH = 43, KEY_Z = 44, KEY_X = 45, KEY_C = 46, KEY_V = 47,
    KEY_B = 48, KEY_N = 49, KEY_M = 50, KEY_COMMA = 51, KEY_DOT = 52, KEY_SLASH = 53, KEY_RIGHTSHIFT = 54,
    KEY_KPASTERISK = 55, KEY_LEFTALT = 56, KEY_SPACE = 57, KEY_CAPSLOCK = 58, KEY_F1 = 59, KEY_F2 = 60,
    KEY_F3 = 61, KEY_F4 = 62, KEY_F5 = 63, KEY_F6 = 64, KEY_F7 = 65, KEY_F8 = 66, KEY_F9 = 67, KEY_F10 = 68,
    KEY_NUMLOCK = 69, KEY_SCROLLLOCK = 70, KEY_KP7 = 71, KEY_KP8 = 72, KEY_KP9 = 73, KEY_KPMINUS = 74, KEY_KP4 = 75,
    KEY_KP5 = 76, KEY_KP6 = 77, KEY_KPPLUS = 78, KEY_KP1 = 79, KEY_KP2 = 80, KEY_KP3 = 81, KEY_KP0 = 82,
    KEY_KPDOT = 83, KEY_ZENKAKUHANKAKU = 85, KEY_102ND = 86, KEY_F11 = 87, KEY_F12 = 88, KEY_RO = 89,
    KEY_KATAKANA = 90, KEY_HIRAGANA = 91, KEY_HENKAN = 92, KEY_KATAKANAHIRAGANA = 93, KEY_MUHENKAN = 94,
    KEY_KPENTER = 96, KEY_RIGHTCTRL = 97, KEY_KPSLASH = 98, KEY_SYSRQ = 99, KEY_RIGHTALT = 100, KEY_HOME = 102,
    KEY_UP = 103, KEY_PAGEUP = 104, KEY_LEFT = 105, KEY_RIGHT = 106, KEY_END = 107, KEY_DOWN = 108,
    KEY_PAGEDOWN = 109, KEY_INSERT = 110, KEY_DELETE = 111, KEY_MUTE = 113, KEY_VOLUMEDOWN = 114,
    KEY_VOLUMEUP = 115, KEY_POWER = 116, KEY_KPEQUAL = 117, KEY_KPPLUSMINUS = 118, KEY_PAUSE = 119,
    KEY_KPCOMMA = 121, KEY_HANGEUL = 122, KEY_HANJA = 123, KEY_YEN = 124, KEY_LEFTMETA = 125, KEY_RIGHTMETA = 126,
    KEY_COMPOSE = 127, KEY_STOP = 128, KEY_AGAIN = 129, KEY_UNDO = 131, KEY_COPY = 133, KEY_OPEN = 134,
    KEY_PASTE = 135, KEY_FIND = 136, KEY_CUT = 137, KEY_HELP = 138, KEY_MENU = 139, KEY_SLEEP = 142,
    KEY_WAKEUP = 143, KEY_BOOKMARKS = 156, KEY_BACK = 158, KEY_FORWARD = 159, KEY_CLOSECD = 160,
    KEY_EJECTCD = 161, KEY_NEXTSONG = 163, KEY_PLAYPAUSE = 164, KEY_PREVIOUSSONG = 165, KEY_STOPCD = 166,
    KEY_RECORD = 167, KEY_REWIND = 168, KEY_HOMEPAGE = 172, KEY_REFRESH = 173, KEY_EXIT = 174,
    KEY_KPLEFTPAREN = 179, KEY_KPRIGHTPAREN = 180, KEY_NEW = 181, KEY_F13 = 183, KEY_F14 = 184, KEY_F15 = 185,
    KEY_F16 = 186, KEY_F17 = 187, KEY_F18 = 188, KEY_F19 = 189, KEY_F20 = 190, KEY_F21 = 191, KEY_F22 = 192,
    KEY_F23 = 193, KEY_F24 = 194, KEY_PLAYCD = 200, KEY_PAUSECD = 201, KEY_FASTFORWARD = 208, KEY_PRINT = 210,
    KEY_SEARCH = 217, KEY_SAVE = 234, KEY_MEDIA = 226, KEY_PROPS = 130, KEY_SELECT = 0x161,
};

}  // namespace

uint32_t evdev_key(SDL_Scancode sc) {
    switch (sc) {
        // Letters.
        case SDL_SCANCODE_A: return KEY_A;
        case SDL_SCANCODE_B: return KEY_B;
        case SDL_SCANCODE_C: return KEY_C;
        case SDL_SCANCODE_D: return KEY_D;
        case SDL_SCANCODE_E: return KEY_E;
        case SDL_SCANCODE_F: return KEY_F;
        case SDL_SCANCODE_G: return KEY_G;
        case SDL_SCANCODE_H: return KEY_H;
        case SDL_SCANCODE_I: return KEY_I;
        case SDL_SCANCODE_J: return KEY_J;
        case SDL_SCANCODE_K: return KEY_K;
        case SDL_SCANCODE_L: return KEY_L;
        case SDL_SCANCODE_M: return KEY_M;
        case SDL_SCANCODE_N: return KEY_N;
        case SDL_SCANCODE_O: return KEY_O;
        case SDL_SCANCODE_P: return KEY_P;
        case SDL_SCANCODE_Q: return KEY_Q;
        case SDL_SCANCODE_R: return KEY_R;
        case SDL_SCANCODE_S: return KEY_S;
        case SDL_SCANCODE_T: return KEY_T;
        case SDL_SCANCODE_U: return KEY_U;
        case SDL_SCANCODE_V: return KEY_V;
        case SDL_SCANCODE_W: return KEY_W;
        case SDL_SCANCODE_X: return KEY_X;
        case SDL_SCANCODE_Y: return KEY_Y;
        case SDL_SCANCODE_Z: return KEY_Z;
        // The digit row.
        case SDL_SCANCODE_1: return KEY_1;
        case SDL_SCANCODE_2: return KEY_2;
        case SDL_SCANCODE_3: return KEY_3;
        case SDL_SCANCODE_4: return KEY_4;
        case SDL_SCANCODE_5: return KEY_5;
        case SDL_SCANCODE_6: return KEY_6;
        case SDL_SCANCODE_7: return KEY_7;
        case SDL_SCANCODE_8: return KEY_8;
        case SDL_SCANCODE_9: return KEY_9;
        case SDL_SCANCODE_0: return KEY_0;
        // Editing and whitespace.
        case SDL_SCANCODE_RETURN: return KEY_ENTER;
        case SDL_SCANCODE_ESCAPE: return KEY_ESC;
        case SDL_SCANCODE_BACKSPACE: return KEY_BACKSPACE;
        case SDL_SCANCODE_TAB: return KEY_TAB;
        case SDL_SCANCODE_SPACE: return KEY_SPACE;
        // Punctuation.
        case SDL_SCANCODE_MINUS: return KEY_MINUS;
        case SDL_SCANCODE_EQUALS: return KEY_EQUAL;
        case SDL_SCANCODE_LEFTBRACKET: return KEY_LEFTBRACE;
        case SDL_SCANCODE_RIGHTBRACKET: return KEY_RIGHTBRACE;
        case SDL_SCANCODE_BACKSLASH: return KEY_BACKSLASH;
        case SDL_SCANCODE_NONUSHASH: return KEY_BACKSLASH;  // ISO '#': evdev has no separate code
        case SDL_SCANCODE_SEMICOLON: return KEY_SEMICOLON;
        case SDL_SCANCODE_APOSTROPHE: return KEY_APOSTROPHE;
        case SDL_SCANCODE_GRAVE: return KEY_GRAVE;
        case SDL_SCANCODE_COMMA: return KEY_COMMA;
        case SDL_SCANCODE_PERIOD: return KEY_DOT;
        case SDL_SCANCODE_SLASH: return KEY_SLASH;
        case SDL_SCANCODE_NONUSBACKSLASH: return KEY_102ND;
        // Locks.
        case SDL_SCANCODE_CAPSLOCK: return KEY_CAPSLOCK;
        case SDL_SCANCODE_SCROLLLOCK: return KEY_SCROLLLOCK;
        case SDL_SCANCODE_NUMLOCKCLEAR: return KEY_NUMLOCK;
        // Function keys.
        case SDL_SCANCODE_F1: return KEY_F1;
        case SDL_SCANCODE_F2: return KEY_F2;
        case SDL_SCANCODE_F3: return KEY_F3;
        case SDL_SCANCODE_F4: return KEY_F4;
        case SDL_SCANCODE_F5: return KEY_F5;
        case SDL_SCANCODE_F6: return KEY_F6;
        case SDL_SCANCODE_F7: return KEY_F7;
        case SDL_SCANCODE_F8: return KEY_F8;
        case SDL_SCANCODE_F9: return KEY_F9;
        case SDL_SCANCODE_F10: return KEY_F10;
        case SDL_SCANCODE_F11: return KEY_F11;
        case SDL_SCANCODE_F12: return KEY_F12;
        case SDL_SCANCODE_F13: return KEY_F13;
        case SDL_SCANCODE_F14: return KEY_F14;
        case SDL_SCANCODE_F15: return KEY_F15;
        case SDL_SCANCODE_F16: return KEY_F16;
        case SDL_SCANCODE_F17: return KEY_F17;
        case SDL_SCANCODE_F18: return KEY_F18;
        case SDL_SCANCODE_F19: return KEY_F19;
        case SDL_SCANCODE_F20: return KEY_F20;
        case SDL_SCANCODE_F21: return KEY_F21;
        case SDL_SCANCODE_F22: return KEY_F22;
        case SDL_SCANCODE_F23: return KEY_F23;
        case SDL_SCANCODE_F24: return KEY_F24;
        // The block above the arrows.
        case SDL_SCANCODE_PRINTSCREEN: return KEY_SYSRQ;
        case SDL_SCANCODE_PAUSE: return KEY_PAUSE;
        case SDL_SCANCODE_INSERT: return KEY_INSERT;
        case SDL_SCANCODE_HOME: return KEY_HOME;
        case SDL_SCANCODE_PAGEUP: return KEY_PAGEUP;
        case SDL_SCANCODE_DELETE: return KEY_DELETE;
        case SDL_SCANCODE_END: return KEY_END;
        case SDL_SCANCODE_PAGEDOWN: return KEY_PAGEDOWN;
        // Arrows.
        case SDL_SCANCODE_RIGHT: return KEY_RIGHT;
        case SDL_SCANCODE_LEFT: return KEY_LEFT;
        case SDL_SCANCODE_DOWN: return KEY_DOWN;
        case SDL_SCANCODE_UP: return KEY_UP;
        // Keypad.
        case SDL_SCANCODE_KP_DIVIDE: return KEY_KPSLASH;
        case SDL_SCANCODE_KP_MULTIPLY: return KEY_KPASTERISK;
        case SDL_SCANCODE_KP_MINUS: return KEY_KPMINUS;
        case SDL_SCANCODE_KP_PLUS: return KEY_KPPLUS;
        case SDL_SCANCODE_KP_ENTER: return KEY_KPENTER;
        case SDL_SCANCODE_KP_1: return KEY_KP1;
        case SDL_SCANCODE_KP_2: return KEY_KP2;
        case SDL_SCANCODE_KP_3: return KEY_KP3;
        case SDL_SCANCODE_KP_4: return KEY_KP4;
        case SDL_SCANCODE_KP_5: return KEY_KP5;
        case SDL_SCANCODE_KP_6: return KEY_KP6;
        case SDL_SCANCODE_KP_7: return KEY_KP7;
        case SDL_SCANCODE_KP_8: return KEY_KP8;
        case SDL_SCANCODE_KP_9: return KEY_KP9;
        case SDL_SCANCODE_KP_0: return KEY_KP0;
        case SDL_SCANCODE_KP_PERIOD: return KEY_KPDOT;
        case SDL_SCANCODE_KP_EQUALS: return KEY_KPEQUAL;
        case SDL_SCANCODE_KP_COMMA: return KEY_KPCOMMA;
        case SDL_SCANCODE_KP_LEFTPAREN: return KEY_KPLEFTPAREN;
        case SDL_SCANCODE_KP_RIGHTPAREN: return KEY_KPRIGHTPAREN;
        case SDL_SCANCODE_KP_PLUSMINUS: return KEY_KPPLUSMINUS;
        // Modifiers.
        case SDL_SCANCODE_LCTRL: return KEY_LEFTCTRL;
        case SDL_SCANCODE_LSHIFT: return KEY_LEFTSHIFT;
        case SDL_SCANCODE_LALT: return KEY_LEFTALT;
        case SDL_SCANCODE_LGUI: return KEY_LEFTMETA;
        case SDL_SCANCODE_RCTRL: return KEY_RIGHTCTRL;
        case SDL_SCANCODE_RSHIFT: return KEY_RIGHTSHIFT;
        case SDL_SCANCODE_RALT: return KEY_RIGHTALT;
        case SDL_SCANCODE_RGUI: return KEY_RIGHTMETA;
        // The PC "menu" key is evdev's compose key; MENU is the HID menu usage.
        case SDL_SCANCODE_APPLICATION: return KEY_COMPOSE;
        case SDL_SCANCODE_MENU: return KEY_MENU;
        // International keys (JIS, Korean).
        case SDL_SCANCODE_INTERNATIONAL1: return KEY_RO;
        case SDL_SCANCODE_INTERNATIONAL2: return KEY_KATAKANAHIRAGANA;
        case SDL_SCANCODE_INTERNATIONAL3: return KEY_YEN;
        case SDL_SCANCODE_INTERNATIONAL4: return KEY_HENKAN;
        case SDL_SCANCODE_INTERNATIONAL5: return KEY_MUHENKAN;
        case SDL_SCANCODE_LANG1: return KEY_HANGEUL;
        case SDL_SCANCODE_LANG2: return KEY_HANJA;
        case SDL_SCANCODE_LANG3: return KEY_KATAKANA;
        case SDL_SCANCODE_LANG4: return KEY_HIRAGANA;
        case SDL_SCANCODE_LANG5: return KEY_ZENKAKUHANKAKU;
        // System, editing and media keys.
        case SDL_SCANCODE_POWER: return KEY_POWER;
        case SDL_SCANCODE_SLEEP: return KEY_SLEEP;
        case SDL_SCANCODE_HELP: return KEY_HELP;
        case SDL_SCANCODE_SELECT: return KEY_SELECT;
        case SDL_SCANCODE_STOP: return KEY_STOP;
        case SDL_SCANCODE_AGAIN: return KEY_AGAIN;
        case SDL_SCANCODE_UNDO: return KEY_UNDO;
        case SDL_SCANCODE_CUT: return KEY_CUT;
        case SDL_SCANCODE_COPY: return KEY_COPY;
        case SDL_SCANCODE_PASTE: return KEY_PASTE;
        case SDL_SCANCODE_FIND: return KEY_FIND;
        case SDL_SCANCODE_MUTE: return KEY_MUTE;
        case SDL_SCANCODE_VOLUMEUP: return KEY_VOLUMEUP;
        case SDL_SCANCODE_VOLUMEDOWN: return KEY_VOLUMEDOWN;
        case SDL_SCANCODE_MEDIA_PLAY: return KEY_PLAYCD;
        case SDL_SCANCODE_MEDIA_PAUSE: return KEY_PAUSECD;
        case SDL_SCANCODE_MEDIA_RECORD: return KEY_RECORD;
        case SDL_SCANCODE_MEDIA_FAST_FORWARD: return KEY_FASTFORWARD;
        case SDL_SCANCODE_MEDIA_REWIND: return KEY_REWIND;
        case SDL_SCANCODE_MEDIA_NEXT_TRACK: return KEY_NEXTSONG;
        case SDL_SCANCODE_MEDIA_PREVIOUS_TRACK: return KEY_PREVIOUSSONG;
        case SDL_SCANCODE_MEDIA_STOP: return KEY_STOPCD;
        case SDL_SCANCODE_MEDIA_EJECT: return KEY_EJECTCD;
        case SDL_SCANCODE_MEDIA_PLAY_PAUSE: return KEY_PLAYPAUSE;
        case SDL_SCANCODE_MEDIA_SELECT: return KEY_MEDIA;
        case SDL_SCANCODE_AC_NEW: return KEY_NEW;
        case SDL_SCANCODE_AC_OPEN: return KEY_OPEN;
        case SDL_SCANCODE_AC_CLOSE: return KEY_CLOSECD;
        case SDL_SCANCODE_AC_EXIT: return KEY_EXIT;
        case SDL_SCANCODE_AC_SAVE: return KEY_SAVE;
        case SDL_SCANCODE_AC_PRINT: return KEY_PRINT;
        case SDL_SCANCODE_AC_PROPERTIES: return KEY_PROPS;
        case SDL_SCANCODE_AC_SEARCH: return KEY_SEARCH;
        case SDL_SCANCODE_AC_HOME: return KEY_HOMEPAGE;
        case SDL_SCANCODE_AC_BACK: return KEY_BACK;
        case SDL_SCANCODE_AC_FORWARD: return KEY_FORWARD;
        case SDL_SCANCODE_AC_STOP: return KEY_STOP;
        case SDL_SCANCODE_AC_REFRESH: return KEY_REFRESH;
        case SDL_SCANCODE_AC_BOOKMARKS: return KEY_BOOKMARKS;
        default: return 0;
    }
}

uint32_t evdev_button(uint8_t sdl_button) {
    switch (sdl_button) {
        case SDL_BUTTON_LEFT: return kBtnLeft;
        case SDL_BUTTON_RIGHT: return kBtnRight;
        case SDL_BUTTON_MIDDLE: return kBtnMiddle;
        case SDL_BUTTON_X1: return kBtnSide;
        case SDL_BUTTON_X2: return kBtnExtra;
        default: return 0;
    }
}

}  // namespace broremote::view
