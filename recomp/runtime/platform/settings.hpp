// The settings file: ghoulies.ini in the data folder, as the other Qilin
// recomps keep theirs (punchout.ini, apb.ini): [display], [controls],
// [sound] and [console] sections, written with every key and comments the
// first time the game runs. The settings menu (F1 or Esc in the game)
// changes them and saves them back; the file can be edited by hand too. A
// line that can't be read is named in the log and left at its default.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace qilin::platform {

// The Xbox controller's controls the keyboard can press, in the menu's order.
enum Control : uint32_t {
    kCtlA, kCtlB, kCtlX, kCtlY, kCtlBlack, kCtlWhite, kCtlLeftTrigger, kCtlRightTrigger,
    kCtlStart, kCtlBack, kCtlUp, kCtlDown, kCtlLeft, kCtlRight,
    kCtlLeftStickUp, kCtlLeftStickDown, kCtlLeftStickLeft, kCtlLeftStickRight,
    kCtlRightStickUp, kCtlRightStickDown, kCtlRightStickLeft, kCtlRightStickRight,
    kCtlLeftThumb, kCtlRightThumb, kControlCount
};
const char* control_name(Control c);   // as the file names it: "left_stick_up"
const char* control_label(Control c);  // as the menu shows it: "Left stick up"
// Keys kept for the program: Escape and F1 (the menu), F11 (full screen).
bool key_reserved(const std::string& sdl_key_name);

struct Settings {
    // [display]
    bool fullscreen = false;           // borderless, at the desktop's mode (F11 or Alt+Enter switch it)
    enum class Scaling : uint8_t { Integer, Fit, Stretch } scaling = Scaling::Fit;
    bool smooth = false;               // bilinear; off: sharp pixels
    enum class Look : uint8_t { Plain, Crt } look = Look::Plain;
    uint32_t scanlines = 60;           // CRT: the gaps between raster lines, 0-100 (percent dark)
    uint32_t mask = 30;                // CRT: the phosphor stripes, 0-100
    bool vsync = true;                 // at the display's refresh, no tearing; off: the lowest latency
    uint32_t anisotropy = 1;           // texture filtering: 1 (the console's), 2, 4, 8 or 16
    // [controls]
    bool rumble = true;
    std::vector<std::string> keys[kControlCount];  // SDL's key names ("Space", "W")
    // [sound]
    uint32_t volume = 100;             // 0-100
    uint32_t channels = 0;             // 0 the device's own (5.1 where it has six speakers), 2 stereo, 6 5.1
    // [console]: what the game reads as the Xbox's own settings
    uint32_t language = 1;             // XC_LANGUAGE: 1 English, 3 German, 4 French, 5 Spanish, 6 Italian
    bool widescreen = true;            // the console's 16:9 setting: the game draws for a 16:9 screen (off: 4:3)
};

Settings default_settings();
void default_keys(Settings& s);  // the keyboard's controls back to the defaults (docs/input.md)

// ghoulies.ini in `folder`: read it, or write the defaults there if it's missing.
Settings load_settings(const std::filesystem::path& folder);
// Write it back (through a temporary file, so a failure leaves the old one);
// false if it can't be written.
bool save_settings(const std::filesystem::path& folder, const Settings& s);
std::filesystem::path settings_file(const std::filesystem::path& folder);

// The text, and reading it (problems into `warnings`, one per line).
std::string settings_text(const Settings& s);
Settings parse_settings(const std::string& text, std::vector<std::string>& warnings);

}  // namespace qilin::platform
