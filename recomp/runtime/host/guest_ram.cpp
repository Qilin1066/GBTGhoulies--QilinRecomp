// Guest RAM: one block of memory seen at three guest addresses, as on the
// Xbox. Physical memory is at 0 (the flat address space titles use), and
// the kernel maps it again at 0x80000000 (contiguous memory, which D3D hands
// out as `Data | 0x80000000`) and at 0xF0000000 (write-combined, which D3D's
// surface locks return). The game writes pixels and vertices through both,
// so they are real views of the same pages, inside one 4 GB host
// reservation: lifted code addresses guest memory as base + address with
// nothing extra per access. Above RAM, up to rt::kVirtualLimit, is the
// title's virtual memory: host pages there are inaccessible until the
// title commits them (commit_guest_pages), and RAM is charged for what's
// committed, not for the address space (host.cpp's physical budget).
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "host.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace qilin::host {
namespace {

constexpr uint64_t kReservation = (uint64_t(1) << 32) + 0x10000;  // the guest's 4 GB, and a guard for accesses that run past its end

struct View {
    uint32_t at;
    uint64_t size;
};

// The views of `ram` bytes: physical memory, then the two kernel windows,
// each as much of RAM as fits below the device registers (0xFD000000).
std::vector<View> views(uint32_t ram) {
    return {{0, ram},
            {rt::kRamCachedAlias, std::min<uint64_t>(ram, rt::kMmioLo - rt::kRamCachedAlias)},
            {rt::kRamWriteCombinedAlias, std::min<uint64_t>(ram, rt::kMmioLo - rt::kRamWriteCombinedAlias)}};
}

#if defined(_WIN32)
using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);

uint8_t* map(uint32_t ram, std::string* error) {
    // Placeholder reservations (Windows 10 1803 and later): reserve the
    // range, split a placeholder out for each view, and map the section into it.
    const HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    const auto alloc2 = kb ? reinterpret_cast<VirtualAlloc2Fn>(reinterpret_cast<void*>(GetProcAddress(kb, "VirtualAlloc2"))) : nullptr;
    const auto map3 = kb ? reinterpret_cast<MapViewOfFile3Fn>(reinterpret_cast<void*>(GetProcAddress(kb, "MapViewOfFile3"))) : nullptr;
    if (!alloc2 || !map3) {
        *error = "this Windows has no VirtualAlloc2/MapViewOfFile3 (Windows 10 version 1803 or later is needed)";
        return nullptr;
    }
    const HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, ram, nullptr);
    if (!section) {
        *error = "CreateFileMapping failed (" + std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    auto* base = static_cast<uint8_t*>(alloc2(nullptr, nullptr, kReservation, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
    if (!base) {
        *error = "can't reserve 4 GB of address space (" + std::to_string(GetLastError()) + ")";
        CloseHandle(section);
        return nullptr;
    }
    for (const View& v : views(ram)) {
        if (!VirtualFree(base + v.at, v.size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) ||
            !map3(section, GetCurrentProcess(), base + v.at, 0, v.size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0)) {
            *error = "can't map guest RAM at " + std::to_string(v.at) + " (" + std::to_string(GetLastError()) + ")";
            return nullptr;
        }
    }
    CloseHandle(section);  // the views keep it
    // The virtual memory: an ordinary reservation, committed page by page.
    const SIZE_T vsize = SIZE_T(rt::kVirtualLimit) - ram;
    if (!VirtualFree(base + ram, vsize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) ||
        !alloc2(GetCurrentProcess(), base + ram, vsize, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0)) {
        *error = "can't reserve the guest's virtual memory (" + std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    return base;
}

void host_commit(uint8_t* p, size_t n) {
    if (!VirtualAlloc(p, n, MEM_COMMIT, PAGE_READWRITE)) {
        std::fprintf(stderr, "qilin_run: can't commit %zu bytes of guest virtual memory (%lu)\n", n, GetLastError());
        std::abort();
    }
}
void host_free(uint8_t* p, size_t n) { VirtualFree(p, n, MEM_DECOMMIT); }
size_t host_page() { return 0x1000; }
#else
uint8_t* map(uint32_t ram, std::string* error) {
    int fd = -1;
#if defined(__linux__)
    fd = memfd_create("qilin-guest-ram", MFD_CLOEXEC);
#else
    // A named object only for as long as it takes to open and unlink it.
    const std::string name = "/qilin-ram-" + std::to_string(getpid());
    fd = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) shm_unlink(name.c_str());
#endif
    if (fd < 0 || ftruncate(fd, off_t(ram)) != 0) {
        *error = std::string("can't create guest RAM: ") + std::strerror(errno);
        if (fd >= 0) close(fd);
        return nullptr;
    }
    void* r = mmap(nullptr, kReservation, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (r == MAP_FAILED) {
        *error = std::string("can't reserve 4 GB of address space: ") + std::strerror(errno);
        close(fd);
        return nullptr;
    }
    auto* base = static_cast<uint8_t*>(r);
    for (const View& v : views(ram)) {
        if (mmap(base + v.at, v.size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) {
            *error = std::string("can't map guest RAM: ") + std::strerror(errno);
            close(fd);
            return nullptr;
        }
    }
    close(fd);  // the mappings keep it
    return base;  // the virtual memory is the reservation's own pages: no access until committed
}

size_t host_page() {
    static const size_t page = size_t(sysconf(_SC_PAGESIZE));
    return page;
}
void host_commit(uint8_t* p, size_t n) {
    if (mprotect(p, n, PROT_READ | PROT_WRITE) != 0) {
        std::fprintf(stderr, "qilin_run: can't commit %zu bytes of guest virtual memory: %s\n", n, std::strerror(errno));
        std::abort();
    }
}
void host_free(uint8_t* p, size_t n) {  // fresh inaccessible pages, the old ones returned to the host
    mmap(p, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
}
#endif

uint8_t* g_base = nullptr;

}  // namespace

uint8_t* map_guest_ram(uint32_t ram_size, std::string* error) {
    if (ram_size == 0 || ram_size % 0x10000) {
        *error = "guest RAM must be a non-zero multiple of 64 KB";
        return nullptr;
    }
    uint8_t* base = map(ram_size, error);
    if (!base) return nullptr;
    rt::g_ram_aliases = true, rt::g_ram_size = ram_size, g_base = base;
    // Every page of RAM made now, not when the game first touches it: the
    // host's first touch of each (a fault, and a page zeroed) otherwise
    // lands in the frames where a new scene's data comes in, a stutter.
    // The memory is the game's own to fill, all of it in use soon anyway.
    const size_t page = host_page();
    for (size_t at = 0; at < ram_size; at += page) base[at] = 0;
    return base;
}

// The host pages under guest pages [at, at + size) of the virtual memory:
// committing makes every host page they touch accessible (a host page
// may be larger than the guest's 4 KB); freeing returns only the host
// pages they cover whole, as others may hold committed guest pages too.
void commit_guest_pages(uint32_t at, uint32_t size) {
    if (!g_base || !size) return;
    const size_t page = host_page();
    const size_t lo = size_t(at) & ~(page - 1), hi = (size_t(at) + size + page - 1) & ~(page - 1);
    host_commit(g_base + lo, hi - lo);
}
void free_guest_pages(uint32_t at, uint32_t size) {
    if (!g_base || !size) return;
    const size_t page = host_page();
    const size_t lo = (size_t(at) + page - 1) & ~(page - 1), hi = (size_t(at) + size) & ~(page - 1);
    if (hi > lo) host_free(g_base + lo, hi - lo);
}

}  // namespace qilin::host
