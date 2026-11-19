// Native replacements for the Xbox kernel (xboxkrnl.exe) functions the game's
// libraries import, by ordinal, and the processor control region (KPCR) that
// fs points at. Each follows the kernel's documented behaviour on the guest's
// own data structures, so lifted code that looks inside them (the C runtime's
// locks, XAPI's thread data) sees what it would on the Xbox.
#include <algorithm>
#include <chrono>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "host.hpp"
#include "sched.hpp"
#include "strf.hpp"

namespace qilin::host {
namespace {

rt::Mem mem(const rt::Context& c) { return rt::Mem{c.mem, c.mem_size}; }
uint32_t ld(const rt::Context& c, uint32_t a) { return rt::ld32(mem(c), a, 0); }
void st(const rt::Context& c, uint32_t a, uint32_t v) { rt::st32(mem(c), a, v, 0); }

// ---------------------------------------------------------------- critical sections
// RTL_CRITICAL_SECTION (28 bytes): a KEVENT (Type, Absolute, Size, Inserted,
// SignalState, WaitListHead), then LockCount (+0x10, -1 when free),
// RecursionCount (+0x14) and OwningThread (+0x18, the owner's KTHREAD).
constexpr uint32_t kCsLock = 0x10, kCsRecursion = 0x14, kCsOwner = 0x18;

void init_event(const rt::Context& c, uint32_t ev, uint8_t type, bool signalled) {
    const rt::Mem m = mem(c);
    rt::st8(m, ev, type, 0);                       // NotificationEvent 0, SynchronizationEvent 1
    rt::st8(m, ev + 1, 0, 0);                      // Absolute
    rt::st8(m, ev + 2, 4, 0);                      // Size: sizeof(KEVENT) / 4
    rt::st8(m, ev + 3, 0, 0);                      // Inserted
    st(c, ev + 4, signalled ? 1u : 0u);            // SignalState
    st(c, ev + 8, ev + 8);                         // WaitListHead: empty, linked to itself
    st(c, ev + 12, ev + 8);
}

// VOID RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION) (291)
void rtl_initialize_critical_section(rt::Context& c) {
    const uint32_t cs = arg(c, 0);
    init_event(c, cs, 1, false);
    st(c, cs + kCsLock, 0xFFFFFFFFu);
    st(c, cs + kCsRecursion, 0);
    st(c, cs + kCsOwner, 0);
    ret(c, c.eax, 4);
}

// VOID RtlEnterCriticalSection(PRTL_CRITICAL_SECTION) (277)
void rtl_enter_critical_section(rt::Context& c) {
    const uint32_t cs = arg(c, 0), me = current_thread(c);
    const uint32_t lock = ld(c, cs + kCsLock) + 1;
    st(c, cs + kCsLock, lock);
    if (lock != 0) {  // already held
        if (ld(c, cs + kCsOwner) == me) {
            st(c, cs + kCsRecursion, ld(c, cs + kCsRecursion) + 1);
            return ret(c, c.eax, 4);
        }
        sched::wait_object(cs);  // the section's event, which the owner sets when it leaves
        st(c, cs + kCsOwner, me);
        st(c, cs + kCsRecursion, 1);
    } else {
        st(c, cs + kCsOwner, me);
        st(c, cs + kCsRecursion, 1);
    }
    ret(c, c.eax, 4);
}

// VOID RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION) (294)
void rtl_leave_critical_section(rt::Context& c) {
    const uint32_t cs = arg(c, 0);
    const uint32_t rec = ld(c, cs + kCsRecursion) - 1;
    st(c, cs + kCsRecursion, rec);
    const uint32_t lock = ld(c, cs + kCsLock) - 1;
    st(c, cs + kCsLock, lock);
    if (rec == 0) {
        st(c, cs + kCsOwner, 0);
        if (int32_t(lock) >= 0) sched::set_event(cs);  // a waiter takes it
    }
    ret(c, c.eax, 4);
}

// BOOLEAN RtlTryEnterCriticalSection(PRTL_CRITICAL_SECTION) (306)
void rtl_try_enter_critical_section(rt::Context& c) {
    const uint32_t cs = arg(c, 0), me = current_thread(c);
    uint32_t got = 1;
    if (ld(c, cs + kCsLock) == 0xFFFFFFFFu) {
        st(c, cs + kCsLock, 0);
        st(c, cs + kCsOwner, me);
        st(c, cs + kCsRecursion, 1);
    } else if (ld(c, cs + kCsOwner) == me) {
        st(c, cs + kCsLock, ld(c, cs + kCsLock) + 1);
        st(c, cs + kCsRecursion, ld(c, cs + kCsRecursion) + 1);
    } else {
        got = 0;
    }
    ret(c, (c.eax & 0xFFFFFF00u) | got, 4);
}

// VOID RtlDeleteCriticalSection(PRTL_CRITICAL_SECTION): not a kernel export
// on the Xbox (XAPI's is empty), so nothing here.


// ---------------------------------------------------------------- memory
constexpr uint32_t kStatusSuccess = 0, kStatusInvalidParameter = 0xC000000D, kStatusNoMemory = 0xC0000017,
                   kStatusConflictingAddresses = 0xC0000018, kStatusMemoryNotAllocated = 0xC00000A0;
constexpr uint32_t kMemCommit = 0x1000, kMemReserve = 0x2000, kMemDecommit = 0x4000, kMemRelease = 0x8000,
                   kMemTopDown = 0x100000;

// The address space an address is in: the title's virtual memory above
// RAM, or RAM (an allocation at a fixed address there, or contiguous memory).
VirtualMemory& space_of(uint32_t addr) { return addr >= virtual_memory().lo() ? virtual_memory() : vm(); }

// NTSTATUS NtAllocateVirtualMemory(PVOID* BaseAddress, ULONG_PTR ZeroBits,
//     PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect) (184)
// As the Xbox's kernel, the address space is separate from RAM: a
// reservation costs none of it, committed pages do, and committing more
// than there is fails with STATUS_NO_MEMORY. Reservations at a fixed
// address in RAM's range are RAM, as the runtime's address space has it.
void nt_allocate_virtual_memory(rt::Context& c) {
    const uint32_t pbase = arg(c, 0), psize = arg(c, 2), type = arg(c, 3), protect = arg(c, 4);
    uint32_t base = ld(c, pbase);
    const uint32_t size = ld(c, psize);
    uint32_t status = kStatusSuccess;
    if (!(type & (kMemCommit | kMemReserve)) || size == 0) {
        status = kStatusInvalidParameter;
    } else if ((type & kMemReserve) || base == 0) {  // a new reservation, committed too if asked
        VirtualMemory& v = base ? space_of(base) : virtual_memory();
        const uint32_t at = v.reserve(base, size, protect, (type & kMemTopDown) != 0);
        const uint32_t bytes = at ? v.region_size(at) : 0;
        // RAM's reservations count as soon as they're made; virtual memory, its committed pages.
        const uint64_t more = &v == &vm() || !(type & kMemCommit) ? 0 : bytes;
        if (at && !physical_room(more)) {
            v.release(at);
            out_of_memory("NtAllocateVirtualMemory", size);
            status = kStatusNoMemory;
        } else if (!at) {
            status = base ? kStatusConflictingAddresses : kStatusNoMemory;
            if (!base) out_of_memory("NtAllocateVirtualMemory", size);
        } else {
            if (type & kMemCommit) v.commit(at, bytes, protect);
            st(c, pbase, at);
            st(c, psize, bytes);
        }
    } else {  // commit inside an existing reservation
        VirtualMemory& v = space_of(base);
        const uint32_t first = base & ~(VirtualMemory::kPage - 1);
        const uint32_t end = uint32_t((uint64_t(base) + size + VirtualMemory::kPage - 1) & ~uint64_t(VirtualMemory::kPage - 1));
        if (&v == &virtual_memory() && !physical_room(v.uncommitted_bytes(base, size))) {
            out_of_memory("NtAllocateVirtualMemory (commit)", size);
            status = kStatusNoMemory;
        } else if (!v.commit(base, size, protect)) {
            status = kStatusConflictingAddresses;
        } else {
            st(c, pbase, first);
            st(c, psize, end - first);
        }
    }
    ret(c, status, 20);
}

// NTSTATUS NtFreeVirtualMemory(PVOID* BaseAddress, PSIZE_T RegionSize, ULONG FreeType) (199)
void nt_free_virtual_memory(rt::Context& c) {
    const uint32_t pbase = arg(c, 0), psize = arg(c, 1), type = arg(c, 2);
    const uint32_t base = ld(c, pbase), size = ld(c, psize);
    VirtualMemory& v = space_of(base);
    uint32_t status = kStatusSuccess;
    if (type == kMemRelease) {
        const uint32_t bytes = v.region_size(base);
        if (!bytes || (size != 0 && size != bytes)) status = kStatusMemoryNotAllocated;
        else {
            v.release(base);
            st(c, psize, bytes);
        }
    } else if (type == kMemDecommit) {
        if (!v.decommit(base, size)) status = kStatusMemoryNotAllocated;
    } else {
        status = kStatusInvalidParameter;
    }
    ret(c, status, 12);
}

// NTSTATUS NtQueryVirtualMemory(PVOID BaseAddress, PMEMORY_BASIC_INFORMATION) (217)
// MEMORY_BASIC_INFORMATION: BaseAddress, AllocationBase, AllocationProtect,
// RegionSize, State, Protect, Type.
void nt_query_virtual_memory(rt::Context& c) {
    const uint32_t addr = arg(c, 0), out = arg(c, 1);
    const VirtualMemory& v = vm();
    const uint32_t page = addr & ~(VirtualMemory::kPage - 1);
    VirtualMemory::Info i{};
    if (auto q = v.query(addr)) {
        i = *q;
    } else if (auto vq = virtual_memory().query(addr)) {
        i = *vq;
    } else if (addr < v.lo()) {  // the image: committed, MEM_IMAGE
        i = {page, 0x10000, 0x40, v.lo() - page, VirtualMemory::kMemCommit, 0x40, 0x1000000};
    } else if (addr < memory_map().ram_size) {  // the main stack and the kernel's area: committed
        i = {page, v.hi(), 4, memory_map().ram_size - page, VirtualMemory::kMemCommit, 4, VirtualMemory::kMemPrivate};
    } else {
        ret(c, kStatusInvalidParameter, 8);
        return;
    }
    const uint32_t f[7] = {i.base, i.allocation_base, i.allocation_protect, i.size, i.state, i.protect, i.type};
    for (uint32_t k = 0; k < 7; ++k) st(c, out + 4 * k, f[k]);
    ret(c, kStatusSuccess, 8);
}

// PVOID MmAllocateContiguousMemoryEx(SIZE_T NumberOfBytes, ULONG_PTR LowestAcceptableAddress,
//     ULONG_PTR HighestAcceptableAddress, ULONG_PTR Alignment, ULONG Protect) (166)
// The Xbox hands out physical pages from the top of RAM, mapped at
// 0x80000000 + physical; the runtime's address space is flat, so the guest
// gets the RAM address itself, from the top down, within the limits.
void mm_allocate_contiguous_memory_ex(rt::Context& c) {
    ret(c, contiguous(arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4)), 20);
}
// PVOID MmAllocateContiguousMemory(SIZE_T NumberOfBytes) (165)
void mm_allocate_contiguous_memory(rt::Context& c) { ret(c, contiguous(arg(c, 0), 0, 0xFFFFFFFFu, 0, 4), 4); }
// VOID MmFreeContiguousMemory(PVOID BaseAddress) (171)
void mm_free_contiguous_memory(rt::Context& c) {
    vm().release(arg(c, 0));
    ret(c, c.eax, 4);
}
// VOID MmPersistContiguousMemory(PVOID, SIZE_T, BOOLEAN) (178): keeps memory
// across a reboot into another title. Nothing to do.
void mm_persist_contiguous_memory(rt::Context& c) { ret(c, c.eax, 12); }

// NTSTATUS MmQueryStatistics(PMM_STATISTICS) (181): Length (must be 36),
// TotalPhysicalPages, AvailablePages, VirtualMemoryBytesCommitted,
// VirtualMemoryBytesReserved, CachePagesCommitted, PoolPagesCommitted,
// StackPagesCommitted, ImagePagesCommitted.
void mm_query_statistics(rt::Context& c) {
    const uint32_t out = arg(c, 0);
    if (ld(c, out) != 36) {
        ret(c, kStatusInvalidParameter, 4);
        return;
    }
    const VirtualMemory& v = vm();
    const uint32_t page = VirtualMemory::kPage, total = memory_map().ram_size / page;
    const uint32_t image = v.lo() / page, stack = MemoryMap::kStackSize / page, kernel = (memory_map().ram_size - memory_map().stack_top()) / page;
    const VirtualMemory& vv = virtual_memory();
    const uint64_t committed_bytes = v.committed_bytes() + vv.committed_bytes();
    const uint32_t used = image + stack + kernel + uint32_t(committed_bytes / page);
    const uint32_t f[8] = {total, used < total ? total - used : 0, uint32_t(committed_bytes),
                           uint32_t(v.reserved_bytes() + vv.reserved_bytes()), 0, 0, stack, image};
    for (uint32_t k = 0; k < 8; ++k) st(c, out + 4 + 4 * k, f[k]);
    ret(c, kStatusSuccess, 4);
}

// ---------------------------------------------------------------- HAL
// VOID HalRegisterShutdownNotification(PHAL_SHUTDOWN_REGISTRATION, BOOLEAN Register) (47):
// the runtime never shuts the console down, so the routines never run.
void hal_register_shutdown_notification(rt::Context& c) { ret(c, c.eax, 8); }

// ---------------------------------------------------------------- dispatcher objects
// VOID KeInitializeDpc(PKDPC, PKDEFERRED_ROUTINE, PVOID DeferredContext) (107):
// Type (DpcObject 19), Inserted, DpcListEntry, DeferredRoutine (+0x0C),
// DeferredContext, SystemArgument1, SystemArgument2.
void ke_initialize_dpc(rt::Context& c) {
    const uint32_t dpc = arg(c, 0);
    rt::st16(mem(c), dpc, 19, 0);
    rt::st8(mem(c), dpc + 2, 0, 0);
    st(c, dpc + 4, 0);
    st(c, dpc + 8, 0);
    st(c, dpc + 0x0C, arg(c, 1));
    st(c, dpc + 0x10, arg(c, 2));
    ret(c, c.eax, 12);
}

// VOID KeInitializeTimerEx(PKTIMER, TIMER_TYPE) (113): a dispatcher header
// (Type 8 + TimerType, Size 10 dwords, not signalled, empty wait list), then
// DueTime, TimerListEntry, Dpc and Period, all clear.
void ke_initialize_timer_ex(rt::Context& c) {
    const uint32_t t = arg(c, 0);
    init_event(c, t, uint8_t(8 + arg(c, 1)), false);
    rt::st8(mem(c), t + 2, 10, 0);
    for (uint32_t k = 0x10; k < 0x28; k += 4) st(c, t + k, 0);
    ret(c, c.eax, 8);
}

// ---------------------------------------------------------------- settings
// NTSTATUS ExQueryNonVolatileSetting(ULONG ValueIndex, PULONG Type, PVOID Value,
//     ULONG ValueLength, PULONG ResultLength) (24): the console's EEPROM
// settings. The runtime has no console: a North American retail Xbox with
// factory settings, English, 60 Hz.
struct Setting {
    uint32_t index, value;
    const char* what;
};
// The audio setting (XC_AUDIO_FLAGS): stereo, or surround with Dolby
// Digital (2 | 0x10000) when the sound device takes 5.1, so the game and
// DirectSound's 3D calculator pan to the back speakers (set_audio_surround).
uint32_t g_audio_flags = 0;
Setting kSettings[] = {
    {0x002, 0, "time zone bias"},        {0x007, 1, "language: English"},
    {0x008, 0x00400000, "video: 60 Hz"}, {0x009, 0, "audio: stereo"},
    {0x00A, 0, "parental control, games"}, {0x00B, 0, "parental control password"},
    {0x00C, 0, "parental control, movies"}, {0x011, 0, "misc flags: no auto power-down"},
    {0x012, 1, "DVD region 1"},          {0x103, 0x00400100, "AV region: NTSC-M"},
    {0x104, 1, "game region: North America"},
};
void ex_query_non_volatile_setting(rt::Context& c) {
    const uint32_t index = arg(c, 0), ptype = arg(c, 1), value = arg(c, 2), len = arg(c, 3), plen = arg(c, 4);
    const auto it = std::ranges::find(kSettings, index, &Setting::index);
    if (index == 0x009 && it != std::end(kSettings)) it->value = g_audio_flags;
    if (it == std::end(kSettings)) {
        static std::string why;
        why = strf("ExQueryNonVolatileSetting: setting %#x isn't modelled yet", index);
        rt::trap(kernel_addr(24), why.c_str());
    }
    if (len < 4) {
        ret(c, 0xC0000023, 20);  // STATUS_BUFFER_TOO_SMALL
        return;
    }
    if (ptype) st(c, ptype, 4);  // REG_DWORD
    st(c, value, it->value);
    if (plen) st(c, plen, 4);
    ret(c, kStatusSuccess, 20);
}

// ---------------------------------------------------------------- Rtl
// ULONG RtlNtStatusToDosError(NTSTATUS) (301): the Win32 error XAPI stores as
// the last error. The NT table, for the statuses this runtime returns and
// the file system's; anything else stops, so a gap can't pass as an error.
struct StatusMap {
    uint32_t status, error;
};
constexpr StatusMap kDosErrors[] = {
    {0x00000000, 0},   {0x00000102, 258}, {0x00000103, 997}, {0x80000005, 234}, {0x80000006, 18},
    {0xC0000002, 1},   {0xC0000008, 6},   {0xC000000D, 87},  {0xC000000F, 2},   {0xC0000010, 1},
    {0xC0000011, 38},  {0xC0000017, 8},   {0xC0000018, 487}, {0xC0000022, 5},   {0xC0000023, 122},
    {0xC0000033, 123}, {0xC0000034, 2},   {0xC0000035, 183}, {0xC000003A, 3},   {0xC0000043, 32},
    {0xC000007F, 112}, {0xC00000A0, 487}, {0xC00000BA, 5},   {0xC0000101, 145}, {0xC0000103, 267},
    {0xC0000120, 995},
};
void rtl_nt_status_to_dos_error(rt::Context& c) {
    const uint32_t status = arg(c, 0);
    const auto it = std::ranges::find(kDosErrors, status, &StatusMap::status);
    if (it == std::end(kDosErrors)) {
        static std::string why;
        why = strf("RtlNtStatusToDosError: status %08X isn't in the table yet", status);
        rt::trap(kernel_addr(301), why.c_str());
    }
    ret(c, it->error, 4);
}

// ---------------------------------------------------------------- timers
// BOOLEAN KeSetTimerEx(PKTIMER, LARGE_INTEGER DueTime, LONG Period, PKDPC) (150);
// KeSetTimer (149) is the same with no period. A negative due time is
// relative. They fire on the scheduler's clock (sched.cpp).
int64_t due_of(rt::Context& c) { return int64_t(uint64_t(arg(c, 2)) << 32 | arg(c, 1)); }
void ke_set_timer(rt::Context& c) { ret(c, sched::set_timer(c, arg(c, 0), due_of(c), 0, arg(c, 3)), 16); }
void ke_set_timer_ex(rt::Context& c) { ret(c, sched::set_timer(c, arg(c, 0), due_of(c), arg(c, 3), arg(c, 4)), 20); }
// BOOLEAN KeCancelTimer(PKTIMER) (97)
void ke_cancel_timer(rt::Context& c) { ret(c, sched::cancel_timer(c, arg(c, 0)) ? 1u : 0u, 4); }

struct KernelFn {
    const char* name;
    uint16_t arg_bytes;  // checked against the export table at registration
    HleFn fn;
};
// ---------------------------------------------------------------- strings
// ANSI_STRING / STRING: USHORT Length, USHORT MaximumLength, PCHAR Buffer.

// VOID RtlInitAnsiString(PANSI_STRING, PCSZ) (289)
void rtl_init_ansi_string(rt::Context& c) {
    const uint32_t s = arg(c, 0), src = arg(c, 1);
    uint32_t n = 0;
    if (src)
        while (rt::ld8(mem(c), src + n, 0) && n < 0xFFFE) ++n;
    rt::st16(mem(c), s, uint16_t(n), 0);
    rt::st16(mem(c), s + 2, uint16_t(src ? n + 1 : 0), 0);
    st(c, s + 4, src);
    ret(c, c.eax, 8);
}

// BOOLEAN RtlEqualString(PSTRING, PSTRING, BOOLEAN CaseInsensitive) (279)
void rtl_equal_string(rt::Context& c) {
    const uint32_t a = arg(c, 0), b = arg(c, 1);
    const bool fold = (arg(c, 2) & 0xFF) != 0;
    const uint32_t n = rt::ld16(mem(c), a, 0);
    bool eq = n == rt::ld16(mem(c), b, 0);
    const uint32_t pa = ld(c, a + 4), pb = ld(c, b + 4);
    for (uint32_t k = 0; eq && k < n; ++k) {
        uint8_t x = rt::ld8(mem(c), pa + k, 0), y = rt::ld8(mem(c), pb + k, 0);
        if (fold) {  // RtlUpperChar: ASCII letters only (no NLS tables on the Xbox)
            if (x >= 'a' && x <= 'z') x = uint8_t(x - 32);
            if (y >= 'a' && y <= 'z') y = uint8_t(y - 32);
        }
        eq = x == y;
    }
    ret(c, (c.eax & ~0xFFu) | (eq ? 1u : 0u), 12);
}

// NTSTATUS RtlUnicodeStringToAnsiString(PANSI_STRING Dest, PUNICODE_STRING Src,
//     BOOLEAN AllocateDestinationString) (308)
// The Xbox kernel's code page is Latin-1: characters below 0x100 map to
// their byte, the rest to '?'. The result is NUL-terminated, and a buffer
// too short takes what fits with STATUS_BUFFER_OVERFLOW.
void rtl_unicode_string_to_ansi_string(rt::Context& c) {
    const uint32_t d = arg(c, 0), sp = arg(c, 1);
    if (arg(c, 2) & 0xFF) {
        rt::trap(0, "RtlUnicodeStringToAnsiString: AllocateDestinationString (pool allocation) isn't modelled yet");
    }
    const uint32_t chars = rt::ld16(mem(c), sp, 0) / 2u, src = ld(c, sp + 4);
    const uint32_t max = rt::ld16(mem(c), d + 2, 0), buf = ld(c, d + 4);
    uint32_t status = 0, with_nul = chars + 1;
    if (with_nul > max) {
        if (max == 0) {
            ret(c, 0x80000005, 12);
            return;
        }
        status = 0x80000005;  // STATUS_BUFFER_OVERFLOW
        with_nul = max;
    }
    const uint32_t n = with_nul - 1;
    for (uint32_t k = 0; k < n; ++k) {
        const uint16_t w = rt::ld16(mem(c), src + 2 * k, 0);
        rt::st8(mem(c), buf + k, w < 0x100 ? uint8_t(w) : uint8_t('?'), 0);
    }
    rt::st8(mem(c), buf + n, 0, 0);
    rt::st16(mem(c), d, uint16_t(n), 0);
    ret(c, status, 12);
}

// SIZE_T RtlCompareMemoryUlong(PVOID Source, SIZE_T Length, ULONG Pattern) (271):
// the bytes from the start, in whole ULONGs, that equal the pattern.
void rtl_compare_memory_ulong(rt::Context& c) {
    const uint32_t src = arg(c, 0), len = arg(c, 1) & ~3u, pat = arg(c, 2);
    uint32_t k = 0;
    while (k < len && ld(c, src + k) == pat) k += 4;
    ret(c, k, 12);
}

// VOID RtlRaiseException(PEXCEPTION_RECORD) (302): the guest's own exception
// dispatch isn't modelled; stop with the code it raised.
void rtl_raise_exception(rt::Context& c) {
    static std::string why;
    why = strf("RtlRaiseException: guest raised exception %08X at %08X", ld(c, arg(c, 0)), ld(c, arg(c, 0) + 12));
    rt::trap(0, why.c_str());
}

// VOID KeQuerySystemTime(PLARGE_INTEGER) (128): 100 ns since 1601, UTC.
void ke_query_system_time(rt::Context& c) {
    const int64_t unix_100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::system_clock::now().time_since_epoch()).count() / 100;
    rt::st64(mem(c), arg(c, 0), uint64_t(unix_100ns + 116444736000000000LL), 0);
    ret(c, c.eax, 4);
}

