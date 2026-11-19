// The kernel's threads, waits, timers and DPCs (see sched.hpp).
//
// One mutex guards the scheduler. The guest thread holding the processor
// runs lifted code without it; it takes it to wait, signal, yield or be
// preempted. A clock thread keeps time: it ends waits whose time is up,
// fires timers, updates KeTickCount every millisecond, ends quanta, and
// when no guest thread runs, runs DPCs and hands the processor on.
//
// Dispatcher objects stay in guest memory in the kernel's layout (the
// DISPATCHER_HEADER's Type and SignalState are what decide a wait), as
// XAPI and the game read them; who waits on what is kept here, so the
// headers' wait lists stay empty.
#include "sched.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "host.hpp"
#include "strf.hpp"

namespace qilin::rt {
void preempt();
}

namespace qilin::host::sched {
namespace {

rt::Mem mem(const rt::Context& c) { return rt::Mem{c.mem, c.mem_size}; }
uint32_t ld(const rt::Context& c, uint32_t a) { return rt::ld32(mem(c), a, 0); }
void st(const rt::Context& c, uint32_t a, uint32_t v) { rt::st32(mem(c), a, v, 0); }

constexpr uint32_t kStatusSuccess = 0, kStatusTimeout = 0x102, kStatusAbandoned = 0x80, kStatusNoYield = 0x40000024,
                   kStatusInvalidHandle = 0xC0000008, kStatusInvalidParameter = 0xC000000D, kStatusNoMemory = 0xC0000017,
                   kStatusTypeMismatch = 0xC0000024, kStatusMutantNotOwned = 0xC0000046, kStatusSuspendCountExceeded = 0xC000004A;

// DISPATCHER_HEADER.Type
enum : uint8_t { kNotificationEvent = 0, kSynchronizationEvent = 1, kMutant = 2, kSemaphore = 5, kThreadObject = 6,
                 kNotificationTimer = 8, kSynchronizationTimer = 9 };

constexpr uint32_t kMutantOwner = 0x18, kSemaphoreLimit = 0x10;
// KTHREAD/ETHREAD fields this keeps: the header, StackBase (+0x1C),
// StackLimit (+0x20), TlsData (+0x28), Priority (+0x8C, the kernel's byte),
// and ETHREAD.UniqueThread (+0x12C), which GetCurrentThreadId reads.
constexpr uint32_t kThreadStackBase = 0x1C, kThreadStackLimit = 0x20, kThreadTls = 0x28, kThreadPriority = 0x8C,
                   kThreadUniqueId = 0x12C;
constexpr uint8_t kDispatchLevel = 2;
// The quantum: 20 ms, the Xbox kernel's (60 quantum units, 3 a clock tick
// of 1 ms).
constexpr auto kQuantum = std::chrono::milliseconds(20);

struct ThreadExit {};  // unwinds a terminated thread's host stack

struct Thread {
    uint32_t kthread = 0, id = 0;
    int priority = 8, increment = 0;
    bool running = false, terminated = false, ready = false;
    int suspend = 0;
    std::condition_variable cv;
    // A wait: the objects, any or all, until when; the status it ends with.
    bool waiting = false, wait_all = false;
    std::vector<uint32_t> objects;
    Clock::time_point deadline = Clock::time_point::max();
    uint32_t result = 0;
    // The NT_TIB fields the processor's KPCR holds for the running thread.
    uint32_t exception_list = 0xFFFFFFFFu, stack_base = 0, stack_limit = 0;
    uint8_t irql = 0;  // the KPCR's Irql while it runs (switches happen below DISPATCH_LEVEL)
    rt::Context ctx{};  // a created thread's registers (the main thread's are the runtime's)
};

struct State {
    std::mutex mu;
    std::condition_variable clock_cv;
    std::vector<std::unique_ptr<Thread>> threads;
    Thread* current = nullptr;   // holding the processor
    bool dpc_running = false;    // the clock thread runs DPCs on an idle processor
    std::deque<Thread*> ready[32];
    std::vector<Thread*> waiting;
    std::deque<uint32_t> dpcs;   // queued KDPCs
    struct Timer {
        uint64_t due;            // interrupt time
        uint32_t period, dpc;    // ms
    };
    std::map<uint32_t, Timer> timers;
    Clock::time_point quantum_end;
    rt::Context* main = nullptr;
    GuestCall call = nullptr;
    rt::Context dpc_ctx{}, start_ctx{};  // DPCs' registers; a new thread's starting ones
    uint32_t tick_count = 0, thread_type = 0, event_type = 0, mutant_type = 0, semaphore_type = 0, timer_type = 0,
             file_type = 0;
    uint32_t next_id = 1;
    std::thread::id main_host;
    Clock::time_point start = Clock::now();
    // Kernel objects by handle: the object's guest address.
    std::map<uint32_t, uint32_t> handles;
    uint32_t next_handle = 0x40000;
    // The kernel's pool: chunks of contiguous memory, first fit.
    std::map<uint32_t, uint32_t> pool_free, pool_used;  // address → bytes
};
State& s() {
    static State* st = new State;  // never destroyed: threads may still wait on it at exit
    return *st;
}
thread_local Thread* t_self = nullptr;

rt::Context& mc() { return *s().main; }
uint32_t kpcr() { return memory_map().kpcr(); }
uint8_t irql() { return rt::ld8(mem(mc()), kpcr() + 0x24, 0); }

// ------------------------------------------------------------ the processor
Thread* best_ready() {
    for (int p = 31; p >= 0; --p)
        if (!s().ready[p].empty()) return s().ready[p].front();
    return nullptr;
}

// Gives the processor to `t` (ready): the KPCR's thread fields become its.
void run(Thread* t) {
    auto& q = s().ready[t->priority];
    q.erase(std::find(q.begin(), q.end(), t));
    t->ready = false;
    s().current = t;
    t->running = true;
    const rt::Context& c = mc();
    st(c, kpcr() + 0x00, t->exception_list);
    st(c, kpcr() + 0x04, t->stack_base);
    st(c, kpcr() + 0x08, t->stack_limit);
    st(c, kpcr() + 0x28, t->kthread);
    rt::st8(mem(c), kpcr() + 0x24, t->irql, 0);
    s().quantum_end = Clock::now() + kQuantum;
    t->cv.notify_one();
}

void dispatch() {
    if (s().dpc_running) return;
    if (Thread* n = best_ready()) run(n);
    else s().current = nullptr;
}

void make_ready(Thread* t) {
    if (t->terminated || t->ready || t->running || t->suspend) return;
    s().ready[t->priority].push_back(t);
    t->ready = true;
    if (!s().current) dispatch();
    else if (t->priority > s().current->priority) rt::g_preempt.store(true, std::memory_order_relaxed);
    s().clock_cv.notify_one();
}

// The running thread gives up the processor; returns when it has it again.
void switch_out(Thread* me, std::unique_lock<std::mutex>& lk) {
    me->exception_list = ld(mc(), kpcr());
    me->irql = irql();
    me->running = false;
    if (s().current == me) {
        s().current = nullptr;
        dispatch();
    }
    s().clock_cv.notify_one();
    me->cv.wait(lk, [&] { return me->running; });
    set_current(&(me == s().threads.front().get() ? mc() : me->ctx));
}

// ------------------------------------------------------------ objects
uint8_t type_of(uint32_t o) { return rt::ld8(mem(mc()), o, 0); }
int32_t state_of(uint32_t o) { return int32_t(ld(mc(), o + 4)); }
void set_state(uint32_t o, int32_t v) { st(mc(), o + 4, uint32_t(v)); }

bool signalled(uint32_t o, const Thread* t) {
    if (type_of(o) == kMutant) return state_of(o) > 0 || ld(mc(), o + kMutantOwner) == t->kthread;
    return state_of(o) > 0;
}
// What a satisfied wait does to the object: abandoned mutants report it.
uint32_t acquire(uint32_t o, const Thread* t) {
    switch (type_of(o)) {
        case kSynchronizationEvent: case kSynchronizationTimer: set_state(o, 0); break;
        case kSemaphore: set_state(o, state_of(o) - 1); break;
        case kMutant: {
            set_state(o, state_of(o) - 1);
            st(mc(), o + kMutantOwner, t->kthread);
            if (rt::ld8(mem(mc()), o + 0x1C, 0)) {  // Abandoned
                rt::st8(mem(mc()), o + 0x1C, 0, 0);
                return kStatusAbandoned;
            }
            break;
        }
        default: break;  // notification events and timers, threads: stay signalled
    }
    return 0;
}
// A wait is satisfied: any, the first signalled (STATUS_WAIT_0 + index); all, every one.
std::optional<uint32_t> try_satisfy(Thread* t) {
    if (t->wait_all) {
        for (uint32_t o : t->objects)
            if (!signalled(o, t)) return std::nullopt;
        uint32_t extra = 0;
        for (uint32_t o : t->objects) extra |= acquire(o, t);
        return t->objects.empty() ? std::nullopt : std::optional<uint32_t>(extra);
    }
    for (size_t i = 0; i < t->objects.size(); ++i)
        if (signalled(t->objects[i], t)) return acquire(t->objects[i], t) + uint32_t(i);
    return std::nullopt;
}
// Something was signalled: waits it satisfies end, in the order they began.
void signalled_changed() {
    for (bool again = true; again;) {
        again = false;
        for (auto it = s().waiting.begin(); it != s().waiting.end(); ++it) {
            Thread* t = *it;
            if (auto r = try_satisfy(t)) {
                t->result = *r;
                t->waiting = false;
                s().waiting.erase(it);
                make_ready(t);
                again = true;
                break;
            }
        }
    }
}

// The calling guest thread waits (KeWaitForMultipleObjects's semantics).
uint32_t wait(std::vector<uint32_t> objects, bool all, std::optional<Clock::time_point> until) {
    Thread* me = t_self;
    if (!me) rt::trap(0, "a kernel wait outside a guest thread");
    std::unique_lock lk(s().mu);
    me->objects = std::move(objects);
    me->wait_all = all;
    if (auto r = try_satisfy(me)) return *r;
    if (until && *until <= Clock::now()) return kStatusTimeout;
    if (irql() >= kDispatchLevel) rt::trap(0, "a kernel wait at DISPATCH_LEVEL or above (a bug check on the Xbox)");
    me->waiting = true;
    me->deadline = until.value_or(Clock::time_point::max());
    s().waiting.push_back(me);
    switch_out(me, lk);
    return me->result;
}

// LARGE_INTEGER* timeout: null forever, negative relative, else an absolute system time.
std::optional<Clock::time_point> timeout_at(const rt::Context& c, uint32_t p) {
    if (!p) return std::nullopt;
    const int64_t v = int64_t(uint64_t(ld(c, p + 4)) << 32 | ld(c, p));
    if (v < 0) return Clock::now() + std::chrono::nanoseconds(uint64_t(-v) * 100);
    // Absolute: system time (100 ns since 1601), against the host's clock.
    const int64_t now_sys = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count() / 100 +
                            116444736000000000LL;
    return Clock::now() + std::chrono::nanoseconds(std::max<int64_t>(0, v - now_sys) * 100);
}

// ------------------------------------------------------------ DPCs
// KDPC: Type, Inserted (+2), DpcListEntry, DeferredRoutine (+0xC),
// DeferredContext (+0x10), SystemArgument1 (+0x14), SystemArgument2 (+0x18).
// They run at DISPATCH_LEVEL, on their own registers and stack: the
// interrupted thread's registers live in its lifted code's locals.
void run_dpcs(std::unique_lock<std::mutex>& lk) {
    rt::Context& d = s().dpc_ctx;
    while (!s().dpcs.empty()) {
        const uint32_t dpc = s().dpcs.front();
        s().dpcs.pop_front();
        rt::st8(mem(mc()), dpc + 2, 0, 0);
        const uint32_t routine = ld(mc(), dpc + 0xC), context = ld(mc(), dpc + 0x10), a1 = ld(mc(), dpc + 0x14),
                       a2 = ld(mc(), dpc + 0x18);
        const uint8_t old = irql();
        rt::st8(mem(mc()), kpcr() + 0x24, kDispatchLevel, 0);
        lk.unlock();
        rt::Context* prev = current_context();
        set_current(&d);
        s().call(d, routine, {dpc, context, a1, a2});
        set_current(prev);
        lk.lock();
        rt::st8(mem(mc()), kpcr() + 0x24, old, 0);
    }
}

// ------------------------------------------------------------ the clock
void fire_timer(uint32_t t, State::Timer tm) {
    set_state(t, 1);
    signalled_changed();
    if (tm.dpc && !rt::ld8(mem(mc()), tm.dpc + 2, 0)) {
        const uint64_t sys = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count() / 100 +
                             116444736000000000ULL;
        rt::st8(mem(mc()), tm.dpc + 2, 1, 0);
        st(mc(), tm.dpc + 0x14, uint32_t(sys));
        st(mc(), tm.dpc + 0x18, uint32_t(sys >> 32));
        s().dpcs.push_back(tm.dpc);
        rt::g_preempt.store(true, std::memory_order_relaxed);
    }
}

void clock_thread() {
    std::unique_lock lk(s().mu);
    auto next_ms = Clock::now();
    for (;;) {
        Clock::time_point next = next_ms + std::chrono::milliseconds(1);
        for (Thread* t : s().waiting) next = std::min(next, t->deadline);
        s().clock_cv.wait_until(lk, next);
        const auto now = Clock::now();
        // KeTickCount: milliseconds since the runtime started.
        if (now >= next_ms + std::chrono::milliseconds(1)) {
            next_ms = now;
            if (s().tick_count)
                st(mc(), s().tick_count, uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(now - s().start).count()));
        }
        // Waits whose time is up.
        for (auto it = s().waiting.begin(); it != s().waiting.end();) {
            Thread* t = *it;
            if (t->deadline <= now) {
                t->result = kStatusTimeout;
                t->waiting = false;
                it = s().waiting.erase(it);
                make_ready(t);
            } else {
                ++it;
            }
        }
        // Timers that are due.
        const uint64_t it_now = interrupt_time();
        for (auto it = s().timers.begin(); it != s().timers.end();) {
            if (it->second.due > it_now) { ++it; continue; }
            const uint32_t t = it->first;
            State::Timer tm = it->second;
            if (tm.period) {
                it->second.due += uint64_t(tm.period) * 10000;
                ++it;
            } else {
                rt::st8(mem(mc()), t + 3, 0, 0);  // no longer inserted
                it = s().timers.erase(it);
            }
            fire_timer(t, tm);
        }
        // The quantum: another thread of the running one's priority goes next.
        if (Thread* cur = s().current; cur && now >= s().quantum_end) {
            if (Thread* r = best_ready(); r && r->priority >= cur->priority) rt::g_preempt.store(true, std::memory_order_relaxed);
            else s().quantum_end = now + kQuantum;
        }
        // An idle processor runs the DPCs, then whoever is ready.
        if (!s().current && !s().dpcs.empty() && !s().dpc_running) {
            s().dpc_running = true;
            run_dpcs(lk);
            s().dpc_running = false;
            dispatch();
        }
        if (!s().current) dispatch();
    }
}

// ------------------------------------------------------------ threads
Thread* thread_of(uint32_t kthread) {
    for (auto& t : s().threads)
        if (t->kthread == kthread) return t.get();
    return nullptr;
}

void terminate(Thread* me, std::unique_lock<std::mutex>& lk) {
    me->terminated = true;
    set_state(me->kthread, 1);  // a thread object is signalled when it ends
    signalled_changed();
    me->running = false;
    if (s().current == me) {
        s().current = nullptr;
        dispatch();
    }
    (void)lk;
}

uint32_t pool_alloc(uint32_t bytes);

// The object a handle names: kernel objects here, -2 the current thread.
uint32_t object_of(uint32_t h) {
    if (h == 0xFFFFFFFEu) return t_self ? t_self->kthread : 0;
    auto it = s().handles.find(h);
    return it == s().handles.end() ? 0 : it->second;
}
uint32_t new_handle(uint32_t object) {
    std::lock_guard lk(s().mu);
    const uint32_t h = s().next_handle;
    s().next_handle += 4;
    s().handles[h] = object;
    return h;
}

// ------------------------------------------------------------ the pool
uint32_t pool_alloc(uint32_t bytes) {
    bytes = (std::max(bytes, 1u) + 15) & ~15u;
    for (auto it = s().pool_free.begin(); it != s().pool_free.end(); ++it) {
        if (it->second < bytes) continue;
        const uint32_t a = it->first, size = it->second;
        s().pool_free.erase(it);
        if (size > bytes) s().pool_free[a + bytes] = size - bytes;
        s().pool_used[a] = bytes;
        std::memset(mc().mem + a, 0, bytes);
        return a;
    }
    const uint32_t chunk = std::max<uint32_t>(0x10000, (bytes + 0xFFFF) & ~0xFFFFu);
    const uint32_t a = contiguous(chunk, 0, 0xFFFFFFFFu, 16, 4);
    if (!a) return 0;
    s().pool_free[a] = chunk;
    return pool_alloc(bytes);
}
void pool_free(uint32_t a) {
    auto it = s().pool_used.find(a);
    if (it == s().pool_used.end()) rt::trap(0, "ExFreePool: not a pool block");
    s().pool_free[a] = it->second;
    s().pool_used.erase(it);
    // Join neighbours.
    auto f = s().pool_free.find(a);
    if (auto n = std::next(f); n != s().pool_free.end() && f->first + f->second == n->first) {
        f->second += n->second;
        s().pool_free.erase(n);
    }
    if (f != s().pool_free.begin())
        if (auto p = std::prev(f); p->first + p->second == f->first) {
            p->second += f->second;
            s().pool_free.erase(f);
        }
}

void init_header(uint32_t o, uint8_t type, uint8_t size_dwords, int32_t state) {
    const rt::Mem m = mem(mc());
    rt::st8(m, o, type, 0);
    rt::st8(m, o + 1, 0, 0);
    rt::st8(m, o + 2, size_dwords, 0);
    rt::st8(m, o + 3, 0, 0);
    set_state(o, state);
    st(mc(), o + 8, o + 8);
    st(mc(), o + 12, o + 8);
}

int priority_for(int increment) {  // a base priority from the increment over the normal 8, saturating
    if (increment >= 16) return 15;
    if (increment <= -16) return 1;
    return std::clamp(8 + increment, 1, 15);
}

[[noreturn]] void unsupported(uint32_t ordinal, const std::string& what) {
    static std::string why;
    why = what;
    rt::trap(kernel_addr(ordinal), why.c_str());
}

// ------------------------------------------------------------ kernel functions
// NTSTATUS PsCreateSystemThreadEx(PHANDLE ThreadHandle, ULONG ThreadExtensionSize,
//     ULONG KernelStackSize, ULONG TlsDataSize, PHANDLE ThreadId, PKSTART_ROUTINE
//     StartRoutine, PVOID StartContext, BOOLEAN CreateSuspended, BOOLEAN
//     DebuggerThread, PKSYSTEM_ROUTINE SystemRoutine) (255): the ETHREAD in
// the pool, a stack in contiguous memory with the TLS block at its top
// (zeroed: the start routine fills it), then SystemRoutine(StartRoutine,
// StartContext) (XAPI's thread start) or StartRoutine(StartContext); when
// it returns, the thread ends.
void ps_create_system_thread_ex(rt::Context& c) {
    const uint32_t phandle = arg(c, 0), ext = arg(c, 1), kstack = arg(c, 2), tls = arg(c, 3), pid = arg(c, 4),
                   start = arg(c, 5), context = arg(c, 6), suspended = arg(c, 7) & 0xFF, system = arg(c, 9);
    const uint32_t stack = std::max<uint32_t>((kstack + 0xFFF) & ~0xFFFu, 0x4000) + ((tls + 0xFFF) & ~0xFFFu);
    uint32_t ethread, base;
    auto t = std::make_unique<Thread>();
    {
        std::lock_guard lk(s().mu);
        ethread = pool_alloc(kKThreadSize + ext);
        base = ethread ? contiguous(stack, 0, 0xFFFFFFFFu, 0x1000, 4) : 0;
        if (!base) {
            if (ethread) pool_free(ethread);
            ret(c, kStatusNoMemory, 40);
            return;
        }
        std::memset(c.mem + base, 0, stack);
        t->id = s().next_id++;
    }
    const uint32_t top = base + stack;
    init_header(ethread, kThreadObject, uint8_t(kKThreadSize / 4), 0);
    st(c, ethread + kThreadStackBase, top);
    st(c, ethread + kThreadStackLimit, base);
    st(c, ethread + kThreadTls, tls ? top - tls : 0);
    rt::st8(mem(c), ethread + kThreadPriority, 8, 0);
    st(c, ethread + kThreadUniqueId, t->id);
    t->kthread = ethread;
    t->stack_base = top;
    t->stack_limit = base;
    t->suspend = suspended ? 1 : 0;
    // Registers: the main thread's starting state (memory, x87 control),
    // the stack below the TLS block, the start routine's arguments.
    t->ctx = s().start_ctx;
    t->ctx.esp = (top - tls) & ~15u;
    Thread* tp = t.get();
    const uint32_t routine = system ? system : start;
    std::thread([tp, routine, system, start, context] {
        t_self = tp;
        {
            std::unique_lock lk(s().mu);
            tp->cv.wait(lk, [&] { return tp->running; });
        }
        set_current(&tp->ctx);
        try {
            if (system) s().call(tp->ctx, routine, {start, context});
            else s().call(tp->ctx, routine, {context});
        } catch (const ThreadExit&) {
        }
        std::unique_lock lk(s().mu);
        if (!tp->terminated) terminate(tp, lk);
    }).detach();
    {
        std::lock_guard lk(s().mu);
        s().threads.push_back(std::move(t));
        if (!tp->suspend) make_ready(tp);
    }
    std::printf("qilin_run: thread %u started (%08X, priority %d)\n", tp->id, system ? start : routine, tp->priority);
    std::fflush(stdout);
    st(c, phandle, new_handle(ethread));
    if (pid) st(c, pid, tp->id);
    ret(c, kStatusSuccess, 40);
}

// VOID PsTerminateSystemThread(NTSTATUS ExitStatus) (258)
void ps_terminate_system_thread(rt::Context& c) {
    (void)c;
    Thread* me = t_self;
    if (!me || me == s().threads.front().get()) rt::trap(kernel_addr(258), "PsTerminateSystemThread on the main thread");
    {
        std::unique_lock lk(s().mu);
        terminate(me, lk);
    }
    throw ThreadExit{};
}

// NTSTATUS NtCreateEvent(PHANDLE, POBJECT_ATTRIBUTES, EVENT_TYPE, BOOLEAN InitialState) (189)
void nt_create_event(rt::Context& c) {
    const uint32_t ph = arg(c, 0), oa = arg(c, 1), type = arg(c, 2), initial = arg(c, 3) & 0xFF;
    if (oa && ld(c, oa + 4)) unsupported(189, "NtCreateEvent: a named event");
    uint32_t o;
    {
        std::lock_guard lk(s().mu);
        o = pool_alloc(16);
    }
    init_header(o, uint8_t(type ? kSynchronizationEvent : kNotificationEvent), 4, initial ? 1 : 0);
    st(c, ph, new_handle(o));
    ret(c, kStatusSuccess, 16);
}

// NTSTATUS NtCreateMutant(PHANDLE, POBJECT_ATTRIBUTES, BOOLEAN InitialOwner) (192)
void nt_create_mutant(rt::Context& c) {
    const uint32_t ph = arg(c, 0), oa = arg(c, 1), owner = arg(c, 2) & 0xFF;
    if (oa && ld(c, oa + 4)) unsupported(192, "NtCreateMutant: a named mutant");
    uint32_t o;
    {
        std::lock_guard lk(s().mu);
        o = pool_alloc(0x20);
    }
    init_header(o, kMutant, 8, owner ? 0 : 1);
    st(c, o + kMutantOwner, owner && t_self ? t_self->kthread : 0);
    st(c, ph, new_handle(o));
    ret(c, kStatusSuccess, 12);
}

// Releases a mutant the caller owns; returns the previous count.
uint32_t release_mutant(uint32_t o, int32_t* prev) {
    std::lock_guard lk(s().mu);
    if (ld(mc(), o + kMutantOwner) != (t_self ? t_self->kthread : 0)) return kStatusMutantNotOwned;
    *prev = state_of(o);
    set_state(o, *prev + 1);
    if (*prev + 1 == 1) {
        st(mc(), o + kMutantOwner, 0);
        signalled_changed();
    }
    return kStatusSuccess;
}
// NTSTATUS NtReleaseMutant(HANDLE, PLONG PreviousCount) (206)
void nt_release_mutant(rt::Context& c) {
    const uint32_t o = object_of(arg(c, 0)), pp = arg(c, 1);
    if (!o) return ret(c, kStatusInvalidHandle, 8);
    int32_t prev = 0;
    const uint32_t r = release_mutant(o, &prev);
    if (pp && !r) st(c, pp, uint32_t(prev));
    ret(c, r, 8);
}

// LONG KeSetEvent(PRKEVENT, KPRIORITY Increment, BOOLEAN Wait) (145)
int32_t set_event_locked(uint32_t o) {
    const int32_t prev = state_of(o);
    set_state(o, 1);
    signalled_changed();
    return prev;
}
void ke_set_event(rt::Context& c) {
    std::lock_guard lk(s().mu);
    ret(c, uint32_t(set_event_locked(arg(c, 0))), 12);
}
// NTSTATUS NtSetEvent(HANDLE, PLONG PreviousState) (225)
void nt_set_event(rt::Context& c) {
    const uint32_t o = object_of(arg(c, 0)), pp = arg(c, 1);
    if (!o) return ret(c, kStatusInvalidHandle, 8);
    std::lock_guard lk(s().mu);
    const int32_t prev = set_event_locked(o);
    if (pp) st(c, pp, uint32_t(prev));
    ret(c, kStatusSuccess, 8);
}

// NTSTATUS KeWaitForSingleObject(PVOID Object, KWAIT_REASON, KPROCESSOR_MODE,
//     BOOLEAN Alertable, PLARGE_INTEGER Timeout) (159). Nothing queues APCs
// (I/O completes at once), so an alertable wait is an ordinary one.
void ke_wait_for_single_object(rt::Context& c) {
    const uint32_t o = arg(c, 0), timeout = arg(c, 4);
    ret(c, wait({o}, false, timeout_at(c, timeout)), 20);
}
// NTSTATUS NtWaitForSingleObjectEx(HANDLE, KPROCESSOR_MODE, BOOLEAN Alertable, PLARGE_INTEGER) (235)
void nt_wait_for_single_object_ex(rt::Context& c) {
    const uint32_t o = object_of(arg(c, 0));
    if (!o) return ret(c, kStatusInvalidHandle, 16);
    ret(c, wait({o}, false, timeout_at(c, arg(c, 3))), 16);
}
// NTSTATUS NtWaitForSingleObject(HANDLE, BOOLEAN Alertable, PLARGE_INTEGER) (234)
void nt_wait_for_single_object(rt::Context& c) {
    const uint32_t o = object_of(arg(c, 0));
    if (!o) return ret(c, kStatusInvalidHandle, 12);
    ret(c, wait({o}, false, timeout_at(c, arg(c, 2))), 12);
}
// NTSTATUS NtWaitForMultipleObjectsEx(ULONG Count, HANDLE[], WAIT_TYPE (0 all,
//     1 any), KPROCESSOR_MODE, BOOLEAN Alertable, PLARGE_INTEGER) (233)
void nt_wait_for_multiple_objects_ex(rt::Context& c) {
    const uint32_t n = arg(c, 0), hs = arg(c, 1), type = arg(c, 2);
    if (n == 0 || n > 64) return ret(c, kStatusInvalidParameter, 24);
    std::vector<uint32_t> objs;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t o = object_of(ld(c, hs + 4 * i));
        if (!o) return ret(c, kStatusInvalidHandle, 24);
        objs.push_back(o);
    }
    ret(c, wait(std::move(objs), type == 0, timeout_at(c, arg(c, 5))), 24);
}

