// Test-build runtime for the differential tests: links the lifted functions
// into a shared library the Python driver calls through ctypes.
//
// Built by difftest.py with the lifted gen/*.cpp: -std=c++23 -O2 -fPIC
// -ffp-contract=off -DQILIN_CHECKED_MEMORY -DQILIN_TRACE_WRITES, linked -shared.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

#include "guest.hpp"

namespace qilin::gen {
struct Entry {
    uint32_t addr;
    void (*fn)(qilin::rt::Context&);
};
extern const Entry kFunctions[];  // sorted by address, terminated by {0, nullptr}
extern const size_t kFunctionCount;
}  // namespace qilin::gen

namespace {
struct Trap {
    uint32_t addr;
    const char* why;
};
std::vector<uint32_t> g_writes;  // (addr, size) pairs

std::span<const qilin::gen::Entry> functions() { return {qilin::gen::kFunctions, qilin::gen::kFunctionCount}; }
}  // namespace

namespace qilin::rt {
void trap(uint32_t guest_addr, const char* why) { throw Trap{guest_addr, why}; }
void trace_write(uint32_t addr, uint32_t size) {
    g_writes.push_back(addr);
    g_writes.push_back(size);
}
}  // namespace qilin::rt

#if defined(_WIN32)
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C" __attribute__((visibility("default")))
#endif

EXPORT int qd_context_size() { return int(sizeof(qilin::rt::Context)); }
EXPORT int qd_function_count() { return int(qilin::gen::kFunctionCount); }
EXPORT uint32_t qd_function_addr(int i) { return functions()[size_t(i)].addr; }

// 0 = returned, 1 = no such function, 2 = trapped (message in err)
EXPORT int qd_call(uint32_t addr, qilin::rt::Context* c, char* err, int errlen) {
    g_writes.clear();
    const auto fns = functions();
    const auto it = std::ranges::lower_bound(fns, addr, {}, &qilin::gen::Entry::addr);
    if (it == fns.end() || it->addr != addr) return 1;
    try {
        it->fn(*c);
        return 0;
    } catch (const Trap& t) {
        std::snprintf(err, size_t(errlen), "trap at %08X: %s", t.addr, t.why);
        return 2;
    }
}

EXPORT int qd_writes(uint32_t* out, int max_pairs) {
    const int n = int(g_writes.size() / 2);
    const int k = std::min(n, max_pairs);
    if (k > 0) std::memcpy(out, g_writes.data(), size_t(k) * 8);  // (memcpy from null is UB even for 0 bytes)
    return n;
}