// ---------------------------------------------------------------- XBE sections
// XBEIMAGE_SECTION: Flags, VirtualAddress, VirtualSize, FileAddress, FileSize,
// SectionName, SectionReferenceCount (+0x18), HeadSharedPageReferenceCount
// (+0x1C), TailSharedPageReferenceCount (+0x20) — the last two point at
// USHORT counts of the sections using the first and last pages.
std::vector<DemandSection>& demand_sections() {
    static std::vector<DemandSection> d;
    return d;
}
const DemandSection* demand_section(uint32_t header) {
    for (const DemandSection& d : demand_sections())
        if (d.header == header) return &d;
    return nullptr;
}
void shared_pages(rt::Context& c, uint32_t header, int delta) {
    const uint32_t head = ld(c, header + 0x1C), tail = ld(c, header + 0x20);
    rt::st16(mem(c), head, uint16_t(rt::ld16(mem(c), head, 0) + delta), 0);
    if (tail != head) rt::st16(mem(c), tail, uint16_t(rt::ld16(mem(c), tail, 0) + delta), 0);
}

// NTSTATUS XeLoadSection(PXBEIMAGE_SECTION) (327): the first load reads the
// section's file data in and zeroes the rest of its virtual size.
void xe_load_section(rt::Context& c) {
    const uint32_t h = arg(c, 0);
    const uint32_t count = ld(c, h + 0x18);
    if (count == 0) {
        const DemandSection* d = demand_section(h);
        if (!d) {
            static std::string why;
            why = strf("XeLoadSection: %08X isn't one of the XBE's section headers", h);
            rt::trap(0, why.c_str());
        }
        rt::check(mem(c), d->va, d->vsize, 0);
        std::memset(c.mem + d->va, 0, d->vsize);
        std::memcpy(c.mem + d->va, d->bytes.data(), d->bytes.size());  // (a preloaded section never gets here: its count starts at 1)
        shared_pages(c, h, +1);
    }
    st(c, h + 0x18, count + 1);
    ret(c, 0, 4);
}