// NTSTATUS KeDelayExecutionThread(KPROCESSOR_MODE, BOOLEAN Alertable, PLARGE_INTEGER Interval) (99)
uint32_t yield();
void ke_delay_execution_thread(rt::Context& c) {
    const auto until = timeout_at(c, arg(c, 2));
    if (!until || *until <= Clock::now()) {
        yield();
    } else {
        wait({}, false, until);
    }
    ret(c, kStatusSuccess, 12);
}

// Another ready thread of the caller's priority or above goes first.
uint32_t yield() {
    Thread* me = t_self;
    std::unique_lock lk(s().mu);
    Thread* r = best_ready();
    if (!me || !r || r->priority < me->priority) return kStatusNoYield;
    me->running = false;
    s().current = nullptr;
    s().ready[me->priority].push_back(me);
    me->ready = true;
    me->exception_list = ld(mc(), kpcr());
    me->irql = irql();
    dispatch();
    s().clock_cv.notify_one();
    me->cv.wait(lk, [&] { return me->running; });
    set_current(&(me == s().threads.front().get() ? mc() : me->ctx));
    return kStatusSuccess;
}
// NTSTATUS NtYieldExecution(VOID) (238)
void nt_yield_execution(rt::Context& c) { ret(c, yield(), 0); }

// LONG KeSetBasePriorityThread(PKTHREAD, LONG Increment) (143): the old increment.
void ke_set_base_priority_thread(rt::Context& c) {
    const uint32_t kt = arg(c, 0);
    const int32_t inc = int32_t(arg(c, 1));
    std::lock_guard lk(s().mu);
    Thread* t = thread_of(kt);
    if (!t) rt::trap(kernel_addr(143), "KeSetBasePriorityThread: not a thread the runtime made");
    const int old = t->increment;
    const int p = priority_for(inc);
    t->increment = inc;
    if (p != t->priority) {
        if (t->ready) {
            auto& q = s().ready[t->priority];
            q.erase(std::find(q.begin(), q.end(), t));
            s().ready[p].push_back(t);
        }
        t->priority = p;
        rt::st8(mem(mc()), kt + kThreadPriority, uint8_t(p), 0);
        if (Thread* cur = s().current; cur && best_ready() && best_ready()->priority > cur->priority)
            rt::g_preempt.store(true, std::memory_order_relaxed);
    }
    ret(c, uint32_t(old), 8);
}

