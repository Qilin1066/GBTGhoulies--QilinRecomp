// Runtime support for lifted (recompiled) game code.
//
// Generated functions take a Context, copy the guest registers into host
// locals, run, and store back the registers they modified at every exit.
// Guest memory is flat: guest address A lives at mem + A (the runtime
// reserves the 4 GB guest space). All helpers are small inline functions so
// the host compiler can keep guest registers in host registers and fold
// everything.
//
// Build switches (for test builds; off in release):
//   QILIN_CHECKED_MEMORY  bounds-check every access against mem_size
//   QILIN_TRACE_WRITES    call trace_write() on every store
#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

// ---- Floating point must be bit-exact IEEE single/double: no contraction into
// fused multiply-add, no fast-math. Clang and MSVC take pragmas; GCC needs
// -ffp-contract=off on the command line (the qilin_runtime CMake target adds it).
#if defined(__FAST_MATH__)
#error "lifted code must not be built with -ffast-math: SSE results would no longer be bit-exact"
#endif
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#pragma float_control(precise, on)
#endif

static_assert(std::endian::native == std::endian::little, "guest memory access assumes a little-endian host");

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

inline void check(const Mem& m, uint32_t a, uint64_t n, uint32_t at) {
#if defined(QILIN_CHECKED_MEMORY)
    if (uint64_t(a) + n > m.size) trap(at, "guest memory access out of bounds");
#else
    (void)m; (void)a; (void)n; (void)at;
#endif
}

inline void traced(uint32_t a, uint64_t n) {
#if defined(QILIN_TRACE_WRITES)
    if (n) trace_write(a, uint32_t(n));
#else
    (void)a; (void)n;
#endif
}

inline uint8_t ld8(const Mem& m, uint32_t a, uint32_t at) { check(m, a, 1, at); return m.base[a]; }
inline uint16_t ld16(const Mem& m, uint32_t a, uint32_t at) { check(m, a, 2, at); uint16_t v; std::memcpy(&v, m.base + a, 2); return v; }
inline uint32_t ld32(const Mem& m, uint32_t a, uint32_t at) { check(m, a, 4, at); uint32_t v; std::memcpy(&v, m.base + a, 4); return v; }

inline void st8(const Mem& m, uint32_t a, uint8_t v, uint32_t at) { check(m, a, 1, at); traced(a, 1); m.base[a] = v; }
inline void st16(const Mem& m, uint32_t a, uint16_t v, uint32_t at) { check(m, a, 2, at); traced(a, 2); std::memcpy(m.base + a, &v, 2); }
inline void st32(const Mem& m, uint32_t a, uint32_t v, uint32_t at) { check(m, a, 4, at); traced(a, 4); std::memcpy(m.base + a, &v, 4); }

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
    traced(a, 16);
    std::memcpy(m.base + a, x.u, 16);
}

// ---------------------------------------------------------------- string ops (DF = 0)
// Whole-run versions of rep stos/movs: same final registers and memory as the
// element-by-element loop, but bulk memset/memmove where that is equivalent.

inline void rep_guard(uint32_t a, uint64_t bytes, uint32_t at) {
    if (uint64_t(a) + bytes > 0x100000000ull) trap(at, "rep string operation wraps the 4 GB address space");
}

inline void rep_stos8(const Mem& m, uint32_t& edi, uint32_t& ecx, uint32_t eax, uint32_t at) {
    const uint64_t n = ecx;
    if (!n) return;
    rep_guard(edi, n, at);
    check(m, edi, n, at);
    traced(edi, n);
    std::memset(m.base + edi, int(eax & 0xFF), size_t(n));
    edi += uint32_t(n);
    ecx = 0;
}

inline void rep_stos32(const Mem& m, uint32_t& edi, uint32_t& ecx, uint32_t eax, uint32_t at) {
    const uint64_t n = ecx;
    if (!n) return;
    rep_guard(edi, n * 4, at);
    check(m, edi, n * 4, at);
    traced(edi, n * 4);
    uint8_t* p = m.base + edi;
    if ((eax & 0xFF) * 0x01010101u == eax) {
        std::memset(p, int(eax & 0xFF), size_t(n * 4));  // byte-uniform pattern (0, -1, ...)
    } else {
        for (uint64_t k = 0; k < n; ++k) std::memcpy(p + 4 * k, &eax, 4);
    }
    edi += uint32_t(n * 4);
    ecx = 0;
}

// Element-wise forward copy. When the destination starts inside the source
// run (dst > src, overlapping) this replicates the pattern exactly as the CPU
// does; otherwise it is an ordinary memmove.
inline void rep_movs(const Mem& m, uint32_t& esi, uint32_t& edi, uint32_t& ecx, uint32_t size, uint32_t at) {
    const uint64_t n = ecx;
    if (!n) return;
    const uint64_t bytes = n * size;
    rep_guard(esi, bytes, at);
    rep_guard(edi, bytes, at);
    check(m, esi, bytes, at);
    check(m, edi, bytes, at);
    traced(edi, bytes);
    uint8_t* d = m.base + edi;
    const uint8_t* s = m.base + esi;
    if (edi <= esi || uint64_t(edi) >= uint64_t(esi) + bytes) {
        std::memmove(d, s, size_t(bytes));
    } else {
        for (uint64_t k = 0; k < bytes; k += size) std::memcpy(d + k, s + k, size);  // overlapping forward copy
    }
    esi += uint32_t(bytes);
    edi += uint32_t(bytes);
    ecx = 0;
}

// ---------------------------------------------------------------- registers
inline void set_lo8(uint32_t& r, uint32_t v) { r = (r & 0xFFFFFF00u) | (v & 0xFFu); }
inline void set_hi8(uint32_t& r, uint32_t v) { r = (r & 0xFFFF00FFu) | ((v & 0xFFu) << 8); }
inline void set_lo16(uint32_t& r, uint32_t v) { r = (r & 0xFFFF0000u) | (v & 0xFFFFu); }

// ---------------------------------------------------------------- flags
// x86 PF: set when the low byte of the result has an even number of 1 bits.
inline bool parity8(uint32_t v) { return (std::popcount(v & 0xFFu) & 1) == 0; }

// ---------------------------------------------------------------- floats
inline float f32(uint32_t u) { return std::bit_cast<float>(u); }
inline uint32_t u32(float f) { return std::bit_cast<uint32_t>(f); }

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
