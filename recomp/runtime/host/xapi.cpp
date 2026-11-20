// Native replacements for the few XAPI functions that can't be lifted, by
// their address in this XBE (the rest of XAPI is lifted; qilin lift's
// kNative list says why each of these isn't). Each documents what it
// replaces; the XAPI originals were identified from their disassembly.
#include <chrono>
#include <cstdio>
#include <string>

#include "host.hpp"

namespace qilin::host {
namespace {

constexpr uint64_t kTscHz = 733333333;  // the Xbox's Pentium III clock, what XAPI's counter counts

}  // namespace

// BOOL QueryPerformanceCounter(LARGE_INTEGER*) at 0x14C29C: XAPI reads rdtsc.
// Host time, scaled to the Xbox's 733 MHz.
uint64_t tsc_now() {
    static const auto start = std::chrono::steady_clock::now();
    const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    return ns / 1000000000u * kTscHz + ns % 1000000000u * kTscHz / 1000000000u;
}

namespace {

void query_performance_counter(rt::Context& c) {
    idle(c);
    const uint64_t ticks = tsc_now();
    const rt::Mem m{c.mem, c.mem_size};
    const uint32_t out = arg(c, 0);
    rt::st32(m, out, uint32_t(ticks), 0);
    rt::st32(m, out + 4, uint32_t(ticks >> 32), 0);
    ret(c, 1, 4);
}

// VOID OutputDebugStringA(LPCSTR) at 0x14B924: XAPI hands the string to the
// kernel debugger (int 0x2D, BREAKPOINT_PRINT). The runtime prints it.
void output_debug_string_a(rt::Context& c) {
    const rt::Mem m{c.mem, c.mem_size};
    std::string text;
    for (uint32_t a = arg(c, 0); a && text.size() < 4096; ++a) {
        const char ch = char(rt::ld8(m, a, 0));
        if (!ch) break;
        text += ch;
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    std::fprintf(stderr, "[guest] %s\n", text.c_str());
    ret(c, c.eax, 4);
}

// int Bink's MMX probe at 0x162840 (in Rare's code, cdecl, one flags
// argument): the original toggles EFLAGS.ID to see whether cpuid exists,
// then reads cpuid's MMX bit, and caches the answer at 0x336894 (negative
// until probed). The Xbox's Pentium III has MMX, so the answer is 1. Bink's
// MMX switch (0x54D9E0) is then on unless the flags turn it off (0x40000).
void bink_mmx_probe(rt::Context& c) {
    const rt::Mem m{c.mem, c.mem_size};
    uint32_t has = rt::ld32(m, 0x336894, 0);
    if (int32_t(has) < 0) {
        has = 1;
        rt::st32(m, 0x336894, has, 0);
    }
    rt::st32(m, 0x54D9E0, has && !(arg(c, 0) & 0x40000u) ? 1u : 0u, 0);
    ret(c, has, 0);
}

}  // namespace

void register_xapi() {
    register_hle(0x162840, "Bink's MMX probe", bink_mmx_probe);
    register_hle(0x14C29C, "QueryPerformanceCounter", query_performance_counter);
    register_hle(0x14B924, "OutputDebugStringA", output_debug_string_a);
}

}  // namespace qilin::host

namespace qilin::rt {
uint64_t rdtsc(uint32_t) { return host::tsc_now(); }
}  // namespace qilin::rt