// NTSTATUS NtSuspendThread(HANDLE, PULONG PreviousSuspendCount) (231),
// NtResumeThread (219).
void nt_suspend_thread(rt::Context& c) {
    const uint32_t o = object_of(arg(c, 0)), pp = arg(c, 1);
    std::unique_lock lk(s().mu);
    Thread* t = thread_of(o);
    if (!t) return ret(c, kStatusInvalidHandle, 8);
    if (t->suspend >= 127) return ret(c, kStatusSuspendCountExceeded, 8);
    if (pp) st(c, pp, uint32_t(t->suspend));
    ++t->suspend;
    if (t->ready) {
        auto& q = s().ready[t->priority];
        q.erase(std::find(q.begin(), q.end(), t));
        t->ready = false;
    }
    if (t == t_self) {
        ret(c, kStatusSuccess, 8);
        switch_out(t, lk);
        return;
    }
    ret(c, kStatusSuccess, 8);
}
void nt_resume_thread(rt::Context& c) {
    const uint32_t o = object_of(arg(c, 0)), pp = arg(c, 1);
    std::lock_guard lk(s().mu);
    Thread* t = thread_of(o);
    if (!t) return ret(c, kStatusInvalidHandle, 8);
    if (pp) st(c, pp, uint32_t(t->suspend));
    if (t->suspend && --t->suspend == 0 && !t->waiting && !t->terminated && !t->running) make_ready(t);
    ret(c, kStatusSuccess, 8);
}

