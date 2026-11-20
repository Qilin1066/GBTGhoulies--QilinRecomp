#include "settings.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>

namespace qilin::platform {
namespace {

struct ControlInfo {
    const char* name;
    const char* label;
    std::vector<std::string> keys;  // the defaults
};
const ControlInfo& info(Control c) {
    static const ControlInfo kInfo[kControlCount] = {
        {"a", "A", {"Space", "K"}},
        {"b", "B", {"L"}},
        {"x", "X", {"J"}},
        {"y", "Y", {"I"}},
        {"black", "Black", {"E"}},
        {"white", "White", {"Q"}},
        {"left_trigger", "Left trigger", {"Left Shift"}},
        {"right_trigger", "Right trigger", {"Right Shift", "O"}},
        {"start", "Start", {"Return"}},
        {"back", "Back", {"Backspace", "Tab"}},
        {"dpad_up", "D-pad up", {"Up"}},
        {"dpad_down", "D-pad down", {"Down"}},
        {"dpad_left", "D-pad left", {"Left"}},
        {"dpad_right", "D-pad right", {"Right"}},
        {"left_stick_up", "Left stick up", {"W"}},
        {"left_stick_down", "Left stick down", {"S"}},
        {"left_stick_left", "Left stick left", {"A"}},
        {"left_stick_right", "Left stick right", {"D"}},
        {"right_stick_up", "Right stick up", {"Keypad 8"}},
        {"right_stick_down", "Right stick down", {"Keypad 5"}},
        {"right_stick_left", "Right stick left", {"Keypad 4"}},
        {"right_stick_right", "Right stick right", {"Keypad 6"}},
        {"left_stick_click", "Left stick click", {"F"}},
        {"right_stick_click", "Right stick click", {"H"}},
    };
    return kInfo[c];
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool read_bool(const std::string& v, bool* out) {
    if (v == "on" || v == "true" || v == "yes" || v == "1") return *out = true, true;
    if (v == "off" || v == "false" || v == "no" || v == "0") return *out = false, true;
    return false;
}
bool read_uint(const std::string& v, uint32_t lo, uint32_t hi, uint32_t* out) {
    char* end = nullptr;
    const unsigned long n = std::strtoul(v.c_str(), &end, 10);
    if (v.empty() || *end || n < lo || n > hi) return false;
    *out = uint32_t(n);
    return true;
}
const char* on(bool b) { return b ? "on" : "off"; }
// "auto" (0:0), or W:H with both 1-10000.
bool read_aspect(const std::string& v, uint32_t* w, uint32_t* h) {
    if (v == "auto") return *w = 0, *h = 0, true;
    const size_t colon = v.find(':');
    uint32_t a = 0, b = 0;
    if (colon == std::string::npos || !read_uint(v.substr(0, colon), 1, 10000, &a) || !read_uint(v.substr(colon + 1), 1, 10000, &b))
        return false;
    return *w = a, *h = b, true;
}

}  // namespace

const char* control_name(Control c) { return info(c).name; }
const char* control_label(Control c) { return info(c).label; }
bool key_reserved(const std::string& name) { return name == "Escape" || name == "F1" || name == "F11"; }

void default_keys(Settings& s) {
    for (uint32_t c = 0; c < kControlCount; ++c) s.keys[c] = info(Control(c)).keys;
}

float aspect_ratio(const Settings& s, float screen) {
    return s.aspect_w && s.aspect_h ? float(s.aspect_w) / float(s.aspect_h) : screen;
}

void resolution_scale(const Settings& s, float aspect, uint32_t shown_lines, uint32_t* x, uint32_t* y) {
    const uint32_t down = s.resolution ? s.resolution : std::clamp<uint32_t>((shown_lines + 240) / 480, 1, 4);
    // Across: as many as the picture is wider than 4:3, so its pixels stay
    // about square on screen (the game's 640 columns are stretched to it).
    *y = down;
    *x = std::clamp<uint32_t>(uint32_t(std::lround(float(down) * aspect * 0.75f)), 1, 8);
}

Settings default_settings() {
    Settings s;
    default_keys(s);
    return s;
}

std::string settings_text(const Settings& s) {
    std::ostringstream o;
    o << "# Grabbed by the Ghoulies QilinRecomp: settings. The settings menu (F1 or Esc in the\n"
         "# game, or Back and Start together on a controller) changes them and writes\n"
         "# them here; this file can be edited by hand too.\n\n";
    o << "[display]\n"
         "# Borderless, at the desktop's mode (F11 or Alt+Enter switch it).\n"
         "fullscreen = " << on(s.fullscreen) << "\n"
         "# integer: whole multiples of the picture (the sharpest); fit: the largest\n"
         "# that keeps its shape; stretch: the whole window.\n"
         "scaling = " << (s.scaling == Settings::Scaling::Integer ? "integer" : s.scaling == Settings::Scaling::Stretch ? "stretch" : "fit") << "\n"
         "# on: bilinear; off: sharp pixels (each of the game's pixels a solid block).\n"
         "smooth = " << on(s.smooth) << "\n"
         "# AMD FidelityFX Super Resolution 1 where the picture is shown larger than\n"
         "# the render resolution (not at whole multiples), and its sharpening, 0-100.\n"
         "fsr = " << on(s.fsr) << "\n"
         "sharpness = " << s.sharpness << "\n"
         "# Anti-aliasing: off, or smaa (SMAA 1x: jagged edges smoothed, at the\n"
         "# render resolution, before any upscaling).\n"
         "antialiasing = " << (s.smaa ? "smaa" : "off") << "\n"
         "# plain, or crt: a television's raster lines and phosphor stripes.\n"
         "look = " << (s.look == Settings::Look::Crt ? "crt" : "plain") << "\n"
         "# CRT: how dark the gaps between lines are, and the stripes' strength, 0-100.\n"
         "scanlines = " << s.scanlines << "\n"
         "mask = " << s.mask << "\n"
         "# on: at the display's refresh, no tearing; off: the lowest latency.\n"
         "vsync = " << on(s.vsync) << "\n"
         "# Frames per second: original (30, as the console), 60, display (the\n"
         "# screen's refresh rate), or unlimited (capped at the refresh rate with vsync).\n"
         "frame_rate = " << (s.frame_rate == Settings::FrameRate::Original ? "original"
                             : s.frame_rate == Settings::FrameRate::Sixty  ? "60"
                             : s.frame_rate == Settings::FrameRate::Display ? "display"
                                                                            : "unlimited") << "\n"
         "# on: with vsync, at most one frame waiting for the screen (the least lag).\n"
         "low_latency = " << on(s.low_latency) << "\n"
         "# Texture filtering: 1 (the console's), 2, 4, 8 or 16.\n"
         "anisotropy = " << s.anisotropy << "\n"
         "# The picture's shape, width:height: auto (the screen's own), 4:3 (the\n"
         "# original), 16:10, 16:9, 64:27 (21:9), 32:9, or any other from 4:3 to 4:1.\n"
         "# Wider than 4:3, the game's cameras see more at the sides.\n"
         "aspect = ";
    if (s.aspect_w && s.aspect_h) o << s.aspect_w << ":" << s.aspect_h;
    else o << "auto";
    o << "\n"
         "# The render resolution: auto (the window's size), or 1 (640x480, the\n"
         "# original) to 4 (1920 lines), its width following the picture's shape.\n"
         "resolution = ";
    if (s.resolution) o << s.resolution;
    else o << "auto";
    o << "\n"
         "# The HUD on pictures wider than 4:3, at its own shape: edges (each part at\n"
         "# the screen's edge it's nearest), middle (the 4:3 area, as on a 4:3 TV), or\n"
         "# stretched (as the game draws it).\n"
         "hud = " << (s.hud == Settings::Hud::Middle ? "middle" : s.hud == Settings::Hud::Stretched ? "stretched" : "edges") << "\n\n";
    o << "[controls]\n"
         "# Controllers use the Xbox controller's layout. These are the keyboard's\n"
         "# keys (SDL's names, several separated by commas; empty for none).\n"
         "rumble = " << on(s.rumble) << "\n";
    for (uint32_t c = 0; c < kControlCount; ++c) {
        o << control_name(Control(c)) << " = ";
        for (size_t k = 0; k < s.keys[c].size(); ++k) o << (k ? ", " : "") << s.keys[c][k];
        o << "\n";
    }
    o << "\n[sound]\n"
         "# 0-100.\n"
         "volume = " << s.volume << "\n"
         "# 0: the device's own (5.1 where it has six speakers), 2: stereo, 6: 5.1. From the next start.\n"
         "channels = " << s.channels << "\n\n";
    o << "[console]\n"
         "# The Xbox's language, which the game uses: 1 English, 3 German, 4 French,\n"
         "# 5 Spanish, 6 Italian. From the next start.\n"
         "language = " << s.language << "\n";
    return o.str();
}

Settings parse_settings(const std::string& text, std::vector<std::string>& warnings) {
    Settings s = default_settings();
    std::istringstream in(text);
    std::string line, section;
    bool aspect_given = false, widescreen = true;  // 0.36's [console] widescreen = off, without an aspect: 4:3
    for (int n = 1; std::getline(in, line); ++n) {
        line = trim(line.substr(0, line.find('#')));
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        const size_t eq = line.find('=');
        auto warn = [&](const std::string& why) { warnings.push_back("line " + std::to_string(n) + ": " + why + " (left at its default)"); };
        if (eq == std::string::npos) {
            warn("not a setting");
            continue;
        }
        const std::string key = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        bool ok = true;
        if (section == "display") {
            if (key == "fullscreen") ok = read_bool(v, &s.fullscreen);
            else if (key == "scaling") {
                if (v == "integer") s.scaling = Settings::Scaling::Integer;
                else if (v == "fit") s.scaling = Settings::Scaling::Fit;
                else if (v == "stretch") s.scaling = Settings::Scaling::Stretch;
                else ok = false;
            } else if (key == "smooth") ok = read_bool(v, &s.smooth);
            else if (key == "fsr") ok = read_bool(v, &s.fsr);
            else if (key == "sharpness") ok = read_uint(v, 0, 100, &s.sharpness);
            else if (key == "antialiasing") ok = v == "smaa" ? (s.smaa = true) : v == "off" ? (s.smaa = false, true) : false;
            else if (key == "look") {
                if (v == "plain") s.look = Settings::Look::Plain;
                else if (v == "crt") s.look = Settings::Look::Crt;
                else ok = false;
            } else if (key == "scanlines") ok = read_uint(v, 0, 100, &s.scanlines);
            else if (key == "mask") ok = read_uint(v, 0, 100, &s.mask);
            else if (key == "vsync") ok = read_bool(v, &s.vsync);
            else if (key == "frame_rate") {
                if (v == "original" || v == "30") s.frame_rate = Settings::FrameRate::Original;
                else if (v == "60") s.frame_rate = Settings::FrameRate::Sixty;
                else if (v == "display") s.frame_rate = Settings::FrameRate::Display;
                else if (v == "unlimited") s.frame_rate = Settings::FrameRate::Unlimited;
                else ok = false;
            } else if (key == "low_latency") ok = read_bool(v, &s.low_latency);
            else if (key == "anisotropy") ok = read_uint(v, 1, 16, &s.anisotropy);
            else if (key == "aspect") ok = aspect_given = read_aspect(v, &s.aspect_w, &s.aspect_h);
            else if (key == "hud") {
                if (v == "edges") s.hud = Settings::Hud::Edges;
                else if (v == "middle") s.hud = Settings::Hud::Middle;
                else if (v == "stretched") s.hud = Settings::Hud::Stretched;
                else ok = false;
            } else if (key == "resolution") ok = v == "auto" ? (s.resolution = 0, true) : read_uint(v, 1, 4, &s.resolution);
            else ok = false;
        } else if (section == "controls") {
            if (key == "rumble") {
                ok = read_bool(v, &s.rumble);
            } else {
                uint32_t c = 0;
                while (c < kControlCount && key != control_name(Control(c))) ++c;
                if (c == kControlCount) {
                    ok = false;
                } else {
                    s.keys[c].clear();
                    std::istringstream list(v);
                    for (std::string k; std::getline(list, k, ',');)
                        if (!trim(k).empty() && !key_reserved(trim(k))) s.keys[c].push_back(trim(k));
                }
            }
        } else if (section == "sound") {
            if (key == "volume") ok = read_uint(v, 0, 100, &s.volume);
            else if (key == "channels") ok = read_uint(v, 0, 6, &s.channels) && (s.channels == 0 || s.channels == 2 || s.channels == 6);
            else ok = false;
        } else if (section == "console") {
            if (key == "language") ok = read_uint(v, 1, 6, &s.language) && s.language != 2;  // 2 Japanese: not on this disc
            else if (key == "widescreen") ok = read_bool(v, &widescreen);
            else ok = false;
        } else {
            ok = false;
        }
        if (!ok) warn("can't read \"" + line + "\"");
    }
    if (!widescreen && !aspect_given) s.aspect_w = 4, s.aspect_h = 3;
    return s;
}

std::filesystem::path settings_file(const std::filesystem::path& folder) { return folder / "ghoulies.ini"; }

Settings load_settings(const std::filesystem::path& folder) {
    const std::filesystem::path path = settings_file(folder);
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        Settings s = default_settings();
        if (!save_settings(folder, s)) std::printf("qilin_run: can't write the settings to %s\n", path.string().c_str());
        return s;
    }
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<std::string> warnings;
    Settings s = parse_settings(text, warnings);
    for (const std::string& w : warnings) std::printf("qilin_run: %s, %s\n", path.string().c_str(), w.c_str());
    return s;
}

bool save_settings(const std::filesystem::path& folder, const Settings& s) {
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::filesystem::path path = settings_file(folder), temp = path.string() + ".new";
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        if (!(f << settings_text(s))) return false;
    }
    std::filesystem::rename(temp, path, ec);
    return !ec;
}

}  // namespace qilin::platform
