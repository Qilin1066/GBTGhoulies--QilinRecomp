#include "input.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <utility>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace qilin::platform {
namespace {

#ifdef _WIN32
// Windows' own XInput, read directly: the fallback for an Xbox-compatible
// controller that SDL doesn't list (seen in the field: Windows had the
// controller, SDL reported none for the whole run). Loaded at run time,
// so nothing links against it; its structures as xinput.h has them. Only
// used while SDL has no gamepad open, so a controller is never read twice.
struct XiGamepad {
    WORD buttons;
    BYTE left_trigger, right_trigger;
    SHORT lx, ly, rx, ry;
};
struct XiState {
    DWORD packet;
    XiGamepad pad;
};
struct XiVibration {
    WORD left, right;
};
struct XInputApi {
    DWORD(WINAPI* get_state)(DWORD, XiState*) = nullptr;
    DWORD(WINAPI* set_state)(DWORD, XiVibration*) = nullptr;
    bool present[4] = {};
    std::chrono::steady_clock::time_point checked[4] = {};
    bool said[4] = {};
    XInputApi() {
        for (const char* dll : {"XInput1_4.dll", "XInput1_3.dll", "XInput9_1_0.dll"}) {
            const HMODULE lib = LoadLibraryA(dll);
            if (!lib) continue;
            get_state = reinterpret_cast<DWORD(WINAPI*)(DWORD, XiState*)>(reinterpret_cast<void*>(GetProcAddress(lib, "XInputGetState")));
            set_state = reinterpret_cast<DWORD(WINAPI*)(DWORD, XiVibration*)>(reinterpret_cast<void*>(GetProcAddress(lib, "XInputSetState")));
            if (get_state) break;
        }
    }
    // The slot's state, or false with nothing there. An empty slot costs a
    // device scan to ask about, so it's asked once a second.
    bool read(uint32_t slot, XiState* out) {
        if (!get_state || slot >= 4) return false;
        const auto now = std::chrono::steady_clock::now();
        if (!present[slot] && now - checked[slot] < std::chrono::seconds(1)) return false;
        checked[slot] = now;
        const bool was = present[slot];
        present[slot] = get_state(slot, out) == ERROR_SUCCESS;
        if (present[slot] && !said[slot]) {
            said[slot] = true;
            std::printf("qilin_run: controller %u: Windows' XInput slot %u, read directly (SDL didn't list it)\n", slot + 1, slot + 1);
        } else if (was && !present[slot]) {
            said[slot] = false;
            std::printf("qilin_run: controller %u removed\n", slot + 1);
        }
        return present[slot];
    }
};
XInputApi& xinput() {
    static XInputApi api;
    return api;
}
#endif

// SDL's trigger axes run 0 to 32767; the Xbox's analog buttons 0 to 255.
uint8_t trigger(int16_t v) { return uint8_t(std::clamp(int(v), 0, 32767) * 255 / 32767); }

// SDL's stick y runs positive down, the Xbox's positive up: negated, with
// -32768 (full down in SDL's terms) becoming 32767.
int16_t flip(int16_t v) { return int16_t(v == -32768 ? 32767 : -v); }

// Two inputs on one stick axis (controller and keyboard): the larger push wins.
int16_t stronger(int16_t a, int16_t b) { return std::abs(int(a)) >= std::abs(int(b)) ? a : b; }

// The keyboard, as a controller, by the settings' keys (docs/input.md has the defaults).
PadState keyboard(const std::array<std::vector<int>, kControlCount>& keys) {
    PadState k;
    const bool* key = SDL_GetKeyboardState(nullptr);
    if (!key) return k;
    auto down = [&](Control c) { return std::ranges::any_of(keys[c], [&](int s) { return key[s]; }); };
    auto axis = [&](Control neg, Control pos) { return int16_t((down(pos) ? 32767 : 0) - (down(neg) ? 32767 : 0)); };
    static constexpr std::pair<Control, uint16_t> kButtons[] = {
        {kCtlUp, kPadUp}, {kCtlDown, kPadDown}, {kCtlLeft, kPadLeft}, {kCtlRight, kPadRight}, {kCtlStart, kPadStart},
        {kCtlBack, kPadBack}, {kCtlLeftThumb, kPadLeftThumb}, {kCtlRightThumb, kPadRightThumb}};
    for (const auto& [c, bit] : kButtons)
        if (down(c)) k.buttons |= bit;
    static constexpr std::pair<Control, PadAnalog> kAnalog[] = {
        {kCtlA, kPadA}, {kCtlB, kPadB}, {kCtlX, kPadX}, {kCtlY, kPadY}, {kCtlWhite, kPadWhite}, {kCtlBlack, kPadBlack},
        {kCtlLeftTrigger, kPadLeftTrigger}, {kCtlRightTrigger, kPadRightTrigger}};
    for (const auto& [c, a] : kAnalog)
        if (down(c)) k.analog[a] = 255;
    k.thumbs[0] = axis(kCtlLeftStickLeft, kCtlLeftStickRight);
    k.thumbs[1] = axis(kCtlLeftStickDown, kCtlLeftStickUp);
    k.thumbs[2] = axis(kCtlRightStickLeft, kCtlRightStickRight);
    k.thumbs[3] = axis(kCtlRightStickDown, kCtlRightStickUp);
    return k;
}

}  // namespace

