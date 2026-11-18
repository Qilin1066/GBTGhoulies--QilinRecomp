// The game's window, through SDL3. It holds no Vulkan state: the renderer
// asks it for the instance extensions a surface needs, Vulkan's loader entry
// point (so the renderer and SDL load the same library) and the surface.
//
// SDL's events are pumped from the thread that created the window, which is
// the game's main thread: macOS takes events only on the process's first
// thread, and the runtime runs the game there.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct SDL_Window;

namespace qilin::platform {

class Input;

class Window {
public:
    // A window for a picture of width × height, at the largest whole
    // multiple of that size (in points, so scaled on high-DPI displays) that
    // fits the main display, titled `title`, ready for a Vulkan surface. Null with *error
    // set where there's no display, or no Vulkan library SDL can load.
    static std::unique_ptr<Window> create(const std::string& title, uint32_t width, uint32_t height, bool fullscreen,
                                          std::string* error);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    std::vector<const char*> vulkan_instance_extensions() const;  // what a surface needs, valid while the window lives
    void* vulkan_loader() const;                                   // PFN_vkGetInstanceProcAddr, as SDL loaded it
    uint64_t create_surface(void* instance, std::string* error) const;  // a VkSurfaceKHR, 0 on failure

    // The drawable area in pixels, which on high-DPI displays is more than the size in points.
    void pixel_size(uint32_t* width, uint32_t* height) const;
    bool minimized() const;

    // Handles what happened since the last call: closing the window asks the
    // runtime to stop (`quit_requested`), F11 and Alt+Enter switch fullscreen;
    // every event goes on to `input` too, if given (controllers coming and going).
    void pump(Input* input = nullptr);
    bool quit_requested() const { return quit_; }

private:
    Window() = default;
    ::SDL_Window* window_ = nullptr;
    bool quit_ = false;
};

}  // namespace qilin::platform
