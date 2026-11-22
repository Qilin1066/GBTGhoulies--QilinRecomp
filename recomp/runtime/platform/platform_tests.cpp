// Reliability checks without a game disc, window or physical controller.
#include "settings.hpp"
#include "file_store.hpp"
#include "input.hpp"
#include "audio_out.hpp"
#include <SDL3/SDL.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); ++failures; } } while (0)
struct Motors { Uint16 left = 0, right = 0; };
bool SDLCALL rumble(void* user, Uint16 left, Uint16 right) {
    *static_cast<Motors*>(user) = {left, right}; return true;
}
}
int main() {
    using namespace qilin::platform;
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / fs::path(u8"ghoulies-settings-é-保存");
    fs::remove_all(root);
    fs::create_directories(root);
    auto s = default_settings();
    s.volume = 37; s.sound_device = "USB interface";
    CHECK(save_settings(root, s));
    s.volume = 62;
    CHECK(save_settings(root, s)); // replacement, including Windows
    CHECK(load_settings(root).volume == 62);
    CHECK(load_settings(root).sound_device == "USB interface");
    std::vector<std::string> warnings;
    CHECK(parse_settings("\xef\xbb\xbf[sound]\r\nvolume = 27\r\n", warnings).volume == 27 && warnings.empty());
    const auto blocked = root / "blocked";
    fs::create_directory(blocked);
    CHECK(!atomic_write(blocked, "must fail"));
    CHECK(fs::is_directory(blocked));
    std::string unchanged = "untouched";
    CHECK(!read_file(blocked, unchanged) && unchanged == "untouched");
    for (const auto& entry : fs::directory_iterator(root)) CHECK(entry.path().filename().string().find(".tmp.") == std::string::npos);
    // Save-history copies stream beyond a chunk, preserve bytes and replace
    // existing destinations. Failures leave the previous destination intact.
    const auto source = root / fs::path(u8"source-é.bin"), destination = root / "snapshot.bin";
    std::string contents(200003, '\0');
    for (size_t i = 0; i < contents.size(); ++i) contents[i] = char((i * 37 + i / 251) & 255);
    CHECK(atomic_write(source, contents));
    CHECK(atomic_copy(source, destination));
    std::string copied;
    CHECK(read_file(destination, copied) && copied == contents);
    CHECK(atomic_write(source, "new version") && atomic_copy(source, destination));
    CHECK(read_file(destination, copied) && copied == "new version");
    CHECK(!atomic_copy(root / "missing", destination));
    CHECK(read_file(destination, copied) && copied == "new version");
    CHECK(!atomic_copy(blocked, destination) && !atomic_copy(source, blocked));
    CHECK(read_file(destination, copied) && copied == "new version");
    CHECK(atomic_copy(destination, destination));
    CHECK(read_file(destination, copied) && copied == "new version");
    CHECK(atomic_write(source, "") && atomic_copy(source, destination));
    CHECK(fs::file_size(destination) == 0);
    for (const auto& entry : fs::directory_iterator(root)) CHECK(entry.path().filename().string().find(".tmp.") == std::string::npos);
    fs::remove_all(root);

    CHECK(SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS));
    Motors motors;
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "Ghoulies test controller";
    desc.userdata = &motors; desc.Rumble = rumble;
    const auto id = SDL_AttachVirtualJoystick(&desc);
    CHECK(id && SDL_IsGamepad(id));
    {
        Input input;
        CHECK(input.start());
        const auto defaults = default_settings();
        input.set_keys(defaults.keys);
        CHECK(hold_key("Space", true));
        CHECK(input.state(0).analog[kPadA] == 255);
        CHECK(hold_key("Space", false));
        input.rumble(0, 12345, 23456);
        CHECK(motors.left == 12345 && motors.right == 23456);
        input.set_rumble(false);
        CHECK(!motors.left && !motors.right);
        input.set_rumble(true);
        input.rumble(0, 23456, 12345);
        input.stop_rumble(); // settings menu
        CHECK(!motors.left && !motors.right);
        input.rumble(0, 12345, 23456);
    }
    CHECK(!motors.left && !motors.right); // shutdown
    // A pad ignored at the four-port limit is promoted when a port opens.
    {
        std::array<SDL_JoystickID, 4> extra{};
        for (auto& joystick : extra) joystick = SDL_AttachVirtualJoystick(&desc);
        Input input;
        CHECK(input.start() && input.connected_ports() == 15);
        auto pump = [&] { SDL_Event e; while (SDL_PollEvent(&e)) input.on_event(e); };
        pump();
        CHECK(SDL_DetachVirtualJoystick(id));
        pump();
        CHECK(input.connected_ports() == 15); // fifth pad took port 0
        SDL_Joystick* fifth = SDL_OpenJoystick(extra.back());
        CHECK(fifth != nullptr);
        if (fifth) {
            CHECK(SDL_SetJoystickVirtualButton(fifth, SDL_GAMEPAD_BUTTON_SOUTH, true));
            SDL_UpdateJoysticks(); pump();
            CHECK(input.state(0).analog[kPadA] == 255);
            CHECK(input.state(1).analog[kPadA] == 0);
            SDL_CloseJoystick(fifth);
        }
        for (auto joystick : extra) CHECK(SDL_DetachVirtualJoystick(joystick));
        pump();
        CHECK(input.connected_ports() == 1); // only the keyboard remains
    }
    SDL_Quit();

    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "not-a-real-driver");
    {
        AudioOut output;
        CHECK(!output.start(0, 2));
        CHECK(!output.start(48000, 4));
        CHECK(!output.start(48000, 2)); // initial device outage
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
        SDL_Delay(1100);

        float samples[960] = {};
        output.push(samples, 480);
        CHECK(output.channels() == 2 && output.pushed() == 480 && output.queued_frames() >= 0);
    }
    CHECK(!SDL_WasInit(SDL_INIT_AUDIO)); // balanced subsystem ownership
    return failures ? 1 : 0;
}