bool Input::start() {
    if (started_) return true;
    // Controllers stay readable while the window is in the background, so a
    // second screen or a overlay doesn't stop the game seeing them.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        std::printf("qilin_run: no controllers (SDL: %s); the keyboard is controller 1\n", SDL_GetError());
        return false;
    }
    started_ = true;
    // What SDL sees now: every joystick, and whether it knows it as a gamepad
    // (one without a mapping can't be read as a controller). Gamepads are
    // opened here rather than left to their SDL_EVENT_GAMEPAD_ADDED events,
    // so one already connected is there from the first frame; the events
    // that follow for them change nothing (attach skips a known id).
    int count = 0;
    SDL_JoystickID* sticks = SDL_GetJoysticks(&count);
    if (count == 0) std::printf("qilin_run: no controllers connected (one plugged in later is picked up); the keyboard is controller 1\n");
#ifdef _WIN32
    if (count == 0) {
        // What Windows itself reports, so the log says where a controller went missing.
        uint32_t seen = 0;
        XiState st{};
        for (uint32_t slot = 0; slot < 4; ++slot) seen += xinput().read(slot, &st) ? 1 : 0;
        if (!xinput().get_state) std::printf("qilin_run: Windows' XInput couldn't be loaded either\n");
        else if (!seen) std::printf("qilin_run: Windows' XInput reports no controller in any of its four slots\n");
    }
#endif
    for (int k = 0; k < count; ++k) {
        const char* name = SDL_GetJoystickNameForID(sticks[k]);
        if (SDL_IsGamepad(sticks[k])) {
            attach(uint32_t(sticks[k]));
        } else {
            std::printf("qilin_run: %s isn't known to SDL as a gamepad (no mapping), so it isn't used\n", name ? name : "a joystick");
        }
    }
    SDL_free(sticks);
    return true;
}

