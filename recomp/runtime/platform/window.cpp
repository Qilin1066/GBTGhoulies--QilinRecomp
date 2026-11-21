#include "window.hpp"

#include "input.hpp"

#include <algorithm>
#include <SDL3/SDL.h>

#include <cstdio>
#define SDL_MAIN_HANDLED  // the runtime's own main() starts everything; SDL is told it's ready
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_vulkan.h>

#include <cstdlib>

namespace qilin::platform {
namespace {

bool g_sdl_video = false;

// SDL keeps display modes and the like until SDL_Quit, so it runs at exit
// however the process ends (the runtime stops with std::exit).
bool start_video(std::string* error) {
    if (g_sdl_video) return true;
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_APP_NAME, "Grabbed by the Ghoulies");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        *error = std::string("can't start SDL's video: ") + SDL_GetError();
        return false;
    }
    g_sdl_video = true;
    std::atexit([] { SDL_Quit(); });
    return true;
}

}  // namespace

std::unique_ptr<Window> Window::create(const std::string& title, uint32_t width, uint32_t height, bool fullscreen,
                                       std::string* error) {
    if (!start_video(error)) return nullptr;
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN;  // borderless, at the desktop's mode
    // As large as fits nine tenths of the main display's usable area, at the
    // size's shape (the picture's): a whole multiple of the game's 480 lines
    // would leave a 16:9 picture at 854×480 on a 1080p screen.
    uint32_t w0 = width, h0 = height;
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) && width && height) {
        const double fit = std::min(double(usable.w) * 0.9 / width, double(usable.h) * 0.9 / height);
        if (fit > 1.0) w0 = uint32_t(width * fit), h0 = uint32_t(height * fit);
    }
    std::unique_ptr<Window> w(new Window);
    w->window_ = SDL_CreateWindow(title.c_str(), int(w0), int(h0), flags);
    if (!w->window_) {
        *error = std::string("can't open a window: ") + SDL_GetError();
        return nullptr;
    }
    SDL_SetWindowMinimumSize(w->window_, 160, 120);
    return w;
}

Window::~Window() {
    if (window_) SDL_DestroyWindow(window_);
}

std::vector<const char*> Window::vulkan_instance_extensions() const {
    Uint32 n = 0;
    const char* const* names = SDL_Vulkan_GetInstanceExtensions(&n);
    return names ? std::vector<const char*>(names, names + n) : std::vector<const char*>{};
}

void* Window::vulkan_loader() const { return reinterpret_cast<void*>(SDL_Vulkan_GetVkGetInstanceProcAddr()); }

uint64_t Window::create_surface(void* instance, std::string* error) const {
    VkSurfaceKHR surface{};
    if (!SDL_Vulkan_CreateSurface(window_, static_cast<VkInstance>(instance), nullptr, &surface)) {
        *error = std::string("can't make a Vulkan surface for the window: ") + SDL_GetError();
        return 0;
    }
    return uint64_t(surface);
}

namespace {
float desktop_aspect(SDL_DisplayID display) {
    const SDL_DisplayMode* mode = display ? SDL_GetDesktopDisplayMode(display) : nullptr;
    return mode && mode->w > 0 && mode->h > 0 ? float(mode->w) / float(mode->h) : 0.0f;
}
}  // namespace

float Window::screen_aspect() const { return desktop_aspect(SDL_GetDisplayForWindow(window_)); }

float Window::refresh_rate() const {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window_));
    return mode && mode->refresh_rate > 0 ? mode->refresh_rate : 0.0f;
}

float Window::main_screen_aspect() {
    std::string error;
    return start_video(&error) ? desktop_aspect(SDL_GetPrimaryDisplay()) : 0.0f;
}

void Window::pixel_size(uint32_t* width, uint32_t* height) const {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    *width = uint32_t(w > 0 ? w : 0);
    *height = uint32_t(h > 0 ? h : 0);
}

bool Window::minimized() const { return (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0; }

bool Window::fullscreen() const { return (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0; }
void Window::set_fullscreen(bool on) {
    if (on != fullscreen()) SDL_SetWindowFullscreen(window_, on);
}

void Window::pump(Input* input, const std::function<bool(const SDL_Event&)>& hook) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (input) input->on_event(e);  // controllers coming and going, whatever else sees it
        if (hook && hook(e)) continue;
        switch (e.type) {
        // Sound devices coming and going, for the log (a stop in the sound
        // with one of these near it is the device's doing).
        case SDL_EVENT_AUDIO_DEVICE_ADDED:
        case SDL_EVENT_AUDIO_DEVICE_REMOVED:
        case SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED:
            if (!e.adevice.recording) {
                const char* name = SDL_GetAudioDeviceName(e.adevice.which);
                std::printf("qilin_run: sound device %s: %s\n",
                            e.type == SDL_EVENT_AUDIO_DEVICE_ADDED     ? "added"
                            : e.type == SDL_EVENT_AUDIO_DEVICE_REMOVED ? "removed"
                                                                       : "format changed",
                            name ? name : "(unnamed)");
                std::fflush(stdout);
            }
            break;
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: quit_ = true; break;
        case SDL_EVENT_KEY_DOWN:
            if (!e.key.repeat && (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))))
                SDL_SetWindowFullscreen(window_, !(SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN));
            break;
        default: break;
        }
    }
}

}  // namespace qilin::platform