// NTSTATUS ObReferenceObjectByHandle(HANDLE, POBJECT_TYPE, PVOID* Object) (246)
void ob_reference_object_by_handle(rt::Context& c) {
    const uint32_t h = arg(c, 0), type = arg(c, 1), out = arg(c, 2);
    const uint32_t o = object_of(h);
    if (!o) return ret(c, kStatusInvalidHandle, 12);
    if (type) {
        const uint8_t k = type_of(o);
        const uint32_t want = k == kThreadObject ? s().thread_type
                              : k <= kSynchronizationEvent ? s().event_type
                              : k == kMutant ? s().mutant_type
                              : k == kSemaphore ? s().semaphore_type
                                                : s().timer_type;
        if (type != want) return ret(c, kStatusTypeMismatch, 12);
    }
    st(c, out, o);
    ret(c, kStatusSuccess, 12);
}
// VOID FASTCALL ObfDereferenceObject(PVOID) (250): objects aren't freed yet.
void obf_dereference_object(rt::Context& c) { ret(c, c.eax, 0); }

// BOOLEAN KeInsertQueueDpc(PRKDPC, PVOID SystemArgument1, PVOID SystemArgument2) (119)
void ke_insert_queue_dpc(rt::Context& c) {
    const uint32_t dpc = arg(c, 0);
    std::lock_guard lk(s().mu);
    if (rt::ld8(mem(c), dpc + 2, 0)) return ret(c, 0, 12);
    rt::st8(mem(c), dpc + 2, 1, 0);
    st(c, dpc + 0x14, arg(c, 1));
    st(c, dpc + 0x18, arg(c, 2));
    s().dpcs.push_back(dpc);
    rt::g_preempt.store(true, std::memory_order_relaxed);
    s().clock_cv.notify_one();
    ret(c, 1, 12);
}
// BOOLEAN KeRemoveQueueDpc(PRKDPC) (137)
void ke_remove_queue_dpc(rt::Context& c) {
    const uint32_t dpc = arg(c, 0);
    std::lock_guard lk(s().mu);
    auto it = std::find(s().dpcs.begin(), s().dpcs.end(), dpc);
    if (it == s().dpcs.end()) return ret(c, 0, 4);
    s().dpcs.erase(it);
    rt::st8(mem(c), dpc + 2, 0, 0);
    ret(c, 1, 4);
}