Input::~Input() {
    for (Port& p : ports_)
        if (p.pad) SDL_CloseGamepad(p.pad);
    if (started_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

void Input::attach(uint32_t id) {
    for (const Port& p : ports_)
        if (p.id == id) return;
    for (uint32_t n = 0; n < kPorts; ++n) {
        Port& p = ports_[n];
        if (p.id) continue;
        p.pad = SDL_OpenGamepad(SDL_JoystickID(id));
        if (!p.pad) {
            std::printf("qilin_run: can't open a controller: %s\n", SDL_GetError());
            return;
        }
        p.id = id;
        p.left = p.right = 0;
        SDL_SetGamepadPlayerIndex(p.pad, int(n));  // the player light, where the controller has one
        const char* name = SDL_GetGamepadName(p.pad);
        std::printf("qilin_run: controller %u: %s\n", n + 1, name ? name : "(unnamed)");
        return;
    }
    std::printf("qilin_run: a fifth controller isn't used (the Xbox has four ports)\n");
}

void Input::detach(uint32_t id) {
    for (uint32_t n = 0; n < kPorts; ++n) {
        Port& p = ports_[n];
        if (p.id != id) continue;
        SDL_CloseGamepad(p.pad);
        p = Port{};
        std::printf("qilin_run: controller %u removed\n", n + 1);
    }
}

void Input::on_event(const SDL_Event& e) {
    if (!started_) return;
    if (e.type == SDL_EVENT_GAMEPAD_ADDED) attach(uint32_t(e.gdevice.which));
    else if (e.type == SDL_EVENT_JOYSTICK_ADDED && SDL_IsGamepad(e.jdevice.which)) attach(uint32_t(e.jdevice.which));
    else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) detach(uint32_t(e.gdevice.which));
}

bool Input::any_sdl_pad() const {
    for (const Port& p : ports_)
        if (p.pad) return true;
    return false;
}

uint32_t Input::connected_ports() const {
    uint32_t mask = 0;
    for (uint32_t n = 0; n < kPorts; ++n)
        if (ports_[n].id) mask |= 1u << n;
#ifdef _WIN32
    if (!any_sdl_pad()) {
        XiState st{};
        for (uint32_t n = 0; n < kPorts; ++n)
            if (xinput().read(n, &st)) mask |= 1u << n;
    }
#endif
    return mask | 1u;  // the keyboard is always on port 1
}

PadState Input::state(uint32_t port) const {
    PadState s;
    if (port >= kPorts) return s;
    if (SDL_Gamepad* g = ports_[port].pad) {
        s.connected = true;
        auto button = [&](SDL_GamepadButton b) { return SDL_GetGamepadButton(g, b); };
        auto axis = [&](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(g, a); };
        if (button(SDL_GAMEPAD_BUTTON_DPAD_UP)) s.buttons |= kPadUp;
        if (button(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) s.buttons |= kPadDown;
        if (button(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) s.buttons |= kPadLeft;
        if (button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) s.buttons |= kPadRight;
        if (button(SDL_GAMEPAD_BUTTON_START)) s.buttons |= kPadStart;
        if (button(SDL_GAMEPAD_BUTTON_BACK)) s.buttons |= kPadBack;
        if (button(SDL_GAMEPAD_BUTTON_LEFT_STICK)) s.buttons |= kPadLeftThumb;
        if (button(SDL_GAMEPAD_BUTTON_RIGHT_STICK)) s.buttons |= kPadRightThumb;
        // Face buttons by position (SDL's south, east, west, north), so the
        // layout is the Xbox's on every controller; on Xbox pads they're A B X Y.
        // The original controller's face buttons are pressure-sensitive; SDL's
        // aren't, so a press is fully pressed.
        auto digital = [&](PadAnalog a, SDL_GamepadButton b) { s.analog[a] = button(b) ? 255 : 0; };
        digital(kPadA, SDL_GAMEPAD_BUTTON_SOUTH);
        digital(kPadB, SDL_GAMEPAD_BUTTON_EAST);
        digital(kPadX, SDL_GAMEPAD_BUTTON_WEST);
        digital(kPadY, SDL_GAMEPAD_BUTTON_NORTH);
        digital(kPadWhite, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        digital(kPadBlack, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        s.analog[kPadLeftTrigger] = trigger(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
        s.analog[kPadRightTrigger] = trigger(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        s.thumbs[0] = axis(SDL_GAMEPAD_AXIS_LEFTX);
        s.thumbs[1] = flip(axis(SDL_GAMEPAD_AXIS_LEFTY));
        s.thumbs[2] = axis(SDL_GAMEPAD_AXIS_RIGHTX);
        s.thumbs[3] = flip(axis(SDL_GAMEPAD_AXIS_RIGHTY));
    }
#ifdef _WIN32
    // No controller through SDL: Windows' XInput slot of the same number.
    // Its buttons' low byte and its sticks are the Xbox controller's own
    // layout (y positive up); the shoulders are White and Black, as above.
    if (XiState xi{}; !any_sdl_pad() && xinput().read(port, &xi)) {
        s.connected = true;
        s.buttons = uint16_t(xi.pad.buttons & 0xFF);
        auto digital = [&](PadAnalog a, uint32_t bit) { s.analog[a] = (xi.pad.buttons & bit) ? 255 : 0; };
        digital(kPadA, 0x1000);
        digital(kPadB, 0x2000);
        digital(kPadX, 0x4000);
        digital(kPadY, 0x8000);
        digital(kPadWhite, 0x0100);
        digital(kPadBlack, 0x0200);
        s.analog[kPadLeftTrigger] = xi.pad.left_trigger;
        s.analog[kPadRightTrigger] = xi.pad.right_trigger;
        s.thumbs[0] = xi.pad.lx, s.thumbs[1] = xi.pad.ly, s.thumbs[2] = xi.pad.rx, s.thumbs[3] = xi.pad.ry;
    }
#endif
    if (port == 0) {
        const PadState k = keyboard(keys_);
        s.connected = true;
        s.buttons |= k.buttons;
        for (int i = 0; i < 8; ++i) s.analog[i] = std::max(s.analog[i], k.analog[i]);
        for (int i = 0; i < 4; ++i) s.thumbs[i] = stronger(s.thumbs[i], k.thumbs[i]);
    }
    return s;
}

void Input::set_keys(const std::vector<std::string> (&names)[kControlCount]) {
    for (uint32_t c = 0; c < kControlCount; ++c) {
        keys_[c].clear();
        for (const std::string& n : names[c]) {
            const SDL_Scancode s = SDL_GetScancodeFromName(n.c_str());
            if (s != SDL_SCANCODE_UNKNOWN) keys_[c].push_back(int(s));
            else std::printf("qilin_run: no key is called \"%s\" (for %s)\n", n.c_str(), control_label(Control(c)));
        }
    }
}

void Input::rumble(uint32_t port, uint16_t left, uint16_t right) {
    if (!rumble_on_) left = right = 0;
#ifdef _WIN32
    if (port < kPorts && !any_sdl_pad() && xinput().set_state && xinput().present[port]) {
        XiVibration v{left, right};
        xinput().set_state(port, &v);
        return;
    }
#endif
    if (port >= kPorts || !ports_[port].pad) return;
    Port& p = ports_[port];
    p.left = left;
    p.right = right;
    // SDL's rumble runs for a set time, up to about a minute; asked again
    // with each change and each call (the game sends its rumble every
    // frame), so it lasts until changed, as the Xbox's.
    SDL_RumbleGamepad(p.pad, left, right, 0xFFFF);
}

}  // namespace qilin::platform