// NTSTATUS XeUnloadSection(PXBEIMAGE_SECTION) (328): the last unload frees
// the section's pages; here its memory goes back to zero.
void xe_unload_section(rt::Context& c) {
    const uint32_t h = arg(c, 0);
    const uint32_t count = ld(c, h + 0x18);
    if (count == 0) {
        ret(c, 0xC000000D, 4);  // STATUS_INVALID_PARAMETER
        return;
    }
    if (count == 1) {
        if (const DemandSection* d = demand_section(h)) std::memset(c.mem + d->va, 0, d->vsize);
        shared_pages(c, h, -1);
    }
    st(c, h + 0x18, count - 1);
    ret(c, 0, 4);
}

// ---------------------------------------------------------------- IRQL
// The current IRQL is the KPCR's Irql byte (+0x24), which XAPI reads
// directly. At DISPATCH_LEVEL and above nothing preempts the running code
// and DPCs wait; lowering it below runs what waited (sched.cpp).
constexpr uint8_t kDispatchLevel = 2;
uint8_t irql(const rt::Context& c) { return rt::ld8(mem(c), c.fs_base + 0x24, 0); }
void set_irql(const rt::Context& c, uint8_t v) { rt::st8(mem(c), c.fs_base + 0x24, v, 0); }

// KIRQL FASTCALL KfRaiseIrql(KIRQL NewIrql) (160): NewIrql in cl.
void kf_raise_irql(rt::Context& c) {
    const uint8_t old = irql(c), to = uint8_t(c.ecx);
    if (to < old) rt::trap(0, "KfRaiseIrql: lowering the IRQL (a bug check on the Xbox)");
    set_irql(c, to);
    ret(c, (c.eax & ~0xFFu) | old, 0);
}
// VOID FASTCALL KfLowerIrql(KIRQL NewIrql) (161)
void kf_lower_irql(rt::Context& c) {
    const uint8_t to = uint8_t(c.ecx);
    if (to > irql(c)) rt::trap(0, "KfLowerIrql: raising the IRQL (a bug check on the Xbox)");
    set_irql(c, to);
    ret(c, c.eax, 0);
    if (to < kDispatchLevel && rt::g_preempt.load(std::memory_order_relaxed)) rt::preempt();
}
// KIRQL KeRaiseIrqlToDpcLevel(VOID) (129)
void ke_raise_irql_to_dpc_level(rt::Context& c) {
    const uint8_t old = irql(c);
    if (old > kDispatchLevel) rt::trap(0, "KeRaiseIrqlToDpcLevel: called above DISPATCH_LEVEL");
    set_irql(c, kDispatchLevel);
    ret(c, (c.eax & ~0xFFu) | old, 0);
}

