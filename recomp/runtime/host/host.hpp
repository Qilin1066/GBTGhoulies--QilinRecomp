// The runtime's host side: guest memory layout, the registry of native
// replacements for code that isn't lifted (kernel, XAPI, CRT, D3D and the
// other XDK libraries: HLE), and helpers for writing them.
//
// Lifted code reaches unlifted code through rt::call_unlifted: direct calls
// to a stub, indirect calls the function table doesn't know, and kernel
// imports, whose thunk slots hold 0x80000000 | ordinal. call_unlifted runs the
// registered replacement; there's no fallback, so anything not registered
// stops the game with a report naming it and its caller.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "guest.hpp"

namespace qilin::host {

// ---- Guest memory: the Xbox's 64 MB of RAM, flat from address 0.
inline constexpr uint32_t kRamSize = 64u << 20;
inline constexpr uint32_t kKernelData = 0x03FF0000;  // objects behind the kernel's data exports, 256 bytes each
inline constexpr uint32_t kTib = 0x03FE0000;         // the main thread's information block (fs)
inline constexpr uint32_t kStackTop = 0x03FE0000;    // the main thread's stack grows down from here
inline constexpr uint32_t kStackSize = 1u << 20;
inline constexpr uint32_t kReturnSentinel = 0xFFFFFFF0u;  // return address of the outermost call

// ---- Kernel exports (third_party/nxdk/xboxkrnl.exe.def).
struct KernelExport {
    uint16_t ordinal;
    const char* name;
    uint16_t arg_bytes;  // stdcall: what the callee pops
    bool data;           // a variable, not a function: its thunk slot gets the variable's address
    bool fastcall;
};
const KernelExport* kernel_export(uint32_t ordinal);  // nullptr if unknown
inline constexpr uint32_t kernel_addr(uint32_t ordinal) { return 0x80000000u | ordinal; }

// ---- Replacements.
using HleFn = void (*)(rt::Context&);
void register_hle(uint32_t guest_addr, const char* name, HleFn fn);
size_t hle_count();

// stdcall: argument i (from 0) is at [esp + 4 + 4i]; returning pops the
// return address into ret_to and the callee's arguments, as `ret n` does.
uint32_t arg(const rt::Context& c, int i);
void ret(rt::Context& c, uint32_t eax, uint32_t arg_bytes);

// ---- Guest RAM allocation: whole 4 KB pages between the end of the image
// and the bottom of the main stack, first fit. Allocations are zeroed.
class GuestHeap {
public:
    void init(uint8_t* ram, uint32_t lo, uint32_t hi);  // lo, hi: page-aligned guest addresses
    uint32_t alloc(uint32_t bytes);                    // 0 when out of memory
    bool free(uint32_t addr);                          // false if addr isn't an allocation
    uint32_t size_of(uint32_t addr) const;             // 0 if addr isn't an allocation
    uint32_t total() const { return hi_ - lo_; }
    uint32_t available() const;

private:
    uint8_t* ram_ = nullptr;
    uint32_t lo_ = 0, hi_ = 0;
    std::vector<std::pair<uint32_t, uint32_t>> used_;  // (address, bytes), sorted
};
GuestHeap& heap();

// The replacements for Ghoulies' XAPI, registered by address (xapi.cpp).
void register_xapi();

// ---- Reports.
struct Region {
    uint32_t lo, hi;
    std::string name;
};
void set_regions(std::vector<Region> regions);  // the loader's map of code regions, for describe()
std::string describe(uint32_t guest_addr);      // "kernel NtCreateFile (ordinal 190)", "XAPI code at ..."
void set_current(rt::Context* c);               // the context trap() reports registers from

}  // namespace qilin::host
