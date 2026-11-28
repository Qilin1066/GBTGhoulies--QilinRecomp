#include "audio_out.hpp"
#include "path.hpp"
#include "menu.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "gfx.hpp"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#include "input.hpp"
#include "window.hpp"

#ifndef QILIN_VERSION
#define QILIN_VERSION "?"
#endif

namespace qilin::ui {
namespace {

VkFormat g_format = VK_FORMAT_UNDEFINED;  // the window's, which ImGui's pipeline info points at while it lives

void check_vk(VkResult r) {
    if (r != VK_SUCCESS) std::fprintf(stderr, "qilin: Vulkan error %d in the settings menu\n", int(r));
}

constexpr const char* kHint = "F1 or Esc (controller: Back + Start): settings";

struct Language {
    uint32_t value;
    const char* name;
};
constexpr Language kLanguages[] = {{1, "English"}, {3, "Deutsch"}, {4, "Fran\xc3\xa7" "ais"}, {5, "Espa\xc3\xb1ol"}, {6, "Italiano"}};

}  // namespace

Menu::Menu(platform::Window& window, gfx::Renderer& renderer, platform::Settings& settings, std::filesystem::path folder,
           std::function<void(const platform::Settings&)> apply)
    : window_(window), renderer_(renderer), s_(settings), folder_(std::move(folder)), apply_(std::move(apply)) {
    gfx::Overlay o;
    if (!renderer_.overlay(o)) return;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no layout file: the settings are ghoulies.ini
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    // Readable at the window's size: the menu was laid out for 720 lines.
    int w = 0, h = 0;
    SDL_GetWindowSize(window_.sdl(), &w, &h);
    const float scale = std::clamp(float(h) / 720.0f, 1.0f, 3.0f) * 1.25f;
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleMain = scale;
    if (!ImGui_ImplSDL3_InitForVulkan(window_.sdl())) return;
    sdl_backend_ = true;
    // Every controller works the menu, not only the first one SDL lists
    // (which can be a virtual pad, or a wheel's base).
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoAll);
    g_format = VkFormat(o.format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = static_cast<VkInstance>(o.instance);
    info.PhysicalDevice = static_cast<VkPhysicalDevice>(o.physical_device);
    info.Device = static_cast<VkDevice>(o.device);
    info.QueueFamily = o.queue_family;
    info.Queue = static_cast<VkQueue>(o.queue);
    info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE;
    info.MinImageCount = std::max<uint32_t>(2, o.min_images);
    info.ImageCount = std::max(info.MinImageCount, o.images);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = VkPipelineRenderingCreateInfo{};
    info.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_format;
    info.CheckVkResultFn = check_vk;
    if (!ImGui_ImplVulkan_Init(&info)) return;
    vulkan_backend_ = true;
    ok_ = true;
    hint_until_ = SDL_GetTicks() + 10000;
}

Menu::~Menu() {
    renderer_.set_overlay({});
    if (vulkan_backend_) {
        gfx::Overlay o;
        if (renderer_.overlay(o)) vkDeviceWaitIdle(static_cast<VkDevice>(o.device));
        ImGui_ImplVulkan_Shutdown();
    }
    if (sdl_backend_) ImGui_ImplSDL3_Shutdown();
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
}

void Menu::changed() {
    if (apply_) apply_(s_);
}

bool Menu::handle(const SDL_Event& e) {
    if (!ok_) return false;
    // Looking at the picture: any key or button brings the menu back.
    if (peek_ && (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)) {
        peek_ = false;
        return true;
    }
    // Setting a control's key: the next key pressed (Escape gives up).
    if (capture_ >= 0 && e.type == SDL_EVENT_KEY_DOWN) {
        if (!e.key.repeat) {
            const char* name = SDL_GetScancodeName(e.key.scancode);
            if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
                capture_ = -1, note_ = "Unchanged.";
            } else if (name && *name && !platform::key_reserved(name)) {
                const std::string key = name;
                note_.clear();
                for (uint32_t c = 0; c < platform::kControlCount; ++c) {  // a key presses one control
                    auto& list = s_.keys[c];
                    if (std::erase(list, key) && int(c) != capture_)
                        note_ = key + " moved here from " + platform::control_label(platform::Control(c)) + ".";
                }
                s_.keys[capture_].push_back(key);
                capture_ = -1;
                changed();
            }
        }
        return true;
    }
    switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
            if (!e.key.repeat && (e.key.key == SDLK_F1 || e.key.key == SDLK_ESCAPE)) {
                toggle();
                return true;
            }
            if (!e.key.repeat && e.key.key == SDLK_F9) {  // a capture of the next frames, for a report
                capture_frames_ = true;
                return true;
            }
            if (!e.key.repeat && e.key.key == SDLK_F3) {  // the frame rate counter, kept in the settings
                s_.performance_overlay = !s_.performance_overlay;
                if (!platform::save_settings(folder_, s_))
                    std::printf("qilin_run: the settings can't be saved to %s\n", qilin::utf8_path(platform::settings_file(folder_)).c_str());
                return true;
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
            // Back and Start together (alone they're the game's); with the
            // menu open, Start alone closes it too.
            SDL_Gamepad* pad = SDL_GetGamepadFromID(e.gbutton.which);
            const bool back = pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK);
            const bool start = pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START);
            if ((back && start) || (open_ && e.gbutton.button == SDL_GAMEPAD_BUTTON_START)) {
                toggle();
                return true;
            }
            break;
        }
        default: break;
    }
    if (open_) ImGui_ImplSDL3_ProcessEvent(&e);
    if (!open_) return false;
    switch (e.type) {  // the game doesn't see input while the menu is up
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_TEXT_INPUT:
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_WHEEL:
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: return true;
        default: return false;
    }
}