constexpr KernelFn kKernel[] = {
    {"KfRaiseIrql", 4, kf_raise_irql},
    {"KfLowerIrql", 4, kf_lower_irql},
    {"KeRaiseIrqlToDpcLevel", 0, ke_raise_irql_to_dpc_level},
    {"XeLoadSection", 4, xe_load_section},
    {"XeUnloadSection", 4, xe_unload_section},
    {"RtlInitAnsiString", 8, rtl_init_ansi_string},
    {"RtlEqualString", 12, rtl_equal_string},
    {"RtlUnicodeStringToAnsiString", 12, rtl_unicode_string_to_ansi_string},
    {"RtlCompareMemoryUlong", 12, rtl_compare_memory_ulong},
    {"RtlRaiseException", 4, rtl_raise_exception},
    {"KeQuerySystemTime", 4, ke_query_system_time},
    {"RtlInitializeCriticalSection", 4, rtl_initialize_critical_section},
    {"RtlEnterCriticalSection", 4, rtl_enter_critical_section},
    {"RtlLeaveCriticalSection", 4, rtl_leave_critical_section},
    {"RtlTryEnterCriticalSection", 4, rtl_try_enter_critical_section},
    {"NtAllocateVirtualMemory", 20, nt_allocate_virtual_memory},
    {"NtFreeVirtualMemory", 12, nt_free_virtual_memory},
    {"NtQueryVirtualMemory", 8, nt_query_virtual_memory},
    {"MmAllocateContiguousMemoryEx", 20, mm_allocate_contiguous_memory_ex},
    {"MmAllocateContiguousMemory", 4, mm_allocate_contiguous_memory},
    {"MmFreeContiguousMemory", 4, mm_free_contiguous_memory},
    {"MmPersistContiguousMemory", 12, mm_persist_contiguous_memory},
    {"MmQueryStatistics", 4, mm_query_statistics},
    {"HalRegisterShutdownNotification", 8, hal_register_shutdown_notification},
    {"KeInitializeDpc", 12, ke_initialize_dpc},
    {"KeInitializeTimerEx", 8, ke_initialize_timer_ex},
    {"ExQueryNonVolatileSetting", 20, ex_query_non_volatile_setting},
    {"RtlNtStatusToDosError", 4, rtl_nt_status_to_dos_error},
    {"KeSetTimer", 16, ke_set_timer},
    {"KeSetTimerEx", 20, ke_set_timer_ex},
    {"KeCancelTimer", 4, ke_cancel_timer},
};

}  // namespace