// PVOID ExAllocatePoolWithTag(SIZE_T, ULONG Tag) (15), VOID ExFreePool(PVOID) (17),
// ULONG ExQueryPoolBlockSize(PVOID) (23)
void ex_allocate_pool_with_tag(rt::Context& c) {
    std::lock_guard lk(s().mu);
    ret(c, pool_alloc(arg(c, 0)), 8);
}
void ex_free_pool(rt::Context& c) {
    std::lock_guard lk(s().mu);
    pool_free(arg(c, 0));
    ret(c, c.eax, 4);
}
void ex_query_pool_block_size(rt::Context& c) {
    std::lock_guard lk(s().mu);
    auto it = s().pool_used.find(arg(c, 0));
    ret(c, it == s().pool_used.end() ? 0 : it->second, 4);
}

// NTSTATUS KeSaveFloatingPointState(PKFLOATING_SAVE) (139) and Restore (138):
// each host thread keeps its own x87 state.
void ke_save_fp(rt::Context& c) { ret(c, kStatusSuccess, 4); }
// VOID KeStallExecutionProcessor(ULONG MicroSeconds) (151): busy, no switch.
void ke_stall_execution_processor(rt::Context& c) {
    const auto until = Clock::now() + std::chrono::microseconds(arg(c, 0));
    while (Clock::now() < until) {
    }
    ret(c, c.eax, 4);
}
// VOID KeBugCheck(ULONG) (95)
void ke_bug_check(rt::Context& c) {
    unsupported(95, strf("KeBugCheck %08X", arg(c, 0)));
}

