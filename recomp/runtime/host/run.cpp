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
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <cstdlib>
#include <memory>
#include <string_view>

#include "regions.hpp"
#include "host.hpp"
#include "xbe.hpp"

using namespace qilin;

namespace {

constexpr uint32_t kGameMain = 0x32390;

void push(rt::Context& c, uint32_t v) {
    c.esp -= 4;
    rt::st32(rt::Mem{c.mem, c.mem_size}, c.esp, v, 0);
}

// Calls a guest function (cdecl) with the given arguments.
void call_guest(rt::Context& c, uint32_t fn, std::initializer_list<uint32_t> args) {
    const uint32_t esp = c.esp;
    for (auto it = std::rbegin(args); it != std::rend(args); ++it) push(c, *it);
    push(c, host::kReturnSentinel);
    rt::call(c, fn);
    if (c.ret_to != host::kReturnSentinel) rt::trap(fn, "an initializer returned somewhere else");
    c.esp = esp;
}

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

// What the XBE entry point (0x14FAE9) does before it starts the main thread:
// fix up the certificate's size field, then size the static TLS block from the
// TLS directory at 0x203468 and store its size (0x549528) and the TLS index.
void entry_prologue(rt::Context& c) {
    const rt::Mem m{c.mem, c.mem_size};
    auto ld = [&](uint32_t a) { return rt::ld32(m, a, 0); };
    const uint32_t cert = ld(0x10118);
    const uint32_t need = ld(0x10108) - cert + 0x10000;
    if (need < ld(cert)) rt::st32(m, cert, need, 0);
    const uint32_t tls = ((ld(0x203478) - ld(0x203468) + ld(0x20346C) + 15) & ~15u) + 4;
    rt::st32(m, 0x549528, tls, 0);
    rt::st32(m, ld(0x203470), uint32_t(int32_t(tls) / -4), 0);
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
    const char* path = nullptr;
    uint32_t ram_mb = host::kRetailRamMb;
    for (int k = 1; k < argc; ++k) {
        const std::string_view a = argv[k];
        if (a == "--ram-mb" && k + 1 < argc) {
            ram_mb = uint32_t(std::strtoul(argv[++k], nullptr, 10));
            if (ram_mb < host::kRetailRamMb || ram_mb > host::kMaxRamMb || ram_mb % 4) {
                std::fprintf(stderr, "qilin_run: --ram-mb takes a multiple of 4 from %u to %u\n", host::kRetailRamMb, host::kMaxRamMb);
                return 2;
            }
        } else if (!path) {
            path = argv[k];
        } else {
            path = nullptr;
            break;
        }
    }
    if (!path) {
        std::fprintf(stderr, "usage: qilin_run [--ram-mb N] <default.xbe>   (N: guest RAM in MB, %u by default, as the retail Xbox)\n",
                     host::kRetailRamMb);
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
    // calloc: the host maps zeroed pages lazily, so RAM the guest never touches costs nothing.
    const std::unique_ptr<uint8_t, decltype(&std::free)> ram(static_cast<uint8_t*>(std::calloc(mm.ram_size, 1)), &std::free);
    if (!ram) {
        std::fprintf(stderr, "qilin_run: can't allocate %u MB of guest RAM\n", ram_mb);
        return 2;
    }
    std::memcpy(ram.get() + xbe.base, xbe.image().data(), xbe.image().size());
    rt::Context c{};
    c.mem = ram.get();
    c.mem_size = mm.ram_size;
    const rt::Mem m{c.mem, c.mem_size};

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

    // Guest virtual memory: the RAM between the image and the main stack.
    host::vm().init(c.mem, (xbe.image_end() + 0xFFFFu) & ~0xFFFFu, mm.stack_bottom() & ~0xFFFFu);
    host::register_xapi();
    host::register_kernel();

    // The main thread: its information block (fs:[0] = end of the exception
    // chain), the C runtime's default x87 control word, and main's arguments.
    host::init_main_thread(c, mm.kpcr(), mm.kthread(), mm.stack_top(), host::MemoryMap::kStackSize);
    c.fpcw = 0x027F;
    c.esp = mm.stack_top();
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