const KernelExport* kernel_export(std::string_view name) {
    for (uint32_t ord = 1; ord < 0x1000; ++ord)
        if (const KernelExport* e = kernel_export(ord); e && name == e->name) return e;
    return nullptr;
}

void init_kernel_data(rt::Context& c, uint32_t ordinal, uint32_t addr) {
    const rt::Mem m = mem(c);
    sched::set_data_export(ordinal, addr);
    switch (ordinal) {
        case 326: {  // XeImageFileName: an ANSI_STRING naming the running XBE, the string after it
            static constexpr char kName[] = "\\Device\\CdRom0\\default.xbe";
            const uint16_t n = uint16_t(sizeof kName - 1);
            rt::st16(m, addr, n, 0);
            rt::st16(m, addr + 2, uint16_t(n + 1), 0);
            st(c, addr + 4, addr + 8);
            for (uint16_t k = 0; k <= n; ++k) rt::st8(m, addr + 8 + k, uint8_t(kName[k]), 0);
            break;
        }
        case 40:  // HalDiskCachePartitionCount: the retail drive's three cache partitions
            st(c, addr, 3);
            break;
        default:  // the rest start as zero: no launch data (LaunchDataPage), retail hardware flags …
            break;
    }
}

void set_demand_sections(std::vector<DemandSection> sections) { demand_sections() = std::move(sections); }