void Menu::toggle() {
    if (open_) {
        close();
        return;
    }
    open_ = true;
    hint_until_ = 0;
    ImGui::GetIO().ClearInputKeys();
}

void Menu::close() {
    s_.fullscreen = window_.fullscreen();
    if (!platform::save_settings(folder_, s_))
        std::printf("qilin_run: the settings can't be saved to %s\n", qilin::utf8_path(platform::settings_file(folder_)).c_str());
    open_ = peek_ = false;
    capture_ = -1;
    note_.clear();
}

void Menu::tick() {
    if (!ok_) return;
    const uint64_t now = SDL_GetTicks();
    if (capture_ >= 0 && now >= capture_until_) capture_ = -1, note_ = "Nothing was pressed: unchanged.";
    const bool hint = !open_ && (now < hint_until_ || now < banner_until_);
    const bool counter = !open_ && s_.performance_overlay;
    if (!open_ && !hint && !counter) {
        if (drawing_) renderer_.set_overlay({}), drawing_ = false;
        return;
    }
    if (!drawing_) {
        renderer_.set_overlay([](void* cmd) {
            if (ImDrawData* data = ImGui::GetDrawData()) ImGui_ImplVulkan_RenderDrawData(data, static_cast<VkCommandBuffer>(cmd));
        });
        drawing_ = true;
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    if (counter) draw_frame_times();
    if (hint) draw_hint();
    else if (open_ && !peek_) draw_menu();
    ImGui::Render();
    if (close_asked_) close_asked_ = false, close();
}

void Menu::banner(std::string text) {
    banner_ = std::move(text);
    banner_until_ = SDL_GetTicks() + 6000;
}

void Menu::frame_shown(double ms, std::vector<std::string> details) {
    frame_times_.add(ms);
    frame_details_ = std::move(details);
}

// F3: how fast and how evenly frames reach the screen, top left. The lows
// are the frame rates of the slowest 1% and 0.1% of frames over the last
// 20 s (as PunchOutWii--QilinRecomp's counter).
void Menu::draw_frame_times() {
    const FrameTimes::Summary s = frame_times_.summary();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 8.0f, vp->WorkPos.y + 8.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##frame times", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    if (s.frames == 0) {
        ImGui::TextUnformatted("frame times: waiting for frames");
    } else {
        ImGui::Text("%.1f fps  %.2f ms", s.fps, s.average_ms);
        ImGui::Text("1%% low %.1f  0.1%% low %.1f", s.low1_fps, s.low01_fps);
        ImGui::Text("worst %.1f ms", s.worst_ms);
        const std::vector<float> recent = frame_times_.recent(240);
        ImGui::PlotLines("##ft", recent.data(), int(recent.size()), 0, nullptr, 0.0f, 50.0f, ImVec2(240.0f, 48.0f));
    }
    for (const std::string& line : frame_details_) ImGui::TextUnformatted(line.c_str());
    ImGui::End();
}