struct Fn {
    const char* name;
    uint16_t arg_bytes;
    HleFn fn;
};
constexpr Fn kFns[] = {
    {"PsCreateSystemThreadEx", 40, ps_create_system_thread_ex},
    {"PsTerminateSystemThread", 4, ps_terminate_system_thread},
    {"NtCreateEvent", 16, nt_create_event},
    {"NtCreateMutant", 12, nt_create_mutant},
    {"NtReleaseMutant", 8, nt_release_mutant},
    {"NtSetEvent", 8, nt_set_event},
    {"KeSetEvent", 12, ke_set_event},
    {"KeWaitForSingleObject", 20, ke_wait_for_single_object},
    {"NtWaitForSingleObject", 12, nt_wait_for_single_object},
    {"NtWaitForSingleObjectEx", 16, nt_wait_for_single_object_ex},
    {"NtWaitForMultipleObjectsEx", 24, nt_wait_for_multiple_objects_ex},
    {"KeDelayExecutionThread", 12, ke_delay_execution_thread},
    {"NtYieldExecution", 0, nt_yield_execution},
    {"KeSetBasePriorityThread", 8, ke_set_base_priority_thread},
    {"NtSuspendThread", 8, nt_suspend_thread},
    {"NtResumeThread", 8, nt_resume_thread},
    {"ObReferenceObjectByHandle", 12, ob_reference_object_by_handle},
    {"ObfDereferenceObject", 4, obf_dereference_object},  // fastcall: ecx, nothing popped,
    {"KeInsertQueueDpc", 12, ke_insert_queue_dpc},
    {"KeRemoveQueueDpc", 4, ke_remove_queue_dpc},
    {"ExAllocatePoolWithTag", 8, ex_allocate_pool_with_tag},
    {"ExFreePool", 4, ex_free_pool},
    {"ExQueryPoolBlockSize", 4, ex_query_pool_block_size},
    {"KeSaveFloatingPointState", 4, ke_save_fp},
    {"KeRestoreFloatingPointState", 4, ke_save_fp},
    {"KeStallExecutionProcessor", 4, ke_stall_execution_processor},
    {"KeBugCheck", 4, ke_bug_check},
};

}  // namespace

