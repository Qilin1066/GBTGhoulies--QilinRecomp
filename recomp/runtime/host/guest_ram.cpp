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
//
// Lifted code doesn't check its accesses (unless built with
// QILIN_CHECKED_MEMORY): every guest address is inside the reservation, and
// what isn't mapped there faults. The fault handler here turns such a fault
// into the runtime's stop, naming the guest address, as a checked access
// would; a fault anywhere else goes on to whatever handled it before.
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "host.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
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
// Kept: still the process's, but no access (committed pages only).
void host_keep(uint8_t* p, size_t n) {
    DWORD was = 0;
    if (!VirtualProtect(p, n, PAGE_NOACCESS, &was)) VirtualFree(p, n, MEM_DECOMMIT);
}
void host_take_back(uint8_t* p, size_t n) {
    DWORD was = 0;
    if (!VirtualProtect(p, n, PAGE_READWRITE, &was)) host_commit(p, n);
}
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
void host_keep(uint8_t* p, size_t n) {  // the same pages, no access
    if (mprotect(p, n, PROT_NONE) != 0) host_free(p, n);
}
void host_take_back(uint8_t* p, size_t n) { host_commit(p, n); }
#endif

uint8_t* g_base = nullptr;

// The stop for an access to guest memory that isn't mapped: written with
// nothing but write(2)-like calls, as it runs in a fault handler.
void stop_at(uintptr_t host_address) {
    const uint32_t guest = uint32_t(host_address - uintptr_t(g_base));
    char line[] = "qilin: stopped: guest memory access at 00000000, which isn't mapped (or is read-only)\n";
    char* digits = std::strchr(line, '0');  // the eight digits
    for (int k = 7; k >= 0; --k) digits[7 - k] = "0123456789ABCDEF"[(guest >> (4 * k)) & 0xF];
    const int n = int(sizeof line - 1);
#if defined(_WIN32)
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(n), &written, nullptr);
#else
    [[maybe_unused]] const ssize_t written = write(2, line, size_t(n));
#endif
    std::_Exit(3);
}

bool in_reservation(uintptr_t a) { return g_base && a >= uintptr_t(g_base) && a - uintptr_t(g_base) < kReservation; }

#if defined(_WIN32)
LONG CALLBACK on_fault(EXCEPTION_POINTERS* e) {
    const EXCEPTION_RECORD* r = e->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2 && in_reservation(uintptr_t(r->ExceptionInformation[1])))
        stop_at(uintptr_t(r->ExceptionInformation[1]));
    return EXCEPTION_CONTINUE_SEARCH;
}
void install_fault_handler() { AddVectoredExceptionHandler(1, on_fault); }
#else
struct sigaction g_previous_segv{}, g_previous_bus{};
void on_fault(int sig, siginfo_t* info, void* context) {
    if (in_reservation(uintptr_t(info->si_addr))) stop_at(uintptr_t(info->si_addr));
    // Not guest memory: as before this handler was installed.
    const struct sigaction& previous = sig == SIGBUS ? g_previous_bus : g_previous_segv;
    if (previous.sa_flags & SA_SIGINFO) {
        if (previous.sa_sigaction) return previous.sa_sigaction(sig, info, context);
    } else if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN && previous.sa_handler) {
        return previous.sa_handler(sig);
    }
    signal(sig, SIG_DFL);  // the fault happens again on return, and ends the process as it would have
}
void install_fault_handler() {
    struct sigaction a{};
    a.sa_sigaction = on_fault;
    a.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&a.sa_mask);
    sigaction(SIGSEGV, &a, &g_previous_segv);
    sigaction(SIGBUS, &a, &g_previous_bus);  // macOS reports some protection faults as SIGBUS
}
#endif


}  // namespace

uint8_t* map_guest_ram(uint32_t ram_size, std::string* error) {
    if (ram_size == 0 || ram_size % 0x10000) {
        *error = "guest RAM must be a non-zero multiple of 64 KB";
        return nullptr;
    }
    uint8_t* base = map(ram_size, error);
    if (!base) return nullptr;
    rt::g_ram_aliases = true, rt::g_ram_size = ram_size, g_base = base;
    install_fault_handler();
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
// may be larger than the guest's 4 KB); freeing takes only the host
// pages they cover whole, as others may hold committed guest pages too.
//
// Freed pages aren't handed back to the host at once: up to kKeep bytes of
// them stay the process's, with no access (a stray read still faults), and
// are given out again as they are when the title next commits them. The
// game's heap decommits and commits megabytes as scenes change, and pages
// fresh from the host cost a fault and a zeroing each as they're first
// touched, in the frame that loads the scene: 50 ms of it in one, measured.
// What the title reads in a page it commits is zeros either way
// (VirtualMemory::commit clears it).
namespace {
constexpr size_t kKeep = size_t(64) << 20;
struct Pages {
    std::vector<bool> committed, kept;  // by host page, from the reservation's start
    size_t kept_bytes = 0;
};
Pages& pages() {
    static Pages p;
    return p;
}
// Calls f(first, count) for each run of pages in [lo, hi) for which is(page) holds.
template <typename Is, typename F>
void runs(size_t lo, size_t hi, Is is, F f) {
    for (size_t k = lo; k < hi;) {
        if (!is(k)) {
            ++k;
            continue;
        }
        size_t end = k + 1;
        while (end < hi && is(end)) ++end;
        f(k, end - k);
        k = end;
    }
}
}  // namespace

void commit_guest_pages(uint32_t at, uint32_t size) {
    if (!g_base || !size) return;
    const size_t page = host_page();
    const size_t lo = (size_t(at) & ~(page - 1)) / page, hi = ((size_t(at) + size + page - 1) & ~(page - 1)) / page;
    Pages& p = pages();
    if (p.committed.size() < hi) p.committed.resize(hi), p.kept.resize(hi);
    runs(lo, hi, [&](size_t k) { return bool(p.kept[k]); }, [&](size_t first, size_t n) {
        host_take_back(g_base + first * page, n * page);
        p.kept_bytes -= n * page;
        for (size_t k = first; k < first + n; ++k) p.kept[k] = false;
    });
    runs(lo, hi, [&](size_t k) { return !p.committed[k]; }, [&](size_t first, size_t n) {
        host_commit(g_base + first * page, n * page);
        for (size_t k = first; k < first + n; ++k) p.committed[k] = true;
    });
}
void free_guest_pages(uint32_t at, uint32_t size) {
    if (!g_base || !size) return;
    const size_t page = host_page();
    const size_t lo = ((size_t(at) + page - 1) & ~(page - 1)) / page, hi = std::min(((size_t(at) + size) & ~(page - 1)) / page, pages().committed.size());
    Pages& p = pages();
    // The ones in use until now: kept while there's room, else handed back.
    runs(lo, hi, [&](size_t k) { return p.committed[k] && !p.kept[k]; }, [&](size_t first, size_t n) {
        if (p.kept_bytes + n * page <= kKeep) {
            host_keep(g_base + first * page, n * page);
            p.kept_bytes += n * page;
            for (size_t k = first; k < first + n; ++k) p.kept[k] = true;
        } else {
            host_free(g_base + first * page, n * page);
            for (size_t k = first; k < first + n; ++k) p.committed[k] = false;
        }
    });
}

}  // namespace qilin::host
