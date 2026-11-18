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
#include <memory>
#include <print>

#include "cfg.hpp"
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
            std::println("qilin_run: initializer {:08X} ({}) skipped: its library is replaced natively", fn, host::describe(fn));
            continue;
        }
        call_guest(c, fn, {});
    }
}

// The C runtime's start-up, as XAPI's thread procedure does it (0x14FAC1).
void crt_startup(rt::Context& c) {
    const rt::Mem m{c.mem, c.mem_size};
    run_table(c, 0x32C800, 0x32C80C);                            // _mtinit, _ioinit
    if (const uint32_t fpinit = rt::ld32(m, 0x337A88, 0))        // _FPinit: float formatting, x87 set-up
        call_guest(c, fpinit, {1});                                // XAPI's `call eax` leaves a non-zero word there
    run_table(c, 0x32C838, 0x32C84C);
    run_table(c, 0x32C810, 0x32C834);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::println(stderr, "usage: qilin_run <default.xbe>");
        return 2;
    }
    auto loaded = Xbe::load(argv[1]);
    if (!loaded) {
        std::println(stderr, "qilin_run: {}", loaded.error());
        return 2;
    }
    const Xbe& xbe = *loaded;
    if (xbe.image_end() > host::kStackTop - host::kStackSize) {
        std::println(stderr, "qilin_run: the image ends at {:08X}, inside the stack", xbe.image_end());
        return 2;
    }

    // Guest RAM, with the image at its base address.
    auto ram = std::make_unique<uint8_t[]>(host::kRamSize);  // zeroed
    std::memcpy(ram.get() + xbe.base, xbe.image().data(), xbe.image().size());
    rt::Context c{};
    c.mem = ram.get();
    c.mem_size = host::kRamSize;
    const rt::Mem m{c.mem, c.mem_size};

    // Kernel imports: a function's thunk slot keeps 0x80000000 | ordinal, which
    // rt::call_unlifted recognises; a variable's slot gets the address of a
    // zeroed 256-byte object in the kernel data area.
    uint32_t data_slot = host::kKernelData;
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
    for (const auto& r : CfgConfig{}.text_regions) regions.push_back({r.lo, r.hi, r.name});
    for (const auto& s : xbe.sections)
        if (s.code && s.name != ".text") regions.push_back({s.va, s.va + s.vsize, s.name});
    host::set_regions(std::move(regions));

    // Guest heap: the pages between the image and the main stack.
    host::heap().init(c.mem, (xbe.image_end() + 0xFFF) & ~0xFFFu, host::kStackTop - host::kStackSize);
    host::register_xapi();

    // The main thread: its information block (fs:[0] = end of the exception
    // chain), the C runtime's default x87 control word, and main's arguments.
    c.fs_base = host::kTib;
    rt::st32(m, host::kTib, 0xFFFFFFFFu, 0);
    rt::st32(m, host::kTib + 0x18, host::kTib, 0);  // NT_TIB.Self
    c.fpcw = 0x027F;
    c.esp = host::kStackTop;
    for (int k = 0; k < 3; ++k) push(c, 0);  // main(argc, argv, envp)
    push(c, host::kReturnSentinel);
    host::set_current(&c);

    std::println("qilin_run: {} replacements registered; starting the C runtime", host::hle_count());
    crt_startup(c);
    std::println("qilin_run: calling main at {:08X}", kGameMain);
    rt::call(c, kGameMain);
    if (c.ret_to != host::kReturnSentinel) rt::trap(kGameMain, "main returned somewhere else");
    std::println("qilin_run: main returned {}", c.eax);
    return 0;
}
