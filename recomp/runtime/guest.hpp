// Runtime support for lifted (recompiled) game code.
//
// Generated functions take a Context, copy the guest registers into host
// locals, run, and write them back at every exit. Guest memory is flat: guest
// address A lives at mem + A (the runtime reserves the 4 GB guest space).
// All helpers are small inline functions so the host compiler can keep guest
// registers in host registers and fold everything.
//
// Build switches (for test builds; off in release):
//   QILIN_CHECKED_MEMORY  bounds-check every access against mem_size
//   QILIN_TRACE_WRITES    call qilin_rt_trace_write() on every store
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace qilin::rt {

struct Xmm {
    uint32_t u[4];
};

struct Context {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t eflags;  // not modelled across calls yet; reserved
    uint32_t ret_to;  // return address popped by the last `ret` (for checking stack balance)
    Xmm xmm[8];
    uint8_t* mem;
    uint64_t mem_size;
};

// Fail loudly: unknown jump-table index, divide error, int3, bad access...
[[noreturn]] void trap(uint32_t guest_addr, const char* why);

#if defined(QILIN_TRACE_WRITES)
void trace_write(uint32_t addr, uint32_t size);
#endif

// ---------------------------------------------------------------- memory
struct Mem {
    uint8_t* base;
    uint64_t size;
};

inline void check(const Mem& m, uint32_t a, uint32_t n, uint32_t at) {
#if defined(QILIN_CHECKED_MEMORY)
    if (uint64_t(a) + n > m.size) trap(at, "guest memory access out of bounds");
#else
    (void)m; (void)a; (void)n; (void)at;
#endif
}

inline uint8_t ld8(const Mem& m, uint32_t a, uint32_t at) { check(m, a, 1, at); return m.base[a]; }
inline uint16_t ld16(const Mem& m, uint32_t a, uint32_t at) { check(m, a, 2, at); uint16_t v; std::memcpy(&v, m.base + a, 2); return v; }
inline uint32_t ld32(const Mem& m, uint32_t a, uint32_t at) { check(m, a, 4, at); uint32_t v; std::memcpy(&v, m.base + a, 4); return v; }

inline void st8(const Mem& m, uint32_t a, uint8_t v, uint32_t at) {
    check(m, a, 1, at);
#if defined(QILIN_TRACE_WRITES)
    trace_write(a, 1);
#endif
    m.base[a] = v;
}
inline void st16(const Mem& m, uint32_t a, uint16_t v, uint32_t at) {
    check(m, a, 2, at);
#if defined(QILIN_TRACE_WRITES)
    trace_write(a, 2);
#endif
    std::memcpy(m.base + a, &v, 2);
}
inline void st32(const Mem& m, uint32_t a, uint32_t v, uint32_t at) {
    check(m, a, 4, at);
#if defined(QILIN_TRACE_WRITES)
    trace_write(a, 4);
#endif
    std::memcpy(m.base + a, &v, 4);
}
inline Xmm ld128(const Mem& m, uint32_t a, bool aligned, uint32_t at) {
    if (aligned && (a & 15)) trap(at, "misaligned 16-byte SSE access (#GP)");
    check(m, a, 16, at);
    Xmm x;
    std::memcpy(x.u, m.base + a, 16);
    return x;
}
inline void st128(const Mem& m, uint32_t a, const Xmm& x, bool aligned, uint32_t at) {
    if (aligned && (a & 15)) trap(at, "misaligned 16-byte SSE access (#GP)");
    check(m, a, 16, at);
#if defined(QILIN_TRACE_WRITES)
    trace_write(a, 16);
#endif
    std::memcpy(m.base + a, x.u, 16);
}

// ---------------------------------------------------------------- registers
inline void set_lo8(uint32_t& r, uint32_t v) { r = (r & 0xFFFFFF00u) | (v & 0xFFu); }
inline void set_hi8(uint32_t& r, uint32_t v) { r = (r & 0xFFFF00FFu) | ((v & 0xFFu) << 8); }
inline void set_lo16(uint32_t& r, uint32_t v) { r = (r & 0xFFFF0000u) | (v & 0xFFFFu); }

// ---------------------------------------------------------------- flags
inline bool parity8(uint32_t v) {  // x86 PF: set when the low byte has even parity
    v &= 0xFF;
    v ^= v >> 4;
    return !((0x6996u >> (v & 0xF)) & 1);
}

// ---------------------------------------------------------------- floats
inline float f32(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
inline uint32_t u32(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

// cvttss2si: truncate; NaN or out of range gives the "integer indefinite" 0x80000000.
inline uint32_t cvtt_f32_i32(float f) {
    if (!(f > -2147483904.0f && f < 2147483648.0f)) return 0x80000000u;
    return uint32_t(int32_t(f));
}
// cvtss2si: round to nearest even (MXCSR default), same overflow rule.
inline uint32_t cvt_f32_i32(float f) {
    if (!(f >= -2147483648.0f && f < 2147483648.0f)) return 0x80000000u;
    return uint32_t(int32_t(std::nearbyint(f)));
}
// minss/maxss: return the second operand if either is NaN or both are zero.
inline uint32_t minss(uint32_t a, uint32_t b) { return f32(a) < f32(b) ? a : b; }
inline uint32_t maxss(uint32_t a, uint32_t b) { return f32(a) > f32(b) ? a : b; }

}  // namespace qilin::rt
