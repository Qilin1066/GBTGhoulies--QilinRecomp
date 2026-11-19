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
#include <initializer_list>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "guest.hpp"

namespace qilin::platform {
class Window;
class Input;
class AudioOut;
}

namespace qilin::host {

// ---- Guest memory: RAM flat from address 0. 64 MB is the retail Xbox's;
// qilin_run --ram-mb raises it (the game reads the size from
// GlobalMemoryStatus and works with more). The top 128 KB holds the kernel's
// data-export objects and the main thread's KPCR and KTHREAD; the main
// stack sits below that, and the rest above the image is the guest's to
// allocate.
inline constexpr uint32_t kRetailRamMb = 64;
inline constexpr uint32_t kMaxRamMb = 1024;  // guest pointers stay below 0x80000000, where the kernel's addresses start
struct MemoryMap {
    uint32_t ram_size;
    uint32_t kernel_data() const { return ram_size - 0x10000; }  // objects behind the kernel's data exports, 256 bytes each
    uint32_t kpcr() const { return ram_size - 0x20000; }          // the main thread's processor control region (fs)
    uint32_t kthread() const { return kpcr() + 0x1000; }
    uint32_t stack_top() const { return kpcr(); }                // the main thread's stack grows down from here
    static constexpr uint32_t kStackSize = 1u << 20;
    uint32_t stack_bottom() const { return stack_top() - kStackSize; }
};
const MemoryMap& memory_map();
void set_memory_map(MemoryMap m);
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
const KernelExport* kernel_export(std::string_view name);
inline constexpr uint32_t kernel_addr(uint32_t ordinal) { return 0x80000000u | ordinal; }

// ---- Replacements.
using HleFn = void (*)(rt::Context&);
void register_hle(uint32_t guest_addr, const char* name, HleFn fn);
size_t hle_count();

// stdcall: argument i (from 0) is at [esp + 4 + 4i]; returning pops the
// return address into ret_to and the callee's arguments, as `ret n` does.
uint32_t arg(const rt::Context& c, int i);
// Calls a lifted guest function (cdecl) from the runtime, e.g. a callback the
// game registered; esp is restored afterwards.
void call_guest(rt::Context& c, uint32_t fn, std::initializer_list<uint32_t> args);
void ret(rt::Context& c, uint32_t eax, uint32_t arg_bytes);

// ---- Guest virtual memory (the kernel's Nt*VirtualMemory and Mm*
// contiguous allocations) over the RAM between the end of the image and the
// main stack. As on the Xbox: 4 KB pages, reservations on 64 KB boundaries,
// pages committed inside a reservation, committed pages zeroed. The address
// space is flat (guest address = RAM offset), so a reservation is backed by
// RAM from the start; "committed" is bookkeeping, which the statistics and
// NtQueryVirtualMemory report.
class VirtualMemory {
public:
    static constexpr uint32_t kPage = 0x1000, kGranularity = 0x10000;
    static constexpr uint32_t kMemCommit = 0x1000, kMemReserve = 0x2000, kMemFree = 0x10000, kMemPrivate = 0x20000;
    struct Info {
        uint32_t base, allocation_base, allocation_protect, size, state, protect, type;
    };
    // lo, hi: 64 KB aligned. host_backed: the range is the guest's virtual
    // memory, whose host pages are made accessible on commit and given
    // back on decommit and release (commit_guest_pages, free_guest_pages).
    void init(uint8_t* ram, uint32_t lo, uint32_t hi, bool host_backed = false);
    // Reserves `size` bytes (rounded up to pages) at `base` (rounded down to
    // 64 KB), or anywhere free if base is 0, lowest first or highest first.
    // Returns the base, or 0 if that range isn't free.
    // lowest/highest bound the whole reservation (contiguous allocations' limits).
    uint32_t reserve(uint32_t base, uint32_t size, uint32_t protect, bool top_down, uint32_t align = kGranularity,
                     uint32_t lowest = 0, uint32_t highest = 0xFFFFFFFFu);
    // Commits pages of one reservation, zeroing those not already committed.
    bool commit(uint32_t base, uint32_t size, uint32_t protect);
    bool decommit(uint32_t base, uint32_t size);
    bool release(uint32_t base);                         // a whole reservation, by its base
    std::optional<Info> query(uint32_t addr) const;      // nullopt outside the managed range
    uint32_t region_size(uint32_t base) const;           // a reservation's size, 0 if base isn't one
    uint32_t lo() const { return lo_; }
    uint32_t hi() const { return hi_; }
    uint64_t committed_bytes() const;
    uint64_t uncommitted_bytes(uint32_t base, uint32_t size) const;  // in [base, base + size), of one reservation
    uint64_t reserved_bytes() const;

private:
    struct Region {
        uint32_t size, protect;
        std::vector<uint32_t> page_protect;  // 0: reserved only
    };
    uint8_t* ram_ = nullptr;
    uint32_t lo_ = 0, hi_ = 0;
    bool host_backed_ = false;
    std::map<uint32_t, Region> regions_;
    const std::pair<const uint32_t, Region>* find(uint32_t addr) const;
};
// RAM: what the image, the main stack and the kernel leave, for the
// title's contiguous memory (reservations here are physical pages).
VirtualMemory& vm();
// The title's virtual memory, above RAM (NtAllocateVirtualMemory): its
// address space costs nothing, its committed pages come out of RAM.
VirtualMemory& virtual_memory();
// Whether `bytes` more of RAM can be used: the RAM range's size, less its
// reservations and the virtual memory's committed pages.
bool physical_room(uint64_t bytes);
// Says on the console that a guest allocation found no room (what, its size).
void out_of_memory(const char* what, uint32_t bytes);

// The replacements for Ghoulies' XAPI, registered by address (xapi.cpp).
void register_xapi();
// Direct3D (d3d.cpp), over the Vulkan renderer.
struct D3dConfig {
    std::string dump_dir;     // write each presented frame there as frame_NNNNN.png (--dump-frames)
    uint64_t max_frames = 0;  // stop after this many frames, 0 for never (--frames)
    int gpu = -1;             // Vulkan device index, -1 for the best one (--gpu)
    platform::Window* window = nullptr;  // where frames are shown; none with --headless
    platform::Input* input = nullptr;    // controllers, whose events the window's pump passes on
};
void register_d3d(const D3dConfig& config);
const D3dConfig& d3d_config();

// DirectSound (dsound.cpp), over the software mixer; dump_wav non-empty
// writes everything mixed there (--dump-audio).
void register_dsound(const std::string& dump_wav, platform::AudioOut* out);
// Brings the mix up to now (DirectSound calls do this too); Swap calls it, so
// sound keeps playing while the game doesn't reach DirectSound.
void pump_audio(rt::Context& c);
// The host's housekeeping while the game waits without swapping (its busy
// loops read QueryPerformanceCounter), at most every 16 ms: what each
// library added (D3D the window's events, DirectSound the mix).
void idle(rt::Context& c);
void add_idle_hook(void (*hook)(rt::Context&));

// XInput (xinput.cpp): the controller ports, over `input` (null: nothing plugged in).
void register_xinput(platform::Input* input);
// The kernel replacements, registered at kernel_addr(ordinal) (kernel.cpp).
void register_kernel();
void register_kernel_crypto();  // XcSHA*, XcHMAC (kernel_crypto.cpp); register_kernel calls it
// The I/O manager (kernel_io.cpp): \\Device\\CdRom0 is `disc` (read-only), the
// hard disk's partitions are data/partition1..6.
struct IoConfig {
    std::filesystem::path disc, data;
};
void register_kernel_io(const IoConfig& cfg);
// Fills in the object behind a kernel data export (XeImageFileName,
// HalDiskCachePartitionCount …) at `addr`, which its thunk slot points to.
void init_kernel_data(rt::Context& c, uint32_t ordinal, uint32_t addr);
// The XBE's sections, for XeLoadSection and XeUnloadSection, which take the
// section header's guest address. Those not preloaded ($$XTIMAGE and the
// like) keep their file data in `bytes` and are read in on the first load.
struct DemandSection {
    uint32_t header, va, vsize;
    std::vector<uint8_t> bytes;  // the section's file data
};
void set_demand_sections(std::vector<DemandSection> sections);
// Counts a preloaded section as loaded, as the kernel's loader does at boot.
void preload_section(rt::Context& c, uint32_t header);

// Contiguous memory (MmAllocateContiguousMemory and the runtime's own guest
// objects): committed, top-down, below kPhysicalLimit; 0 if there's no room.
inline constexpr uint32_t kPhysicalLimit = 0x10000000u;
uint32_t contiguous(uint32_t bytes, uint32_t lowest, uint32_t highest, uint32_t align, uint32_t protect);

// ---- Threads. fs points at the KPCR, whose PrcbData.CurrentThread (+0x28)
// is the running thread's KTHREAD (kernel.cpp).
inline constexpr uint32_t kKThreadSize = 0x200;
// tls_size: the thread's TLS block, at the top of its stack (KTHREAD.TlsData).
void init_main_thread(rt::Context& c, uint32_t kpcr, uint32_t kthread, uint32_t stack_top, uint32_t stack_size,
                      uint32_t tls_size);
uint32_t current_thread(const rt::Context& c);

// ---- Guest RAM (guest_ram.cpp): ram_size bytes, seen at 0 and again at
// rt::kRamCachedAlias and rt::kRamWriteCombinedAlias, in a 4 GB host
// reservation whose base is returned (the guest's address 0). Pages start
// zero and cost nothing until touched. Null with the reason in *error.
uint8_t* map_guest_ram(uint32_t ram_size, std::string* error);
// The host pages under the guest's virtual memory [at, at + size): made
// accessible, or given back (only those the range covers whole).
void commit_guest_pages(uint32_t at, uint32_t size);
void free_guest_pages(uint32_t at, uint32_t size);

// ---- Device registers lifted code reads or writes (rt::mmio_read32 and
// rt::mmio_write32). An unregistered register stops the runtime, named.
using MmioRead = uint32_t (*)();
void register_mmio_read(uint32_t addr, const char* name, MmioRead fn);

// ---- XDK library symbols: the functions and variables of the Microsoft
// libraries linked into the game (D3D8, DSOUND, XACT …), by address.
struct XdkSymbol {
    uint32_t address;
    const char* library;
    const char* name;
};
const XdkSymbol* xdk_symbol(uint32_t guest_addr);
const XdkSymbol* xdk_symbol(std::string_view name);  // the first symbol of that name

// ---- Reports.
struct Region {
    uint32_t lo, hi;
    std::string name;
};
void set_regions(std::vector<Region> regions);  // the loader's map of code regions, for describe()
std::string describe(uint32_t guest_addr);      // "kernel NtCreateFile (ordinal 190)", "D3D8 D3DDevice_Clear (00176970)", "XAPI code at ..."
void set_current(rt::Context* c);               // the context trap() reports registers from

}  // namespace qilin::host
