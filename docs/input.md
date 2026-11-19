# Input: controllers and the keyboard

Last updated 2026-11-19 (v0.36.0: the keyboard's keys set in the settings menu and ghoulies.ini, rumble switch; v0.34.0: Windows' own XInput read directly when SDL lists no controller; v0.24.0: controllers through SDL3, the keyboard as controller 1).

The game reads the Xbox's four controller ports through XInput (the XDK's XPP library), which the runtime replaces natively (`recomp/runtime/host/xinput.cpp`) over SDL3's gamepad API (`recomp/runtime/platform/input.*`).

## What the game does

It asks about one device type, gamepads (`XDEVICE_TYPE_GAMEPAD`, the descriptor at `0x1FCDC8`); any other type stops the runtime. At start-up (`0x155540`) it calls `XInitDevices`, then `XGetDevices` for the ports with a controller, and opens each. Each frame (`0x1555B0`) it asks `XGetDeviceChanges` what was plugged in or removed, closes and opens ports to match, reads every open port with `XInputGetState`, and, when its vibration option is on, sends each port's rumble with `XInputSetState`.

| Function | Replacement |
|---|---|
| `XInitDevices` | Checks the device types reserved are gamepads |
| `XGetDevices` | The ports with a controller; what XGetDeviceChanges counts from |
| `XGetDeviceChanges` | Insertions and removals since the last call (or XGetDevices) |
| `XInputOpen` | A handle for the port, or `NULL` with `ERROR_DEVICE_NOT_CONNECTED` (through XAPI's own `SetLastError`). Slots and polling parameters stop the runtime |
| `XInputClose` | Closes the port, stopping its rumble |
| `XInputGetState` | `XINPUT_STATE`: the packet number (it steps when the state changes), the digital buttons, the eight analog buttons, both sticks; `ERROR_DEVICE_NOT_CONNECTED` once the controller is gone |
| `XInputSetState` | The two motors' speeds to the controller's rumble. The original sends it asynchronously and returns `ERROR_IO_PENDING`; here it's sent at once, so the request's status is already `ERROR_SUCCESS`. A completion event stops the runtime |

## Controllers

Any controller SDL3 knows (Xbox, PlayStation, Switch and generic pads, over USB or Bluetooth) is read through SDL's standard layout and mapped onto the Xbox controller's:

| Xbox controller | SDL (an Xbox One pad's names) |
|---|---|
| A, B, X, Y | The face buttons by position: bottom, right, left, top (A, B, X, Y) |
| Black, White | Right and left shoulder buttons (RB, LB) |
| Left and right triggers | The triggers, 0–255 |
| Sticks, stick clicks | The sticks (y turned to positive up), stick clicks |
| D-pad, Start, Back | D-pad, Menu (Start), View (Back) |

The original controller's face, Black and White buttons are pressure-sensitive; SDL's buttons aren't, so a press reads fully pressed (255). Controllers take the lowest free port as they connect and keep it until they're removed; the player light, where a controller has one, shows the port. Controllers are read while the window is in the background, too.

Rumble: the Xbox's left motor is the low-frequency one, as SDL's. SDL's rumble runs for a set time (at most about a minute); the game sends its rumble every frame, so it lasts until the game changes it, as on the console.

**Windows' XInput as a fallback** (v0.34.0). On one Windows machine with an Xbox controller connected, SDL listed no controller at all for the whole run, while the keyboard worked. When SDL has no gamepad open, the runtime reads Windows' own XInput directly (`XInput1_4.dll`, else 1.3 or 9.1.0, loaded at run time): its four slots are the four ports, and its buttons and sticks are the Xbox controller's own layout, with the shoulders as White and Black and rumble through `XInputSetState`. An empty slot is asked about once a second (asking costs a device scan). As soon as SDL opens a gamepad, SDL's controllers are used and XInput isn't read, so a controller is never read twice. At start the log says what SDL sees, and when SDL sees nothing, whether XInput does (`controller 1: Windows' XInput slot 1, read directly (SDL didn't list it)`).

## The keyboard

Since v0.36.0 the keys are settings ([controls] in `ghoulies.ini`, SDL's key names, several to a control), set in the settings menu by pressing them; Escape, F1 and F11 are kept for the program. The table is the defaults. Rumble can be switched off there too.

The keyboard is controller 1 (port 0): merged with the controller there, and on its own when none is connected, so the game can be played without one. Port 1 always has a device, as a console with a controller in its first port.

| Keyboard | Controller |
|---|---|
| W A S D | Left stick |
| Numeric keypad 8 4 5 6 | Right stick |
| Arrow keys | D-pad |
| Space, K | A |
| L | B |
| J | X |
| I | Y |
| Q | White |
| E | Black |
| Left Shift | Left trigger |
| Right Shift, O | Right trigger |
| Enter | Start |
| Backspace, Tab | Back |
| F, H | Left, right stick click |

F11 and Alt+Enter switch fullscreen (the window, not the game). F1 or Esc, or Back and Start together on a controller, open the settings menu.

## Not modelled yet

Memory units and other devices in a controller's slots, the voice communicator, keyboards and other device types as the game would see them (none of which the game asks about), button remapping.