void Menu::draw_hint() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 12, vp->WorkPos.y + vp->WorkSize.y - 12), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##hint", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted(SDL_GetTicks() < banner_until_ ? banner_.c_str() : kHint);
    ImGui::End();
}

void Menu::draw_menu() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("Grabbed by the Ghoulies - settings (the game is paused)");
    // Close and Quit, top right on every page, as in the other recomps.
    const ImGuiStyle& st = ImGui::GetStyle();
    const float buttons = ImGui::CalcTextSize("Close (Esc)").x + ImGui::CalcTextSize("Quit the game").x + 4 * st.FramePadding.x + st.ItemSpacing.x;
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - buttons + ImGui::GetCursorPosX());
    if (ImGui::Button("Close (Esc)")) close_asked_ = true;
    ImGui::SameLine();
    if (ImGui::Button("Quit the game")) {
        close();
        quit_ = true;
    }
    ImGui::Separator();
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
    if (ImGui::BeginTabBar("pages")) {
        if (ImGui::BeginTabItem("Controls")) {
            controls_page();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Display")) {
            display_page();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Game")) {
            game_page();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::PopItemWidth();
    ImGui::End();
}

// The picture's shape: the game's cameras widen to it (see host/aspect.cpp).
void Menu::aspect_combo() {
    struct Shape {
        uint32_t w, h;
        const char* name;
    };
    static const Shape kShapes[] = {
        {0, 0, "Auto (the screen's shape)"}, {4, 3, "4:3 (the original)"}, {16, 10, "16:10"},
        {16, 9, "16:9 (the console's widescreen)"}, {64, 27, "21:9 ultrawide"}, {32, 9, "32:9 super ultrawide"},
    };
    std::string current = std::to_string(s_.aspect_w) + ":" + std::to_string(s_.aspect_h);
    for (const Shape& k : kShapes)
        if (k.w == s_.aspect_w && k.h == s_.aspect_h) current = k.name;
    if (ImGui::BeginCombo("Aspect ratio", current.c_str())) {
        for (const Shape& k : kShapes)
            if (ImGui::Selectable(k.name, k.w == s_.aspect_w && k.h == s_.aspect_h)) s_.aspect_w = k.w, s_.aspect_h = k.h, changed();
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(wider: more at the sides)");

    // The render resolution, with the size it gives at the shape shown.
    const float aspect = renderer_.picture().aspect;
    auto size_of = [&](uint32_t lines) {
        platform::Settings t = s_;
        t.resolution = lines;
        uint32_t sx = 1, sy = 1;
        platform::resolution_scale(t, aspect, 480 * lines, &sx, &sy);
        return std::to_string(640 * sx) + "x" + std::to_string(480 * sy);
    };
    const std::string now = std::to_string(640 * renderer_.resolution_scale_x()) + "x" + std::to_string(480 * renderer_.resolution_scale_y());
    const std::string names[5] = {"Auto (the window's size: " + now + ")", "1x, " + size_of(1) + " (the original)", "2x, " + size_of(2),
                                  "3x, " + size_of(3), "4x, " + size_of(4)};
    {
        static const char* const kHud[] = {"At the screen's edges", "In the middle 4:3 (as on a 4:3 TV)", "Stretched (as the game draws it)"};
        int hud = int(s_.hud);
        if (ImGui::Combo("HUD (wider than 4:3)", &hud, kHud, 3)) s_.hud = platform::Settings::Hud(hud), changed();
    }
    if (ImGui::BeginCombo("Render resolution", names[std::min<uint32_t>(s_.resolution, 4)].c_str())) {
        for (uint32_t k = 0; k <= 4; ++k)
            if (ImGui::Selectable(names[k].c_str(), s_.resolution == k)) s_.resolution = k, changed();
        ImGui::EndCombo();
    }
}

void Menu::display_page() {
    ImGui::SeparatorText("Window");
    bool full = window_.fullscreen();
    if (ImGui::Checkbox("Full screen (F11 or Alt+Enter)", &full)) {
        window_.set_fullscreen(full);
        s_.fullscreen = full;
    }
    ImGui::SeparatorText("Picture");
    aspect_combo();
    int scaling = int(s_.scaling);
    bool any = false;
    any |= ImGui::RadioButton("Whole multiples (sharpest)", &scaling, int(platform::Settings::Scaling::Integer));
    ImGui::SameLine();
    any |= ImGui::RadioButton("Fit (keeps its shape)", &scaling, int(platform::Settings::Scaling::Fit));
    ImGui::SameLine();
    any |= ImGui::RadioButton("Fill the window", &scaling, int(platform::Settings::Scaling::Stretch));
    if (any) s_.scaling = platform::Settings::Scaling(scaling), changed();
    if (ImGui::Checkbox("Smooth (bilinear; off: sharp pixels)", &s_.smooth)) changed();
    if (ImGui::Checkbox("SMAA anti-aliasing", &s_.smaa)) changed();
    ImGui::SameLine();
    ImGui::TextDisabled("(smooths jagged edges; with a higher render resolution, smoother still)");
    if (ImGui::Checkbox("AMD FSR 1 upscaling", &s_.fsr)) changed();
    ImGui::SameLine();
    ImGui::TextDisabled(s_.scaling == platform::Settings::Scaling::Integer ? "(not at whole multiples)"
                                                                           : "(where the picture is shown larger than it's drawn)");
    if (s_.fsr) {
        int sharp = int(s_.sharpness);
        if (ImGui::SliderInt("FSR sharpness", &sharp, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) s_.sharpness = uint32_t(sharp), changed();
    }
    int look = int(s_.look);
    any = ImGui::RadioButton("Plain", &look, int(platform::Settings::Look::Plain));
    ImGui::SameLine();
    any |= ImGui::RadioButton("CRT television", &look, int(platform::Settings::Look::Crt));
    if (any) s_.look = platform::Settings::Look(look), changed();
    if (s_.look == platform::Settings::Look::Crt) {
        int scan = int(s_.scanlines), mask = int(s_.mask);
        if (ImGui::SliderInt("Scanlines", &scan, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) s_.scanlines = uint32_t(scan), changed();
        if (ImGui::SliderInt("Phosphor stripes", &mask, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) s_.mask = uint32_t(mask), changed();
    }
    if (ImGui::Checkbox("Vsync (off: the lowest latency, may tear)", &s_.vsync)) changed();
    {
        static const char* const kRates[] = {"30 (original)", "60", "The display's refresh rate", "Unlimited"};
        int rate = int(s_.frame_rate);
        if (ImGui::Combo("Frame rate", &rate, kRates, 4)) s_.frame_rate = platform::Settings::FrameRate(rate), changed();
    }
    if (ImGui::Checkbox("Low latency (with vsync: at most one frame waiting)", &s_.low_latency)) changed();
    if (ImGui::Checkbox("Frame times (F3)", &s_.performance_overlay)) changed();
    if (ImGui::Button("Look at the picture")) peek_ = true;
    ImGui::SameLine();
    ImGui::TextDisabled("(any key or button comes back)");

    ImGui::SeparatorText("Enhancements");
    const uint32_t most = renderer_.max_anisotropy();
    if (most > 1) {
        const std::string label = s_.anisotropy <= 1 ? std::string("Off (the console's)") : std::to_string(s_.anisotropy) + "x";
        if (ImGui::BeginCombo("Anisotropic filtering", label.c_str())) {
            for (uint32_t level = 1; level <= most; level *= 2)
                if (ImGui::Selectable(level == 1 ? "Off (the console's)" : (std::to_string(level) + "x").c_str(), s_.anisotropy == level))
                    s_.anisotropy = level, changed();
            ImGui::EndCombo();
        }
    } else {
        ImGui::TextDisabled("Anisotropic filtering: this GPU doesn't have it.");
    }
    ImGui::SeparatorText("Graphics device");
    ImGui::TextDisabled("In use: %s.", renderer_.device_name().c_str());
}

void Menu::controls_page() {
    ImGui::SeparatorText("Controllers");
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (n == 0) ImGui::TextDisabled("None connected: the keyboard plays controller 1.");
    for (int i = 0; i < n; ++i) {
        const char* name = SDL_GetGamepadNameForID(ids[i]);
        ImGui::BulletText("%s", name ? name : "A controller");
    }
    SDL_free(ids);
    if (ImGui::Checkbox("Rumble", &s_.rumble)) changed();
    ImGui::SeparatorText("Keys");
    ImGui::TextWrapped("Controllers use the Xbox controller's layout (Black and White are the shoulder buttons). "
                       "The keyboard is controller 1; click a control's keys to add one, right-click a key to remove it.");
    if (!note_.empty()) ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), "%s", note_.c_str());
    if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthFixed, ImGui::GetContentRegionAvail().x * 0.3f);
        ImGui::TableSetupColumn("Keys");
        for (uint32_t c = 0; c < platform::kControlCount; ++c) {
            ImGui::PushID(int(c));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(platform::control_label(platform::Control(c)));
            ImGui::TableSetColumnIndex(1);
            auto& list = s_.keys[c];
            for (size_t k = 0; k < list.size(); ++k) {
                ImGui::PushID(int(k));
                ImGui::SmallButton(list[k].c_str());
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    list.erase(list.begin() + long(k));
                    changed();
                    ImGui::PopID();
                    break;
                }
                ImGui::SameLine();
                ImGui::PopID();
            }
            if (capture_ == int(c)) {
                ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), "Press a key (Esc: cancel)");
            } else if (ImGui::SmallButton("Add")) {
                capture_ = int(c);
                capture_until_ = SDL_GetTicks() + 5000;
                note_.clear();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Back to the defaults")) {
        platform::default_keys(s_);
        note_.clear();
        changed();
    }
    ImGui::TextDisabled("F1 or Esc (controller: Back + Start): this menu. F3: frame times. F9: capture frames for a report. F11 or Alt+Enter: full screen.");
}