// ------------------------------------------------------------ the API
void init_main(rt::Context& c, uint32_t kthread, GuestCall call) {
    State& st_ = s();
    st_.main = &c;
    st_.call = call;
    st_.main_host = std::this_thread::get_id();
    auto t = std::make_unique<Thread>();
    t->kthread = kthread;
    t->id = st_.next_id++;
    t->running = true;
    t->stack_base = ld(c, kpcr() + 4);
    t->stack_limit = ld(c, kpcr() + 8);
    st(c, kthread + kThreadUniqueId, t->id);
    rt::st8(mem(c), kthread + kThreadPriority, 8, 0);
    init_header(kthread, kThreadObject, uint8_t(kKThreadSize / 4), 0);
    t_self = t.get();
    st_.current = t.get();
    st_.quantum_end = Clock::now() + kQuantum;
    // DPCs' registers and a 64 KB stack of their own; new threads start from the same state.
    st_.dpc_ctx = c;
    st_.start_ctx = c;
    const uint32_t dpc_stack = contiguous(0x10000, 0, 0xFFFFFFFFu, 0x1000, 4);
    st_.dpc_ctx.esp = dpc_stack + 0x10000 - 16;
    st_.threads.push_back(std::move(t));
    std::thread(clock_thread).detach();
}

bool started() { return s().main != nullptr; }
bool on_main_thread() { return !started() || std::this_thread::get_id() == s().main_host; }

