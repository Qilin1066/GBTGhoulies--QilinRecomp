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

// A wait or wake-up between threads. The runtime has one thread so far, so
// this can only mean a lock held across a missing thread, or a deadlock.
[[noreturn]] void needs_threads(uint32_t ordinal, const char* what) {
    static std::string why;
    why = strf("%s: needs another thread, and the runtime has only the main thread so far", what);
    rt::trap(kernel_addr(ordinal), why.c_str());
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
        if (ld(c, cs + kCsOwner) != me) needs_threads(277, "RtlEnterCriticalSection on a lock another thread holds");
        st(c, cs + kCsRecursion, ld(c, cs + kCsRecursion) + 1);
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
        if (int32_t(lock) >= 0) needs_threads(294, "RtlLeaveCriticalSection with waiters");
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

// NTSTATUS NtAllocateVirtualMemory(PVOID* BaseAddress, ULONG_PTR ZeroBits,
//     PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect) (184)
void nt_allocate_virtual_memory(rt::Context& c) {
    const uint32_t pbase = arg(c, 0), psize = arg(c, 2), type = arg(c, 3), protect = arg(c, 4);
    uint32_t base = ld(c, pbase);
    const uint32_t size = ld(c, psize);
    VirtualMemory& v = vm();
    uint32_t status = kStatusSuccess;
    if (!(type & (kMemCommit | kMemReserve)) || size == 0) {
        status = kStatusInvalidParameter;
    } else if ((type & kMemReserve) || base == 0) {  // a new reservation, committed too if asked
        const uint32_t at = v.reserve(base, size, protect, (type & kMemTopDown) != 0);
        if (!at) {
            status = base ? kStatusConflictingAddresses : kStatusNoMemory;
        } else {
            const uint32_t bytes = v.region_size(at);
            if (type & kMemCommit) v.commit(at, bytes, protect);
            st(c, pbase, at);
            st(c, psize, bytes);
        }
    } else {  // commit inside an existing reservation
        const uint32_t first = base & ~(VirtualMemory::kPage - 1);
        const uint32_t end = uint32_t((uint64_t(base) + size + VirtualMemory::kPage - 1) & ~uint64_t(VirtualMemory::kPage - 1));
        if (!v.commit(base, size, protect)) {
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
    VirtualMemory& v = vm();
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
uint32_t contiguous(uint32_t bytes, uint32_t lowest, uint32_t highest, uint32_t align, uint32_t protect) {
    VirtualMemory& v = vm();
    const uint32_t a = std::max<uint32_t>(align, VirtualMemory::kPage);
    const uint32_t at = v.reserve(0, bytes, protect, true, a & (a - 1) ? VirtualMemory::kPage : a, lowest & 0x7FFFFFFFu,
                                  highest >= 0x80000000u ? 0x7FFFFFFFu : highest);
    if (at) v.commit(at, v.region_size(at), protect);
    return at;
}
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
    const uint32_t committed = uint32_t(v.committed_bytes() / page);
    const uint32_t used = image + stack + kernel + committed;
    const uint32_t f[8] = {total, used < total ? total - used : 0, uint32_t(v.committed_bytes()), uint32_t(v.reserved_bytes()), 0, 0, stack, image};
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
constexpr Setting kSettings[] = {
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
// Timers are recorded with their due time (100 ns units on the interrupt
// clock, which starts with the runtime) and DPC. Firing them belongs to the
// scheduler, which isn't there yet; until then a due timer is reported when
// the runtime stops. XAPI's first one is its six-hour auto power-off.
struct Timer {
    uint64_t due;      // interrupt time, 100 ns
    uint32_t period;   // ms, 0 for one-shot
    uint32_t dpc;
};
std::map<uint32_t, Timer>& timers() {
    static std::map<uint32_t, Timer> t;
    return t;
}
uint64_t interrupt_time() {  // 100 ns since the runtime started
    static const auto start = std::chrono::steady_clock::now();
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count() / 100);
}
// BOOLEAN KeSetTimerEx(PKTIMER, LARGE_INTEGER DueTime, LONG Period, PKDPC) (150);
// KeSetTimer (149) is the same with no period. A negative due time is relative.
uint32_t set_timer(rt::Context& c, uint32_t t, uint32_t lo, uint32_t hi, uint32_t period, uint32_t dpc) {
    const int64_t due = int64_t(uint64_t(hi) << 32 | lo);
    const uint64_t now = interrupt_time();
    const uint64_t at = due < 0 ? now + uint64_t(-due) : uint64_t(due);  // absolute times: treated as the interrupt clock's
    const bool was = timers().erase(t) != 0;
    timers()[t] = Timer{at, period, dpc};
    rt::st8(mem(c), t + 3, 1, 0);  // Header.Inserted
    st(c, t + 4, 0);               // SignalState: not yet
    st(c, t + 0x10, uint32_t(at));
    st(c, t + 0x14, uint32_t(at >> 32));
    st(c, t + 0x20, dpc);
    st(c, t + 0x24, period);
    return was ? 1 : 0;
}
void ke_set_timer(rt::Context& c) { ret(c, set_timer(c, arg(c, 0), arg(c, 1), arg(c, 2), 0, arg(c, 3)), 16); }
void ke_set_timer_ex(rt::Context& c) { ret(c, set_timer(c, arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4)), 20); }
// BOOLEAN KeCancelTimer(PKTIMER) (97)
void ke_cancel_timer(rt::Context& c) {
    const uint32_t t = arg(c, 0);
    const bool was = timers().erase(t) != 0;
    rt::st8(mem(c), t + 3, 0, 0);
    ret(c, was ? 1u : 0u, 4);
}

struct KernelFn {
    const char* name;
    uint16_t arg_bytes;  // checked against the export table at registration
    HleFn fn;
};
constexpr KernelFn kKernel[] = {
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

void register_kernel() {
    for (const KernelFn& k : kKernel) {
        const KernelExport* e = kernel_export(k.name);
        if (!e || e->data || e->arg_bytes != k.arg_bytes) {
            std::fprintf(stderr, "qilin: kernel replacement %s doesn't match the export table\n", k.name);
            std::abort();
        }
        register_hle(kernel_addr(e->ordinal), e->name, k.fn);
    }
}

// ---------------------------------------------------------------- KPCR
// fs points at the processor control region. Its first 0x1C bytes are the
// NT_TIB of the running thread (ExceptionList, StackBase, StackLimit, …,
// Self at +0x18); then SelfPcr (+0x1C), Prcb (+0x20, pointing at PrcbData),
// Irql (+0x24) and PrcbData (+0x28), whose first field is CurrentThread.
void init_main_thread(rt::Context& c, uint32_t kpcr, uint32_t kthread, uint32_t stack_top, uint32_t stack_size) {
    std::memset(c.mem + kpcr, 0, 0x300);
    std::memset(c.mem + kthread, 0, kKThreadSize);
    st(c, kpcr + 0x00, 0xFFFFFFFFu);              // ExceptionList: the end of the chain
    st(c, kpcr + 0x04, stack_top);                // StackBase
    st(c, kpcr + 0x08, stack_top - stack_size);   // StackLimit
    st(c, kpcr + 0x18, kpcr);                     // NT_TIB.Self
    st(c, kpcr + 0x1C, kpcr);                     // SelfPcr
    st(c, kpcr + 0x20, kpcr + 0x28);              // Prcb
    st(c, kpcr + 0x28, kthread);                  // PrcbData.CurrentThread
    c.fs_base = kpcr;
}

uint32_t current_thread(const rt::Context& c) { return ld(c, c.fs_base + 0x28); }

}  // namespace qilin::host
