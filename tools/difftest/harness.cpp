// Test-build runtime for the differential tests: links the lifted functions
// into a shared library the Python driver calls through ctypes.
//
// Built by difftest.py with the lifted gen/*.cpp and runtime/dispatch.cpp:
// -std=c++23 -O2 -fPIC -ffp-contract=off -DQILIN_CHECKED_MEMORY
// -DQILIN_TRACE_WRITES, linked -shared.
//
// Calls into code with no lifted body (rt::call_unlifted) go back to the
// driver, which runs the original bytes of that callee in a second Unicorn
// instance sharing this library's guest memory.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

#include "guest.hpp"

namespace {
struct Trap {
    uint32_t addr;
    const char* why;
};
struct Budget {};  // an unlifted callee ran past the instruction budget
struct Runaway {   // lifted code nested or called far beyond anything the original could do
    uint32_t fn;
};
int g_depth = 0;
long g_calls = 0, g_ticks = 0, g_unlifted_calls = 0;
// The original's run is capped at 200k instructions, so a correct lifted run
// stays far below these. The wall-clock cap catches anything else (e.g. a loop
// around an unlifted callee, which runs in Unicorn and is slow).
constexpr int kMaxDepth = 4096;
constexpr long kMaxCalls = 2'000'000;
constexpr long kMaxTicks = 20'000'000;  // backward branches
constexpr long kMaxUnliftedCalls = 50'000;
constexpr auto kMaxTime = std::chrono::seconds(10);
std::chrono::steady_clock::time_point g_start;
void check_time(uint32_t fn) {
    if (std::chrono::steady_clock::now() - g_start > kMaxTime) throw Runaway{fn};
}
std::vector<uint32_t> g_writes;  // (addr, size) pairs
// Runs an unlifted callee: 0 = returned, 2 = faulted, 3 = budget exceeded.
// For a tail call (jmp), the last argument is the tail-calling function's
// entry esp (where its return address is); 0 for a call.
using UnliftedFn = int (*)(qilin::rt::Context*, uint32_t, uint32_t);
bool g_tail = false;              // set by QILIN_TAIL() just before a tail call
std::vector<uint32_t> g_entry_esp;  // per active lifted frame
UnliftedFn g_unlifted = nullptr;

std::span<const qilin::gen::Entry> functions() { return {qilin::gen::kFunctions, qilin::gen::kFunctionCount}; }
}  // namespace

namespace qilin::rt {
void trap(uint32_t guest_addr, const char* why) { throw Trap{guest_addr, why}; }
// One thread here: nothing ever asks for a switch (g_preempt stays false).
void preempt() {}
// Device registers aren't mapped in the emulator either, so the original
// faults on them: the lifted code traps, and the trial counts as faulting in both.
uint32_t mmio_read32(uint32_t addr, uint32_t at) { (void)addr; trap(at, "device register read"); }
void mmio_write32(uint32_t addr, uint32_t value, uint32_t at) { (void)addr; (void)value; trap(at, "device register write"); }
// The reference's clock (EmuFixups' rdtsc: from 0 each trial, 1000 a read), so
// code that reads it compares instead of trapping.
uint64_t g_tsc = 0;
uint64_t rdtsc(uint32_t at) {
    (void)at;
    return g_tsc += 1000;
}
void trace_write(uint32_t addr, uint32_t size) {
    g_writes.push_back(addr);
    g_writes.push_back(size);
}
void enter_frame(uint32_t fn, uint32_t entry_esp) {
    g_tail = false;  // a tail call that reached lifted code
    g_entry_esp.push_back(entry_esp);
    if (++g_depth > kMaxDepth || ++g_calls > kMaxCalls) throw Runaway{fn};
    if ((g_calls & 0xFFF) == 0) check_time(fn);
}
void leave_frame() {
    --g_depth;
    if (!g_entry_esp.empty()) g_entry_esp.pop_back();
}
void tail_call() { g_tail = true; }
void tick() {
    if (++g_ticks > kMaxTicks) throw Runaway{0};
    if ((g_ticks & 0xFFFF) == 0) check_time(0);
}
void call_unlifted(Context& c, uint32_t target) {
    if (!g_unlifted) trap(target, "call into code that isn't lifted");
    if (++g_unlifted_calls > kMaxUnliftedCalls) throw Runaway{target};
    check_time(target);
    const uint32_t tail_esp = g_tail && !g_entry_esp.empty() ? g_entry_esp.back() : 0;
    g_tail = false;
    switch (g_unlifted(&c, target, tail_esp)) {
        case 0: return;
        case 3: throw Budget{};
        default: trap(target, "the original code of an unlifted callee faulted");
    }
}
}  // namespace qilin::rt

#if defined(_WIN32)
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C" __attribute__((visibility("default")))
#endif

EXPORT void qd_set_unlifted(UnliftedFn fn) { g_unlifted = fn; }
EXPORT int qd_context_size() { return int(sizeof(qilin::rt::Context)); }
EXPORT int qd_function_count() { return int(qilin::gen::kFunctionCount); }
EXPORT uint32_t qd_function_addr(int i) { return functions()[size_t(i)].addr; }

// 0 = returned, 1 = no such function, 2 = trapped (message in err),
// 3 = an unlifted callee ran past the instruction budget,
// 4 = the lifted code ran away (recursion or call count far past the original's)
EXPORT int qd_call(uint32_t addr, qilin::rt::Context* c, char* err, int errlen) {
    g_writes.clear();
    qilin::rt::g_tsc = 0;
    g_depth = 0;
    g_calls = g_ticks = g_unlifted_calls = 0;
    g_tail = false;
    g_entry_esp.clear();
    g_start = std::chrono::steady_clock::now();
    const auto fns = functions();
    const auto it = std::ranges::lower_bound(fns, addr, {}, &qilin::gen::Entry::addr);
    if (it == fns.end() || it->addr != addr) return 1;
    try {
        it->fn(*c);
        return 0;
    } catch (const Trap& t) {
        std::snprintf(err, size_t(errlen), "trap at %08X: %s", t.addr, t.why);
        return 2;
    } catch (const Budget&) {
        return 3;
    } catch (const Runaway& r) {
        std::snprintf(err, size_t(errlen),
                      "lifted code ran away (call depth %d, %ld calls, %ld unlifted calls, %ld loop iterations) at %08X", g_depth,
                      g_calls, g_unlifted_calls, g_ticks, r.fn);
        return 4;
    }
}

EXPORT int qd_writes(uint32_t* out, int max_pairs) {
    const int n = int(g_writes.size() / 2);
    const int k = std::min(n, max_pairs);
    if (k > 0) std::memcpy(out, g_writes.data(), size_t(k) * 8);  // (memcpy from null is UB even for 0 bytes)
    return n;
}
