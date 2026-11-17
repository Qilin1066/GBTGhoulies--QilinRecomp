// Test-build runtime for the differential tests: links the lifted functions
// into a shared library the Python driver calls through ctypes.
//
// Build (difftest.py does this):
//   c++ -std=c++20 -O1 -shared -fPIC -ffp-contract=off \
//       -DQILIN_CHECKED_MEMORY -DQILIN_TRACE_WRITES -I recomp/runtime \
//       gen/*.cpp tools/difftest/harness.cpp -o libdifftest.so
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "guest.hpp"

namespace qilin::gen {
struct Entry {
    uint32_t addr;
    void (*fn)(qilin::rt::Context&);
};
extern const Entry kFunctions[];
extern const size_t kFunctionCount;
}  // namespace qilin::gen

namespace {
struct Trap {
    uint32_t addr;
    const char* why;
};
std::vector<uint32_t> g_writes;  // (addr, size) pairs
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
EXPORT uint32_t qd_function_addr(int i) { return qilin::gen::kFunctions[i].addr; }

// 0 = returned, 1 = no such function, 2 = trapped (message in err)
EXPORT int qd_call(uint32_t addr, qilin::rt::Context* c, char* err, int errlen) {
    g_writes.clear();
    for (size_t i = 0; i < qilin::gen::kFunctionCount; ++i) {
        if (qilin::gen::kFunctions[i].addr != addr) continue;
        try {
            qilin::gen::kFunctions[i].fn(*c);
            return 0;
        } catch (const Trap& t) {
            std::snprintf(err, size_t(errlen), "trap at %08X: %s", t.addr, t.why);
            return 2;
        }
    }
    return 1;
}

EXPORT int qd_writes(uint32_t* out, int max_pairs) {
    int n = int(g_writes.size() / 2);
    int k = n < max_pairs ? n : max_pairs;
    std::memcpy(out, g_writes.data(), size_t(k) * 8);
    return n;
}