void preload_section(rt::Context& c, uint32_t header) {
    st(c, header + 0x18, 1);
    shared_pages(c, header, +1);
}

void set_audio_surround(bool surround) { g_audio_flags = surround ? 0x00010002u : 0u; }

void register_kernel() {
    for (const KernelFn& k : kKernel) {
        const KernelExport* e = kernel_export(k.name);
        if (!e || e->data || e->arg_bytes != k.arg_bytes) {
            std::fprintf(stderr, "qilin: kernel replacement %s doesn't match the export table\n", k.name);
            std::abort();
        }
        register_hle(kernel_addr(e->ordinal), e->name, k.fn);
    }
    register_kernel_crypto();
    sched::register_kernel_sched();
}

// ---------------------------------------------------------------- KPCR
// fs points at the processor control region. Its first 0x1C bytes are the
// NT_TIB of the running thread (ExceptionList, StackBase, StackLimit, …,
// Self at +0x18); then SelfPcr (+0x1C), Prcb (+0x20, pointing at PrcbData),
// Irql (+0x24) and PrcbData (+0x28), whose first field is CurrentThread.
// KTHREAD: Header (0x10), MutantListHead, KernelTime, StackBase (+0x1C),
// StackLimit (+0x20), KernelStack (+0x24), TlsData (+0x28), State …
void init_main_thread(rt::Context& c, uint32_t kpcr, uint32_t kthread, uint32_t stack_top, uint32_t stack_size,
                      uint32_t tls_size) {
    std::memset(c.mem + kpcr, 0, 0x300);
    std::memset(c.mem + kthread, 0, kKThreadSize);
    st(c, kpcr + 0x00, 0xFFFFFFFFu);              // ExceptionList: the end of the chain
    st(c, kpcr + 0x04, stack_top);                // StackBase
    st(c, kpcr + 0x08, stack_top - stack_size);   // StackLimit
    st(c, kpcr + 0x18, kpcr);                     // NT_TIB.Self
    st(c, kpcr + 0x1C, kpcr);                     // SelfPcr
    st(c, kpcr + 0x20, kpcr + 0x28);              // Prcb
    st(c, kpcr + 0x28, kthread);                  // PrcbData.CurrentThread
    st(c, kthread + 0x1C, stack_top);             // StackBase
    st(c, kthread + 0x20, stack_top - stack_size);
    st(c, kthread + 0x28, tls_size ? stack_top - tls_size : 0);  // TlsData
    c.fs_base = kpcr;
}

uint32_t current_thread(const rt::Context& c) { return ld(c, c.fs_base + 0x28); }

}  // namespace qilin::host
