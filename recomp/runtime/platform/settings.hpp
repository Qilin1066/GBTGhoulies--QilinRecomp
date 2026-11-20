// The settings file: ghoulies.ini in the data folder, as the sister
// projects keep theirs (punchout.ini in PunchOutWii-QilinRecomp, apb.ini in
// ApbArcade-QilinRecomp): [display], [controls], [sound] and [console]
// sections, written with every key and comments the first time the game runs. The settings menu (F1 or Esc in the game)
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
    bool fsr = false;                  // AMD FSR 1 where the picture is shown larger than it's drawn
    uint32_t sharpness = 80;           // FSR's sharpening, 0-100
    bool smaa = false;                 // SMAA 1x anti-aliasing
    enum class Look : uint8_t { Plain, Crt } look = Look::Plain;
    uint32_t scanlines = 60;           // CRT: the gaps between raster lines, 0-100 (percent dark)
    uint32_t mask = 30;                // CRT: the phosphor stripes, 0-100
    bool vsync = true;                 // at the display's refresh, no tearing; off: the lowest latency
    // Frames per second: the console's 30, 60, the display's refresh rate
    // (the game's time step from the clock, not the console's vblanks), or
    // as many as the computer draws (capped at the refresh rate with vsync).
    enum class FrameRate : uint8_t { Original, Sixty, Display, Unlimited } frame_rate = FrameRate::Display;
    // With vsync, one frame at most waiting for the display (fewer swapchain
    // images): the least lag, a little less slack for a GPU near its limit.
    bool low_latency = true;
    uint32_t anisotropy = 1;           // texture filtering: 1 (the console's), 2, 4, 8 or 16
    // The picture's shape, width:height: 4:3 the original, wider ones show more
    // at the sides (up to 4:1); 0:0 is "auto", the screen's own shape.
    uint32_t aspect_w = 0, aspect_h = 0;
    // The render resolution, in multiples of the game's 480 lines (1-4; its
    // width follows the picture's shape); 0 is "auto", the window's size.
    uint32_t resolution = 0;
    // The HUD where the picture is wider than 4:3 (the game stretches it with
    // its frame): at the screen's edges, in the middle 4:3, or stretched.
    enum class Hud : uint8_t { Edges, Middle, Stretched } hud = Hud::Edges;
    // [controls]
    bool rumble = true;
    std::vector<std::string> keys[kControlCount];  // SDL's key names ("Space", "W")
    // [sound]
    uint32_t volume = 100;             // 0-100
    uint32_t channels = 0;             // 0 the device's own (5.1 where it has six speakers), 2 stereo, 6 5.1
    // [console]: what the game reads as the Xbox's own settings
    uint32_t language = 1;             // XC_LANGUAGE: 1 English, 3 German, 4 French, 5 Spanish, 6 Italian
};
// The shape asked for: width over height, or `screen` (the screen's own) for auto.
float aspect_ratio(const Settings& s, float screen);
// The render resolution's scale across and down, for a picture `aspect` wide
// shown `shown_lines` high (auto rounds to the nearest whole multiple of 480).
void resolution_scale(const Settings& s, float aspect, uint32_t shown_lines, uint32_t* x, uint32_t* y);

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