void sleep_until(Clock::time_point t) {
    if (!t_self || !started()) {
        std::this_thread::sleep_until(t);
        return;
    }
    wait({}, false, t);
}

void set_event(uint32_t event) {
    std::lock_guard lk(s().mu);
    set_event_locked(event);
}

void wait_object(uint32_t object) { wait({object}, false, std::nullopt); }

uint64_t interrupt_time() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - s().start).count() / 100);
}

void set_data_export(uint32_t ordinal, uint32_t addr) {
    switch (ordinal) {
        case 156: s().tick_count = addr; break;
        case 259: s().thread_type = addr; break;
        case 16: s().event_type = addr; break;
        case 22: s().mutant_type = addr; break;
        case 30: s().semaphore_type = addr; break;
        case 31: s().timer_type = addr; break;
        case 71: s().file_type = addr; break;
        default: break;
    }
}

uint32_t set_timer(rt::Context& c, uint32_t t, int64_t due, uint32_t period, uint32_t dpc) {
    const uint64_t now = interrupt_time();
    const uint64_t at = due < 0 ? now + uint64_t(-due) : uint64_t(due);  // absolute: as the interrupt clock's
    std::lock_guard lk(s().mu);
    const bool was = s().timers.erase(t) != 0;
    s().timers[t] = State::Timer{at, period, dpc};
    rt::st8(mem(c), t + 3, 1, 0);  // Header.Inserted
    set_state(t, 0);
    st(c, t + 0x10, uint32_t(at));
    st(c, t + 0x14, uint32_t(at >> 32));
    st(c, t + 0x20, dpc);
    st(c, t + 0x24, period);
    s().clock_cv.notify_one();
    return was ? 1 : 0;
}

bool cancel_timer(rt::Context& c, uint32_t t) {
    std::lock_guard lk(s().mu);
    const bool was = s().timers.erase(t) != 0;
    rt::st8(mem(c), t + 3, 0, 0);
    return was;
}

bool close_handle(uint32_t h) {
    std::lock_guard lk(s().mu);
    return s().handles.erase(h) != 0;
}

void register_kernel_sched() {
    for (const Fn& f : kFns) {
        const KernelExport* e = kernel_export(f.name);
        if (!e || e->arg_bytes != f.arg_bytes) {
            std::fprintf(stderr, "qilin: kernel export %s: argument size mismatch\n", f.name);
            std::exit(3);
        }
        register_hle(kernel_addr(e->ordinal), e->name, f.fn);
    }
}

}  // namespace qilin::host::sched

// The preemption point lifted code reaches at its backward branches (and
// QILIN_TICK's callers): DPCs first, then a higher-priority thread, or one
// of the same priority when the quantum is over. Nothing switches at
// DISPATCH_LEVEL and above; the flag stays set until the level drops.
namespace qilin::rt {
using namespace qilin::host::sched;
void preempt() {
    auto& S = s();
    Thread* me = t_self;
    if (!me || !S.main) return;
    std::unique_lock lk(S.mu);
    if (irql() >= kDispatchLevel) return;
    g_preempt.store(false, std::memory_order_relaxed);
    if (!S.dpcs.empty() && !S.dpc_running) {
        S.dpc_running = true;
        run_dpcs(lk);
        S.dpc_running = false;
    }
    Thread* r = best_ready();
    if (!r || S.current != me) return;
    const bool higher = r->priority > me->priority;
    const bool turn = r->priority == me->priority && Clock::now() >= S.quantum_end;
    if (!higher && !turn) return;
    me->running = false;
    S.current = nullptr;
    S.ready[me->priority].push_back(me);
    me->ready = true;
    me->exception_list = ld(mc(), kpcr());
    me->irql = irql();
    dispatch();
    me->cv.wait(lk, [&] { return me->running; });
    host::set_current(&(me == S.threads.front().get() ? mc() : me->ctx));
}
}  // namespace qilin::rt
