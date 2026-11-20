// The settings menu (Dear ImGui over the game's window), as ApbArcade-QilinRecomp and
// PunchOutWii-QilinRecomp have theirs: F1 or Esc, or Back and Start together on a
// controller, open it, and the game pauses under it. Its pages are the
// settings file's sections (platform/settings.hpp); closing it saves them.
// A hint names the keys for the first seconds of a run.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "settings.hpp"

union SDL_Event;

namespace qilin::gfx {
class Renderer;
}
namespace qilin::platform {
class Window;
class Input;
}  // namespace qilin::platform

namespace qilin::ui {

class Menu {
public:
    // `apply` puts settings into effect (the renderer's picture, the sound's
    // volume, the controls); it's called whenever one changes.
    Menu(platform::Window& window, gfx::Renderer& renderer, platform::Settings& settings, std::filesystem::path folder,
         std::function<void(const platform::Settings&)> apply);
    ~Menu();
    Menu(const Menu&) = delete;
    Menu& operator=(const Menu&) = delete;

    bool ok() const { return ok_; }  // the menu can draw (a window that takes a UI)
    bool open() const { return open_; }
    bool quit_requested() const { return quit_; }

    // An event from the window, first: true if the menu takes it (the game
    // doesn't see keys and buttons while the menu is up).
    bool handle(const SDL_Event& e);
    // Builds what's drawn over the next frame shown (the menu, or the hint),
    // or nothing.
    void tick();

private:
    void toggle();
    void close();
    void draw_menu();
    void draw_hint();
    void controls_page();
    void display_page();
    void game_page();
    void changed();

    platform::Window& window_;
    gfx::Renderer& renderer_;
    platform::Settings& s_;
    std::filesystem::path folder_;
    std::function<void(const platform::Settings&)> apply_;
    bool ok_ = false, sdl_backend_ = false, vulkan_backend_ = false;
    bool open_ = false, quit_ = false, close_asked_ = false, peek_ = false, drawing_ = false;
    uint64_t hint_until_ = 0;
    int capture_ = -1;            // the control waiting for a key, or -1
    uint64_t capture_until_ = 0;  // when waiting gives up
    std::string note_;
};

}  // namespace qilin::ui
