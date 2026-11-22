#include "command_line.hpp"
// qilin_run: loads default.xbe into guest memory and runs the game's lifted
// main() until it returns or reaches something the runtime doesn't provide
// yet, which it names. Built with the generated code (-DQILIN_GEN_DIR=...).
//
// Ghoulies' XBE entry point (0x14FAE9) is XAPI start-up code. It creates the
// main thread, whose procedure (0x14FA75) initialises XAPI, runs the
// initializer tables (0x15172C, 0x1516D4) and then calls the game's
// main(0, 0, 0) at 0x32390. The runtime replaces XAPI's part: it sets up
// memory, the stack and the thread information block. The C runtime is lifted,
// so its initializers run as XAPI ran them, in the same order; entries that
// belong to libraries replaced natively (XGRPH, DSOUND) are skipped, their
// work done by the native replacements.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string_view>

#include "regions.hpp"
#include "host.hpp"
#include "settings.hpp"
#include "sched.hpp"
#include "xbe.hpp"
#include "audio.hpp"
#include "audio_out.hpp"
#include "input.hpp"
#include "window.hpp"

using namespace qilin;

namespace {

constexpr uint32_t kGameMain = 0x32390;

void push(rt::Context& c, uint32_t v) {
    c.esp -= 4;
    rt::st32(rt::Mem{c.mem, c.mem_size}, c.esp, v, 0);
}

// Where the hard disk's partitions live: the platform's per-user data
// directory, under qilin/ghoulies.
std::filesystem::path default_data_dir() {
    auto env = [](const char* n) -> std::filesystem::path {
#ifdef _WIN32
        const std::wstring key(n, n + std::strlen(n));
        const wchar_t* v = _wgetenv(key.c_str());
#else
        const char* v = std::getenv(n);
#endif
        return v && *v ? std::filesystem::path(v) : std::filesystem::path();
    };
#if defined(_WIN32)
    std::filesystem::path base = env("LOCALAPPDATA");
#elif defined(__APPLE__)
    std::filesystem::path base = env("HOME");
    if (!base.empty()) base /= "Library/Application Support";
#else
    std::filesystem::path base = env("XDG_DATA_HOME");
    if (base.empty() && !env("HOME").empty()) base = env("HOME") / ".local/share";
#endif
    if (base.empty()) base = ".";
    return base / "qilin" / "ghoulies";
}

using host::call_guest;

// One initializer table, [lo, hi): null and -1 entries are empty slots.
void run_table(rt::Context& c, uint32_t lo, uint32_t hi) {
    const rt::Mem m{c.mem, c.mem_size};
    for (uint32_t a = lo; a < hi; a += 4) {
        const uint32_t fn = rt::ld32(m, a, 0);
        if (fn == 0 || fn == 0xFFFFFFFFu) continue;
        if (!rt::is_lifted(fn)) {
            std::printf("qilin_run: initializer %08X (%s) skipped: its library is replaced natively\n", fn, host::describe(fn).c_str());
            continue;
        }
        call_guest(c, fn, {});
    }
}

// The XBE's TLS directory (header +0x12C): StartAddressOfRawData,
// EndAddressOfRawData, AddressOfIndex, AddressOfCallBacks, SizeOfZeroFill.
// A thread's TLS block is a pointer to its data, then the data: the template
// and the zero fill, rounded up to 16 bytes, as the entry point sizes it.
uint32_t tls_directory(const rt::Mem& m) { return rt::ld32(m, 0x1012C, 0); }
uint32_t tls_size(const rt::Mem& m) {
    const uint32_t dir = tls_directory(m);
    if (!dir) return 0;
    auto ld = [&](uint32_t a) { return rt::ld32(m, a, 0); };
    return ((ld(dir + 16) - ld(dir) + ld(dir + 4) + 15) & ~15u) + 4;
}

// What the XBE entry point (0x14FAE9) does before it starts the main thread:
// fix up the certificate's size field, then store the TLS block's size
// (0x549528) and the TLS index, which makes [StackBase + 4 * index] the block.
void entry_prologue(rt::Context& c) {
    const rt::Mem m{c.mem, c.mem_size};
    auto ld = [&](uint32_t a) { return rt::ld32(m, a, 0); };
    const uint32_t cert = ld(0x10118);
    const uint32_t need = ld(0x10108) - cert + 0x10000;
    if (need < ld(cert)) rt::st32(m, cert, need, 0);
    const uint32_t tls = tls_size(m);
    rt::st32(m, 0x549528, tls, 0);
    rt::st32(m, ld(tls_directory(m) + 8), uint32_t(int32_t(tls) / -4), 0);
}

// What XAPI's thread start routine (0x14BB4D) does first, on the TLS block the
// kernel put at the top of the thread's stack (KTHREAD.TlsData): point its
// first word at the data after it, copy the template there and zero the fill.
void init_tls(rt::Context& c, uint32_t tls_data) {
    const rt::Mem m{c.mem, c.mem_size};
    auto ld = [&](uint32_t a) { return rt::ld32(m, a, 0); };
    const uint32_t dir = tls_directory(m);
    const uint32_t start = ld(dir), raw = ld(dir + 4) - start, zero = ld(dir + 16);
    rt::st32(m, tls_data, tls_data + 4, 0);
    for (uint32_t k = 0; k < raw; ++k) rt::st8(m, tls_data + 4 + k, rt::ld8(m, start + k, 0), 0);
    for (uint32_t k = 0; k < zero; ++k) rt::st8(m, tls_data + 4 + raw + k, 0, 0);
}

// Start-up, as XAPI's main-thread procedure (0x14FA75) does it: XAPI's
// process initialisation (0x151011: heap, drive letters; its kernel-patching
// step 0x1517B9 is left out), then the C runtime (0x14FAC1).
void crt_startup(rt::Context& c) {
    const rt::Mem m{c.mem, c.mem_size};
    entry_prologue(c);
    call_guest(c, 0x151011, {});
    run_table(c, 0x32C800, 0x32C80C);                            // _mtinit, _ioinit
    if (const uint32_t fpinit = rt::ld32(m, 0x337A88, 0))        // _FPinit: float formatting, x87 set-up
        call_guest(c, fpinit, {1});                                // XAPI's `call eax` leaves a non-zero word there
    run_table(c, 0x32C838, 0x32C84C);
    run_table(c, 0x32C810, 0x32C834);
}

}  // namespace

