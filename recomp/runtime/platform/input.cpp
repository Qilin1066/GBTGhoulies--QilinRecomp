#include "input.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>

namespace qilin::platform {
namespace {

// SDL's trigger axes run 0 to 32767; the Xbox's analog buttons 0 to 255.
uint8_t trigger(int16_t v) { return uint8_t(std::clamp(int(v), 0, 32767) * 255 / 32767); }

// SDL's stick y runs positive down, the Xbox's positive up: negated, with
// -32768 (full down in SDL's terms) becoming 32767.
int16_t flip(int16_t v) { return int16_t(v == -32768 ? 32767 : -v); }

// Two inputs on one stick axis (controller and keyboard): the larger push wins.
int16_t stronger(int16_t a, int16_t b) { return std::abs(int(a)) >= std::abs(int(b)) ? a : b; }

// The keyboard, as a controller (the table is in docs/input.md).
PadState keyboard() {
    PadState k;
    const bool* key = SDL_GetKeyboardState(nullptr);
    if (!key) return k;
    auto down = [&](SDL_Scancode s) { return key[s]; };
    auto axis = [&](SDL_Scancode neg, SDL_Scancode pos) {
        return int16_t((down(pos) ? 32767 : 0) - (down(neg) ? 32767 : 0));
    };
    if (down(SDL_SCANCODE_UP)) k.buttons |= kPadUp;
    if (down(SDL_SCANCODE_DOWN)) k.buttons |= kPadDown;
    if (down(SDL_SCANCODE_LEFT)) k.buttons |= kPadLeft;
    if (down(SDL_SCANCODE_RIGHT)) k.buttons |= kPadRight;
    if (down(SDL_SCANCODE_RETURN)) k.buttons |= kPadStart;
    if (down(SDL_SCANCODE_BACKSPACE) || down(SDL_SCANCODE_TAB)) k.buttons |= kPadBack;
    if (down(SDL_SCANCODE_F)) k.buttons |= kPadLeftThumb;
    if (down(SDL_SCANCODE_H)) k.buttons |= kPadRightThumb;
    auto analog = [&](PadAnalog a, std::initializer_list<SDL_Scancode> keys) {
        for (const SDL_Scancode s : keys)
            if (down(s)) k.analog[a] = 255;
    };
    analog(kPadA, {SDL_SCANCODE_SPACE, SDL_SCANCODE_K});
    analog(kPadB, {SDL_SCANCODE_L});
    analog(kPadX, {SDL_SCANCODE_J});
    analog(kPadY, {SDL_SCANCODE_I});
    analog(kPadWhite, {SDL_SCANCODE_Q});
    analog(kPadBlack, {SDL_SCANCODE_E});
    analog(kPadLeftTrigger, {SDL_SCANCODE_LSHIFT});
    analog(kPadRightTrigger, {SDL_SCANCODE_RSHIFT, SDL_SCANCODE_O});
    k.thumbs[0] = axis(SDL_SCANCODE_A, SDL_SCANCODE_D);
    k.thumbs[1] = axis(SDL_SCANCODE_S, SDL_SCANCODE_W);
    k.thumbs[2] = axis(SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_6);
    k.thumbs[3] = axis(SDL_SCANCODE_KP_5, SDL_SCANCODE_KP_8);
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
    // Controllers already connected arrive as SDL_EVENT_GAMEPAD_ADDED events too.
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
    else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) detach(uint32_t(e.gdevice.which));
}

uint32_t Input::connected_ports() const {
    uint32_t mask = 0;
    for (uint32_t n = 0; n < kPorts; ++n)
        if (ports_[n].id) mask |= 1u << n;
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
    if (port == 0) {
        const PadState k = keyboard();
        s.connected = true;
        s.buttons |= k.buttons;
        for (int i = 0; i < 8; ++i) s.analog[i] = std::max(s.analog[i], k.analog[i]);
        for (int i = 0; i < 4; ++i) s.thumbs[i] = stronger(s.thumbs[i], k.thumbs[i]);
    }
    return s;
}

void Input::rumble(uint32_t port, uint16_t left, uint16_t right) {
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