void Menu::game_page() {
    ImGui::SeparatorText("Sound and language");
    int volume = int(s_.volume);
    if (ImGui::SliderInt("Volume", &volume, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) s_.volume = uint32_t(volume), changed();
    static const char* kChannels[] = {"The device's own (5.1 where it has six speakers)", "Stereo", "5.1"};
    int channels = s_.channels == 2 ? 1 : s_.channels == 6 ? 2 : 0;
    if (ImGui::Combo("Sound output (from the next start)", &channels, kChannels, 3)) s_.channels = channels == 1 ? 2 : channels == 2 ? 6 : 0;
    // The playback device: the system's default (followed when it changes:
    // a controller with a headset socket, a streaming app or a monitor can
    // take the default over while the game runs), or one by name.
    static const std::vector<std::string> devices = platform::AudioOut::playback_devices();
    if (ImGui::BeginCombo("Sound device (from the next start)", s_.sound_device.empty() ? "The system's default" : s_.sound_device.c_str())) {
        if (ImGui::Selectable("The system's default", s_.sound_device.empty())) s_.sound_device.clear(), changed();
        for (const std::string& name : devices)
            if (ImGui::Selectable(name.c_str(), s_.sound_device == name)) s_.sound_device = name, changed();
        ImGui::EndCombo();
    }
    const char* current = "English";
    for (const Language& l : kLanguages)
        if (l.value == s_.language) current = l.name;
    if (ImGui::BeginCombo("Language (from the next start)", current)) {
        for (const Language& l : kLanguages)
            if (ImGui::Selectable(l.name, l.value == s_.language)) s_.language = l.value;
        ImGui::EndCombo();
    }

    ImGui::SeparatorText("About");
    ImGui::TextWrapped("Grabbed by the Ghoulies QilinRecomp %s: a static recompilation, the game's own code compiled for this computer.",
                       QILIN_VERSION);
    ImGui::TextWrapped("Graphics: Vulkan. Window, sound and input: SDL3 (zlib). Menu: Dear ImGui (MIT).");
    ImGui::TextWrapped("Settings file: %s", qilin::utf8_path(platform::settings_file(folder_)).c_str());
}

}  // namespace qilin::ui
