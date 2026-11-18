// Native replacements for the XAPI functions Ghoulies calls, by their address
// in this XBE. Each one documents what it replaces; the XAPI originals were
// identified from their disassembly.
#include <chrono>

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

// BOOL QueryPerformanceFrequency(LARGE_INTEGER*) at 0x14C2AD: 733,333,333.
void query_performance_frequency(rt::Context& c) {
    const rt::Mem m{c.mem, c.mem_size};
    const uint32_t out = arg(c, 0);
    rt::st32(m, out, uint32_t(kTscHz), 0);
    rt::st32(m, out + 4, 0, 0);
    ret(c, 1, 4);
}

// void GlobalMemoryStatus(MEMORYSTATUS*) at 0x14AD17: XAPI fills it from
// MmQueryStatistics. Physical memory is the 64 MB of RAM and what the guest
// heap has left; the virtual figures are XAPI's fixed 0x7FFE0000.
void global_memory_status(rt::Context& c) {
    const rt::Mem m{c.mem, c.mem_size};
    const uint32_t ms = arg(c, 0);
    const uint32_t fields[8] = {0x20, 0, kRamSize, heap().available(), 0, 0, 0x7FFE0000u, 0x7FFE0000u - (heap().total() - heap().available())};
    for (uint32_t k = 0; k < 8; ++k) rt::st32(m, ms + 4 * k, fields[k], 0);
    ret(c, 0, 4);
}

// DWORD XGetLaunchInfo(DWORD* type, LAUNCH_DATA* data) at 0x14AE89: XAPI
// copies the kernel's LaunchDataPage if there is one. The runtime always
// cold-boots the game, so there's none: ERROR_NOT_FOUND.
void x_get_launch_info(rt::Context& c) { ret(c, 0x490, 8); }

// DWORD XGetLanguage(void) at 0x14FA3B: XAPI reads the console's language
// setting (ExQueryNonVolatileSetting 7) and falls back to English, or
// Japanese on a Japanese console. The runtime has no console settings yet:
// English (1).
void x_get_language(rt::Context& c) { ret(c, 1, 0); }

}  // namespace

void register_xapi() {
    register_hle(0x14C29C, "QueryPerformanceCounter", query_performance_counter);
    register_hle(0x14C2AD, "QueryPerformanceFrequency", query_performance_frequency);
    register_hle(0x14AD17, "GlobalMemoryStatus", global_memory_status);
    register_hle(0x14AE89, "XGetLaunchInfo", x_get_launch_info);
    register_hle(0x14FA3B, "XGetLanguage", x_get_language);
}

}  // namespace qilin::host
