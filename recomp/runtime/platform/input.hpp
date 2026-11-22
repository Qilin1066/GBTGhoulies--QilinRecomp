// Controllers, through SDL3's gamepad API, as the Xbox's four controller
// ports. Any controller SDL knows (Xbox, PlayStation, Switch, generic, over
// USB or Bluetooth) is read through its standard layout and mapped onto the
// Xbox controller's: the face buttons, Black and White on the shoulder
// buttons, the triggers, both sticks and the d-pad.
//
// On Windows, when SDL lists no gamepad, Windows' own XInput is read
// directly instead (its four slots as the four ports), so an Xbox-compatible
// controller works whatever SDL makes of it.
//
// Controllers take the lowest free port as they connect, and keep it until
// they're removed. The keyboard also drives port 1 (index 0): merged with
// the controller there, and on its own when no controller is connected, so
// the game can be played without one.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "settings.hpp"

union SDL_Event;
struct SDL_Gamepad;

namespace qilin::platform {

// One port's state, in the Xbox's terms (XINPUT_GAMEPAD).
struct PadState {
    bool connected = false;
    uint16_t buttons = 0;        // XINPUT_GAMEPAD_DPAD_UP 0x1, DOWN 0x2, LEFT 0x4, RIGHT 0x8, START 0x10, BACK 0x20,
                                 // LEFT_THUMB 0x40, RIGHT_THUMB 0x80
    uint8_t analog[8] = {};      // A, B, X, Y, BLACK, WHITE, LEFT_TRIGGER, RIGHT_TRIGGER: 0 (released) to 255
    int16_t thumbs[4] = {};      // left x, left y, right x, right y; y is positive up
    bool operator==(const PadState&) const = default;
};

enum PadButton : uint16_t {
    kPadUp = 0x1, kPadDown = 0x2, kPadLeft = 0x4, kPadRight = 0x8, kPadStart = 0x10, kPadBack = 0x20,
    kPadLeftThumb = 0x40, kPadRightThumb = 0x80,
};
enum PadAnalog : uint8_t { kPadA, kPadB, kPadX, kPadY, kPadBlack, kPadWhite, kPadLeftTrigger, kPadRightTrigger };

class Input {
public:
    static constexpr uint32_t kPorts = 4;

    // Starts SDL's gamepad support. False (with a message printed) if it
    // can't; the ports then have the keyboard only.
    Input() = default;
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
    bool start();
    ~Input();

    void on_event(const SDL_Event& e);  // connections and removals; the window forwards every event

    PadState state(uint32_t port) const;  // as of the last events handled
    uint32_t connected_ports() const;     // bit n: port n has a controller (or the keyboard)

    // Rumble, the two motors' speeds from 0 to 65535 (the Xbox's left motor
    // is the low-frequency one). Lasts until changed.
    void rumble(uint32_t port, uint16_t left, uint16_t right);
    void set_rumble(bool on);
    void stop_rumble();
    // The keyboard's keys for each control (SDL's key names).
    void set_keys(const std::vector<std::string> (&names)[kControlCount]);

private:
    struct Port {
        uint32_t id = 0;  // SDL_JoystickID, 0 for none
        ::SDL_Gamepad* pad = nullptr;
        uint16_t left = 0, right = 0;  // rumble asked for
    };
    std::array<Port, kPorts> ports_{};
    bool started_ = false;
    bool rumble_on_ = true;
    std::array<std::vector<int>, kControlCount> keys_;  // SDL scancodes
    void attach(uint32_t id);
    void detach(uint32_t id);
    bool any_sdl_pad() const;  // a gamepad is open through SDL (Windows' XInput is read directly only when none is)
};

// A key held as if on the keyboard (a test run's script presses the game's
// keys this way): `name` is SDL's name for it ("Return", "Space", "W",
// "Keypad 4"). False if SDL knows no such key.
bool hold_key(const std::string& name, bool down);

}  // namespace qilin::platform
