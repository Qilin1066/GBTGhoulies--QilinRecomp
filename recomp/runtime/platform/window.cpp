#include "window.hpp"

#include "input.hpp"

#include <SDL3/SDL.h>
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
    // The largest whole multiple of the size that fits the main display's
    // usable area, so the picture is big and its pixels even.
    uint32_t scale = 1;
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) && width && height)
        while (width * (scale + 1) <= uint32_t(usable.w) * 9 / 10 && height * (scale + 1) <= uint32_t(usable.h) * 9 / 10) ++scale;
    std::unique_ptr<Window> w(new Window);
    w->window_ = SDL_CreateWindow(title.c_str(), int(width * scale), int(height * scale), flags);
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
