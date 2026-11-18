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

// BOOL QueryPerformanceCounter(LARGE_INTEGER*) at 0x14C29C: XAPI reads rdtsc.
// Host time, scaled to the Xbox's 733 MHz.
void query_performance_counter(rt::Context& c) {
    static const auto start = std::chrono::steady_clock::now();
    const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    const uint64_t ticks = ns / 1000000000u * kTscHz + ns % 1000000000u * kTscHz / 1000000000u;
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

}  // namespace

void register_xapi() {
    register_hle(0x14C29C, "QueryPerformanceCounter", query_performance_counter);
    register_hle(0x14B924, "OutputDebugStringA", output_debug_string_a);
}

}  // namespace qilin::host