int main(int argc, char** argv) {
    const qilin::CommandLine command_line(argc, argv);
    argc = command_line.count(); argv = command_line.values();
    const char* path = nullptr;
    const char* game_dir = nullptr;
    const char* data_dir = nullptr;
    host::D3dConfig d3d;
    std::string dump_audio;
    bool headless = false, fullscreen = false, mute = false;
    uint32_t audio_channels = 0;  // the device's own
    uint32_t ram_mb = host::kRetailRamMb;
    for (int k = 1; k < argc; ++k) {
        const std::string_view a = argv[k];
        if (a == "--ram-mb" && k + 1 < argc) {
            ram_mb = uint32_t(std::strtoul(argv[++k], nullptr, 10));
            if (ram_mb < host::kRetailRamMb || ram_mb > host::kMaxRamMb || ram_mb % 4) {
                std::fprintf(stderr, "qilin_run: --ram-mb takes a multiple of 4 from %u to %u\n", host::kRetailRamMb, host::kMaxRamMb);
                return 2;
            }
        } else if (a == "--game-dir" && k + 1 < argc) {
            game_dir = argv[++k];
        } else if (a == "--data-dir" && k + 1 < argc) {
            data_dir = argv[++k];
        } else if (a == "--dump-frames" && k + 1 < argc) {
            d3d.dump_dir = argv[++k];
        } else if (a == "--dump-audio" && k + 1 < argc) {
            dump_audio = argv[++k];
        } else if (a == "--frames" && k + 1 < argc) {
            d3d.max_frames = std::strtoull(argv[++k], nullptr, 10);
        } else if (a == "--headless") {
            headless = true;
        } else if (a == "--mute") {
            mute = true;
        } else if (a == "--audio-channels" && k + 1 < argc) {
            audio_channels = uint32_t(std::strtoul(argv[++k], nullptr, 10));
            if (audio_channels != 2 && audio_channels != 6) {
                std::fprintf(stderr, "qilin_run: --audio-channels takes 2 or 6\n");
                return 2;
            }
        } else if (a == "--fullscreen") {
            fullscreen = true;
        } else if (a == "--gpu" && k + 1 < argc) {
            d3d.gpu = int(std::strtol(argv[++k], nullptr, 10));
        } else if (!path) {
            path = argv[k];
        } else {
            path = nullptr;
            break;
        }
    }
    if (!path) {
        std::fprintf(stderr,
                     "usage: qilin_run [--ram-mb N] [--game-dir DIR] [--data-dir DIR] [--dump-frames DIR] [--dump-audio FILE.wav] [--frames N] [--gpu N] [--headless] [--fullscreen] [--mute] [--audio-channels 2|6] <default.xbe>\n"
                     "  --ram-mb N       guest RAM in MB (%u by default, as the retail Xbox)\n"
                     "  --game-dir DIR   the extracted disc (default: default.xbe's directory)\n"
                     "  --data-dir DIR   the hard disk: saves and caches (default: %s)\n"
                     "  --dump-frames DIR  write every frame shown there as a PNG\n"
                     "  --dump-audio FILE.wav  write everything the mixer plays there\n"
                     "  --frames N       stop after N frames\n"
                     "  --gpu N          the Vulkan device to use, by index (default: the best one)\n"
                     "  --headless       no window (frames are still drawn, for --dump-frames)\n"
                     "  --fullscreen     start fullscreen (F11 or Alt+Enter switches)\n"
                     "  --mute           no sound output (--headless implies it; --dump-audio still writes)\n"
                     "  --audio-channels 2 or 6 (5.1); by default 5.1 where the sound device has six or more speakers\n",
                     host::kRetailRamMb, qilin::utf8_path(default_data_dir()).c_str());
        return 2;
    }
    host::set_memory_map(host::MemoryMap{ram_mb << 20});
    const host::MemoryMap& mm = host::memory_map();
    auto loaded = Xbe::load(path);
    if (!loaded) {
        std::fprintf(stderr, "qilin_run: %s\n", loaded.error().c_str());
        return 2;
    }
    const Xbe& xbe = *loaded;
    if (xbe.image_end() > mm.stack_bottom()) {
        std::fprintf(stderr, "qilin_run: the image ends at %08X, inside the stack\n", xbe.image_end());
        return 2;
    }

    // Guest RAM, with the image at its base address.
    // Mapped at 0 and at the kernel's two windows onto it; pages the guest
    // never touches cost nothing. The mapping lasts until the process exits.
    std::string ram_error;
    uint8_t* const ram_base = host::map_guest_ram(mm.ram_size, &ram_error);
    if (!ram_base) {
        std::fprintf(stderr, "qilin_run: can't set up %u MB of guest RAM: %s\n", ram_mb, ram_error.c_str());
        return 2;
    }
    struct {
        uint8_t* p;
        uint8_t* get() const { return p; }
    } ram{ram_base};
    std::memcpy(ram.get() + xbe.base, xbe.image().data(), xbe.image().size());
    rt::Context c{};
    c.mem = ram.get();
    c.mem_size = rt::kVirtualLimit;  // RAM, then the title's virtual memory
    const rt::Mem m{c.mem, c.mem_size};
    // Sections not marked preload stay unloaded until XeLoadSection, as the
    // kernel's loader leaves them: keep their file data and clear their memory.
    {
        std::vector<host::DemandSection> demand;
        const uint32_t headers = xbe.u32(xbe.base + 0x120);
        for (size_t i = 0; i < xbe.sections.size(); ++i) {
            const XbeSection& s = xbe.sections[i];
            host::DemandSection d{headers + 56 * uint32_t(i), s.va, s.vsize, {}};
            if (s.preload()) {
                // Loaded at boot: the kernel's loader counts it loaded once, and its first and last pages shared.
                host::preload_section(c, d.header);
            } else {
                d.bytes.assign(ram.get() + s.va, ram.get() + s.va + s.rsize);
                std::memset(ram.get() + s.va, 0, s.vsize);
            }
            demand.push_back(std::move(d));
        }
        host::set_demand_sections(std::move(demand));
    }

    // Kernel imports: a function's thunk slot keeps 0x80000000 | ordinal, which
    // rt::call_unlifted recognises; a variable's slot gets the address of a
    // zeroed 256-byte object in the kernel data area.
    uint32_t data_slot = mm.kernel_data();
    for (size_t i = 0; i < xbe.kernel_imports.size(); ++i) {
        const uint32_t ordinal = xbe.kernel_imports[i];
        const host::KernelExport* e = host::kernel_export(ordinal);
        const uint32_t slot = xbe.kthunk + 4 * uint32_t(i);
        if (e && e->data) {
            rt::st32(m, slot, data_slot, 0);
            host::init_kernel_data(c, ordinal, data_slot);
            data_slot += 256;
        } else {
            rt::st32(m, slot, host::kernel_addr(ordinal), 0);
        }
    }

    // Code regions, for naming what the game calls.
    std::vector<host::Region> regions;
    for (const auto& r : kTextRegions) regions.push_back({r.lo, r.hi, r.name});
    for (const auto& s : xbe.sections)
        if (s.code && s.name != ".text") regions.push_back({s.va, s.va + s.vsize, s.name});
    host::set_regions(std::move(regions));

    // RAM between the image and the main stack, for contiguous memory.
    host::vm().init(c.mem, (xbe.image_end() + 0xFFFFu) & ~0xFFFFu, mm.stack_bottom() & ~0xFFFFu);
    // And the title's virtual memory, above it.
    host::virtual_memory().init(c.mem, mm.ram_size, rt::kVirtualLimit, true);
    host::register_xapi();
    host::register_game_fixes();
    // The settings (ghoulies.ini in the data folder): written with the defaults the first time.
    const std::filesystem::path data_folder = data_dir ? qilin::native_path(data_dir) : default_data_dir();
    static platform::Settings settings = platform::load_settings(data_folder);
    fullscreen = fullscreen || settings.fullscreen;
    if (!audio_channels) audio_channels = settings.channels;
    host::set_console_language(settings.language);
    // The picture's shape (auto: the main screen's; headless, 16:9), which
    // the game reads as it starts: the window opens at it too.
    const float aspect = host::game_aspect_ratio(platform::aspect_ratio(settings, headless ? 0.0f : platform::Window::main_screen_aspect()));
    host::set_game_aspect(m, aspect, false);
    d3d.settings = &settings;
    d3d.settings_folder = qilin::utf8_path(data_folder);
    // The window: where there's no display, the game still runs and draws.
    std::unique_ptr<platform::Window> window;
    if (!headless) {
        std::string why;
        window = platform::Window::create("Grabbed by the Ghoulies", uint32_t(std::lround(480.0f * aspect)), 480, fullscreen, &why);
        if (!window) std::printf("qilin_run: no window (%s); running without one\n", why.c_str());
    }
    // Controllers, read through the window's events; without a window, none.
    std::unique_ptr<platform::Input> input;
    if (window) {
        input = std::make_unique<platform::Input>();
        input->start();
        input->set_keys(settings.keys);
        input->set_rumble(settings.rumble);
    }
    d3d.window = window.get();
    d3d.input = input.get();
    host::register_xinput(input.get());
    // Outside the partitions, so the game's disk never sees it.
    d3d.pipeline_cache = qilin::utf8_path((data_dir ? qilin::native_path(data_dir) : default_data_dir()) / "vulkan_pipelines.cache");
    host::register_d3d(d3d);
    // Sound: the mixer's output to the default playback device, unless asked not to.
    std::unique_ptr<platform::AudioOut> sound;
    if (!headless && !mute) {
        sound = std::make_unique<platform::AudioOut>();
        sound->start(audio::kOutputRate, audio_channels, settings.sound_device); // keep retrying during an outage
    }
    host::set_audio_surround(sound && sound->channels() == 6);
    host::register_dsound(dump_audio, sound.get());
    struct RuntimeLifetime {
        ~RuntimeLifetime() { host::shutdown_dsound(); host::shutdown_d3d(); }
    } runtime_lifetime; // before output, controllers and window, including normal main() return
    host::register_kernel();
    const std::filesystem::path disc = game_dir ? qilin::native_path(game_dir) : qilin::native_path(path).parent_path();
    std::filesystem::path data = data_dir ? qilin::native_path(data_dir) : default_data_dir();
    // A test run (selftest.txt, which the Direct3D side plays) gets a hard
    // disk of its own, empty each time: the player's saves aren't touched,
    // and the run starts the same way every time.
    if (std::error_code ec; std::filesystem::exists(data / "selftest.txt", ec)) {
        data /= "selftest-disk";
        std::filesystem::remove_all(data, ec);
        std::filesystem::create_directories(data, ec);
    }
    host::register_kernel_io({disc.empty() ? std::filesystem::path(".") : disc, data, !headless});
    std::printf("qilin_run: disc %s, hard disk %s\n", qilin::utf8_path(disc).c_str(), qilin::utf8_path(data).c_str());

    // The main thread: its information block (fs:[0] = end of the exception
    // chain), the C runtime's default x87 control word, and main's arguments.
    // The kernel puts the thread's TLS block at the top of its stack, below
    // StackBase, and starts the stack under it.
    const uint32_t tls = tls_size(m);
    host::init_main_thread(c, mm.kpcr(), mm.kthread(), mm.stack_top(), host::MemoryMap::kStackSize, tls);
    // Before init_main, which copies these registers as the starting state of
    // every thread created later and of DPCs: the kernel starts each thread
    // with this control word (all exceptions masked, 53-bit precision).
    c.fpcw = 0x027F;
    host::sched::init_main(c, mm.kthread(), host::call_guest);  // the scheduler, with this as the main thread
    if (tls) init_tls(c, mm.stack_top() - tls);
    c.esp = (mm.stack_top() - tls) & ~15u;
    for (int k = 0; k < 3; ++k) push(c, 0);  // main(argc, argv, envp)
    push(c, host::kReturnSentinel);
    host::set_current(&c);

    std::printf("qilin_run: %u MB of guest RAM, %zu replacements registered; starting XAPI and the C runtime\n", ram_mb, host::hle_count());
    crt_startup(c);
    std::printf("qilin_run: calling main at %08X\n", kGameMain);
    rt::call(c, kGameMain);
    if (c.ret_to != host::kReturnSentinel) rt::trap(kGameMain, "main returned somewhere else");
    std::printf("qilin_run: main returned %u\n", c.eax);
    return 0;
}
