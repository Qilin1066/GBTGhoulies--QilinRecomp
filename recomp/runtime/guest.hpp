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
//   QILIN_TEST_HOOKS      every lifted function opens a Frame (enter_frame /
//                         leave_frame) and every backward branch ticks, so a
//                         test harness can stop runaway recursion or loops;
//                         tail calls are marked (tail_call): the frame records
//                         its entry esp, where the function's own return
//                         address is (a tail-called callee returns there)
#pragma once

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "x87_math.hpp"

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
    uint32_t ret_to;  // return address popped by the last `ret` (callers check it)
    uint32_t fs_base; // guest address of the thread's TIB: fs:[0] heads its exception-registration chain
    Xmm xmm[8];
    // x87 (slice 3). Registers are doubles, in physical order: ST(i) is
    // st[(ftop + i) & 7]. See the x87 section below for why doubles are exact.
    double st[8];
    uint32_t ftop;  // TOP
    uint32_t ftag;  // bit p set: physical register p holds a value
    uint32_t fpcw;  // control word: precision control in bits 8-9, rounding in 10-11
    uint32_t fpsw;  // status word except TOP: condition codes C0-C3, sticky exception bits
    uint8_t* mem;
    uint64_t mem_size;
    // MMX registers. On the hardware they alias the x87 registers' mantissas;
    // here they're separate (the x87 registers are doubles), and MMX
    // instructions set the x87 state as the hardware does: TOP 0 and every
    // register in use, then emms marks them all empty. XGraphics' swizzler
    // uses them between an x87-free entry and its emms.
    uint64_t mm[8];
};

// Fail loudly: unknown jump-table index, divide error, int3, bad access...
[[noreturn]] void trap(uint32_t guest_addr, const char* why);

// ---------------------------------------------------------------- calls
// A lifted function ends with its guest `ret`, which pops the return address
// into c.ret_to. Direct calls are plain C++ calls (fn_XXXXXXXX(c)); after one
// returns, the caller checks it came back to the call site.
inline void check_return(const Context& c, uint32_t expect, uint32_t at) {
    if (c.ret_to != expect) [[unlikely]]
        trap(at, "callee returned somewhere other than its call site");
}

// Indirect call or jump to a guest address (call [eax+0x14], jmp eax): the
// lifted function at `target` from the generated table, else call_unlifted.
// Defined in dispatch.cpp.
void call(Context& c, uint32_t target);

// Code with no lifted body: library code that is replaced by native
// implementations (HLE), or game code that doesn't lift yet. Entered like
// the guest function: the return address is on the guest stack, and it must
// leave the Context as that function's `ret` would (esp popped, c.ret_to
// set). Provided by whatever links the generated code: the runtime, or the
// differential test, which runs the original bytes.
void call_unlifted(Context& c, uint32_t target);
// Whether target has a lifted body in the generated table.
bool is_lifted(uint32_t target);

// Device registers: a load or store at a constant address in the Xbox's
// register space (kMmioLo..kMmioHi: the NV2A at 0xFD000000, the APU at
// 0xFE800000 …) is lifted to these, which the runtime implements.
inline constexpr uint32_t kMmioLo = 0xFD000000u, kMmioHi = 0xFF000000u;
uint32_t mmio_read32(uint32_t addr, uint32_t at);
void mmio_write32(uint32_t addr, uint32_t value, uint32_t at);

#if defined(QILIN_TRACE_WRITES)
void trace_write(uint32_t addr, uint32_t size);
#endif

#if defined(QILIN_TEST_HOOKS)
void enter_frame(uint32_t fn, uint32_t entry_esp);
void leave_frame();
struct Frame {
    Frame(uint32_t fn, uint32_t entry_esp) { enter_frame(fn, entry_esp); }
    ~Frame() { leave_frame(); }
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
};
void tick();
void tail_call();
#define QILIN_FRAME(fn, esp) const ::qilin::rt::Frame qilin_frame_(fn, esp)
#define QILIN_TICK() ::qilin::rt::tick()
#define QILIN_TAIL() ::qilin::rt::tail_call()
#else
#define QILIN_FRAME(fn, esp) ((void)0)
#define QILIN_TICK() ((void)0)
#define QILIN_TAIL() ((void)0)
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
inline uint64_t ld64(const Mem& m, uint32_t a, uint32_t at) { check(m, a, 8, at); uint64_t v; std::memcpy(&v, m.base + a, 8); return v; }
inline void st64(const Mem& m, uint32_t a, uint64_t v, uint32_t at) { check(m, a, 8, at); traced(a, 8); std::memcpy(m.base + a, &v, 8); }
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

inline void rep_stos16(const Mem& m, uint32_t& edi, uint32_t& ecx, uint32_t eax, uint32_t at) {
    const uint64_t n = ecx;
    if (!n) return;
    rep_guard(edi, n * 2, at);
    check(m, edi, n * 2, at);
    traced(edi, n * 2);
    const uint16_t v = uint16_t(eax);
    for (uint64_t k = 0; k < n; ++k) std::memcpy(m.base + edi + 2 * k, &v, 2);
    edi += uint32_t(n * 2);
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

// rep movs with DF = 1 (std ... cld): elements are copied from the highest
// address down, starting at esi/edi. Equivalent to memmove unless the
// destination starts inside the source run below it (then the CPU's
// element-by-element order replicates a pattern, reproduced here).
inline void rep_movs_back(const Mem& m, uint32_t& esi, uint32_t& edi, uint32_t& ecx, uint32_t size, uint32_t at) {
    const uint64_t n = ecx;
    if (!n) return;
    const uint64_t bytes = n * size;
    const uint64_t span = bytes - size;  // from the lowest element to the first
    if (esi < span || edi < span) trap(at, "rep string operation wraps the 4 GB address space");
    const uint32_t s0 = uint32_t(esi - span), d0 = uint32_t(edi - span);  // lowest elements
    rep_guard(s0, bytes, at);
    rep_guard(d0, bytes, at);
    check(m, s0, bytes, at);
    check(m, d0, bytes, at);
    traced(d0, bytes);
    if (d0 >= s0 || uint64_t(d0) + bytes <= s0) {
        std::memmove(m.base + d0, m.base + s0, size_t(bytes));
    } else {
        for (uint64_t k = 0; k < n; ++k)  // overlapping, destination below: highest element first
            std::memcpy(m.base + edi - k * size, m.base + esi - k * size, size);
    }
    esi -= uint32_t(bytes);
    edi -= uint32_t(bytes);
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
// Scalar SSE arithmetic with x86's NaN results, whatever the host CPU or the
// compiler's operand order: a NaN operand propagates, quietened (the first
// operand's if both are NaN); an invalid operation on non-NaN operands
// (inf - inf, 0 * inf, 0 / 0, sqrt of a negative) gives the default NaN
// 0xFFC00000. ARM hosts would otherwise give 0x7FC00000 and pick NaNs by
// their own rules. One compare on the result: NaN results are rare.
inline bool is_nan32(uint32_t a) { return (a & 0x7FFFFFFFu) > 0x7F800000u; }
inline uint32_t sse_nan(uint32_t a, uint32_t b) {
    return is_nan32(a) ? (a | 0x00400000u) : is_nan32(b) ? (b | 0x00400000u) : 0xFFC00000u;
}
inline uint32_t addss(uint32_t a, uint32_t b) { const float r = f32(a) + f32(b); return r == r ? u32(r) : sse_nan(a, b); }
inline uint32_t subss(uint32_t a, uint32_t b) { const float r = f32(a) - f32(b); return r == r ? u32(r) : sse_nan(a, b); }
inline uint32_t mulss(uint32_t a, uint32_t b) { const float r = f32(a) * f32(b); return r == r ? u32(r) : sse_nan(a, b); }
// IEEE division: x / 0 is an infinity or a NaN by design (MSVC's C4723 would
// flag it wherever a constant zero divisor gets inlined).
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4723)
#endif
inline uint32_t divss(uint32_t a, uint32_t b) { const float r = f32(a) / f32(b); return r == r ? u32(r) : sse_nan(a, b); }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
inline uint32_t sqrtss(uint32_t a) { const float r = std::sqrt(f32(a)); return r == r ? u32(r) : sse_nan(a, a); }

// ---------------------------------------------------------------- x87
// Registers hold doubles. That is exact for what the game does:
//  - It runs with precision control at 24 bits (_controlfp(_PC_24) at
//    startup), so every add, subtract, multiply, divide and square root rounds
//    its result to a 24-bit significand, while the exponent keeps the
//    register's range (unlike a C++ float). x87_round does that rounding.
//  - When both operands have at most 24 significant bits, computing in double
//    and rounding once to 24 bits gives the correctly rounded result (53 >=
//    2*24 + 2). When one is wider (a loaded double or integer, a constant, a
//    transcendental result), the double result is first made round-to-odd
//    using its exact error (fma / TwoSum), which makes the second rounding
//    exact too.
//  - With precision control at 53 bits (the C runtime's setting before the
//    game changes it), double arithmetic is the x87 arithmetic.
// Not exact: values wider than 53 bits (fild of a 64-bit integer above 2^53,
// the 64-bit constants such as fldpi, and transcendental results, which the
// x87 computes to 64 bits; these use the host's double maths), precision
// control at 64 bits, and exponents beyond double's range.
inline uint64_t dbits(double d) { return std::bit_cast<uint64_t>(d); }
inline double dfrom(uint64_t b) { return std::bit_cast<double>(b); }
inline bool is_nan64(uint64_t b) { return (b & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull; }
inline bool is_snan64(uint64_t b) { return is_nan64(b) && !(b & (1ull << 51)); }
constexpr uint64_t kX87Indefinite = 0xFFF8000000000000ull;  // the "real indefinite" QNaN
inline double quiet64(double a) { return dfrom(dbits(a) | (1ull << 51)); }

// x87 NaN results: a QNaN beats an SNaN; between two of the same kind the
// larger significand wins (the positive one on a tie); a single NaN operand
// propagates; an invalid operation on non-NaNs gives the indefinite.
inline double x87_nan(double a, double b) {
    const uint64_t A = dbits(a), B = dbits(b);
    const bool an = is_nan64(A), bn = is_nan64(B);
    if (an && bn) {
        const bool as = is_snan64(A), bs = is_snan64(B);
        if (as != bs) return quiet64(as ? b : a);
        const uint64_t ma = A & 0x000FFFFFFFFFFFFFull, mb = B & 0x000FFFFFFFFFFFFFull;
        if (ma != mb) return quiet64(ma > mb ? a : b);
        return quiet64((A >> 63) <= (B >> 63) ? a : b);
    }
    if (an) return quiet64(a);
    if (bn) return quiet64(b);
    return dfrom(kX87Indefinite);
}
inline double x87_nan1(double a) { return is_nan64(dbits(a)) ? quiet64(a) : dfrom(kX87Indefinite); }

// Memory operands. f32_raw/f64_raw keep an SNaN signalling (it matters for
// which NaN an arithmetic result propagates); fld quietens, as the x87 does.
inline double f32_raw(uint32_t u) {
    if ((u & 0x7FFFFFFFu) > 0x7F800000u)
        return dfrom(uint64_t(u >> 31) << 63 | 0x7FF0000000000000ull | uint64_t(u & 0x7FFFFFu) << 29);
    return double(f32(u));
}
inline double f64_raw(uint64_t u) { return dfrom(u); }
inline double fld32(uint32_t u) { const double d = f32_raw(u); return d == d ? d : quiet64(d); }
inline double fld64(uint64_t u) { const double d = dfrom(u); return d == d ? d : quiet64(d); }
// fst m32: round to single precision and range (nearest); a NaN keeps the
// top of its payload.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4756)  // overflow to infinity is intended (IEEE)
#endif
inline uint32_t fst32(double d) {
    const uint64_t b = dbits(d);
    if (is_nan64(b)) return uint32_t(b >> 63) << 31 | 0x7FC00000u | uint32_t((b >> 29) & 0x3FFFFFu);
    return u32(float(d));
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// Round to a 24-bit significand, ties to even, keeping double's exponent range.
inline double round24(double x) {
    uint64_t b = dbits(x);
    if ((b & 0x7FF0000000000000ull) == 0x7FF0000000000000ull) return x;  // inf, NaN
    b += 0x0FFFFFFFull + ((b >> 29) & 1u);
    return dfrom(b & ~0x1FFFFFFFull);
}
inline bool narrow24(double x) { return (dbits(x) & 0x1FFFFFFFull) == 0; }
// r was rounded to nearest from r + e (e exact): make it round-to-odd.
inline double to_odd(double r, double e) {
    if (e == 0.0 || (dbits(r) & 1u)) return r;
    return std::nextafter(r, e > 0.0 ? INFINITY : -INFINITY);
}
inline unsigned x87_pc(uint32_t cw) { return (cw >> 8) & 3u; }  // 0: 24 bits, 2: 53, 3: 64

// The precision-controlled operations. `wide` is the exact-error fallback.
template <class Op, class Err>
inline double x87_arith(uint32_t cw, double a, double b, Op op, Err err) {
    const double r = op(a, b);
    if (r != r) [[unlikely]] return x87_nan(a, b);
    if (x87_pc(cw) != 0u || !std::isfinite(r)) return r;
    if (narrow24(a) && narrow24(b)) return round24(r);
    return round24(to_odd(r, err(a, b, r)));
}
inline double fadd(uint32_t cw, double a, double b) {
    return x87_arith(cw, a, b, [](double x, double y) { return x + y; },
                     [](double x, double y, double r) { const double bb = r - x; return (x - (r - bb)) + (y - bb); });
}
inline double fsub(uint32_t cw, double a, double b) {
    return x87_arith(cw, a, b, [](double x, double y) { return x - y; },
                     [](double x, double y, double r) { const double bb = r - x; return (x - (r - bb)) + (-y - bb); });
}
inline double fmul(uint32_t cw, double a, double b) {
    return x87_arith(cw, a, b, [](double x, double y) { return x * y; },
                     [](double x, double y, double r) { return std::fma(x, y, -r); });
}
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4723)  // IEEE division by zero is intended
#endif
inline double fdiv(uint32_t cw, double a, double b) {
    return x87_arith(cw, a, b, [](double x, double y) { return x / y; },
                     [](double x, double y, double r) {
                         const double rem = std::fma(-r, y, x);  // x - r*y, exact
                         if (rem == 0.0) return 0.0;  // exact
                         return (rem < 0.0) != (y < 0.0) ? -1.0 : 1.0;  // sign of x/y - r
                     });
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
inline double fsqrt(uint32_t cw, double a) {
    const double r = std::sqrt(a);
    if (r != r) [[unlikely]] return x87_nan1(a);
    if (x87_pc(cw) != 0u || !std::isfinite(r)) return r;
    if (narrow24(a)) return round24(r);
    const double rem = std::fma(-r, r, a);  // a - r*r, exact
    return round24(to_odd(r, rem));
}
// Transcendentals (precision control doesn't apply): x87_math.hpp, the same
// bits on every host. Special operands follow the x87 tables, which match
// C's for atan2 and IEEE's for y * log2(x).
inline double fsin(double a) {
    if (!std::isfinite(a)) return x87_nan1(a);  // an infinity is invalid
    return a == 0 ? a : xm::sin(a);
}
inline double fcos(double a) {
    if (!std::isfinite(a)) return x87_nan1(a);
    return a == 0 ? 1.0 : xm::cos(a);
}
inline double ftan(double a) {
    if (!std::isfinite(a)) return x87_nan1(a);
    return a == 0 ? a : xm::tan(a);
}
inline double fpatan(double y, double x) {  // atan2(y, x)
    if (y != y || x != x) return x87_nan(y, x);
    constexpr double pi = 0x1.921fb54442d18p+1, pi_2 = 0x1.921fb54442d18p+0, pi_4 = 0x1.921fb54442d18p-1,
                     pi3_4 = 0x1.2d97c7f3321d2p+1;
    if (y == 0) return x > 0 || (x == 0 && !std::signbit(x)) ? y : std::copysign(pi, y);
    if (std::isinf(y)) return std::copysign(std::isinf(x) ? (x > 0 ? pi_4 : pi3_4) : pi_2, y);
    if (x == 0) return std::copysign(pi_2, y);
    if (std::isinf(x)) return std::copysign(x > 0 ? 0.0 : pi, y);
    return xm::atan2(y, x);
}
inline double fyl2x(double y, double x) {  // y * log2(x)
    if (y != y || x != x) return x87_nan(y, x);
    if (std::isfinite(y) && y != 0 && std::isfinite(x) && x > 0 && x != 1.0) return xm::yl2x(y, x);
    // Special operands: IEEE y * log2(x), with log2 of 0, +inf and 1 being
    // -inf, +inf and 0, and of a negative number invalid. inf * 0 is invalid.
    const double inf = dfrom(0x7FF0000000000000ull);
    const double l = x < 0 ? dfrom(kX87Indefinite) : x == 0 ? -inf : x == inf ? inf : x == 1.0 ? 0.0 : x < 1.0 ? -1.0 : 1.0;
    const double r = y * l;
    return r == r ? r : x87_nan(y, x);
}
// fsin, fcos, fptan leave operands of magnitude 2^63 or more unchanged (C2 = 1).
inline bool x87_trig_in_range(double a) { return std::fabs(a) < 9223372036854775808.0; }

// Round to an integer by the control word's rounding mode (fist, frndint).
inline double x87_round(uint32_t cw, double a) {
    switch ((cw >> 10) & 3u) {
        case 0: return std::nearbyint(a);  // the host's mode is nearest
        case 1: return std::floor(a);
        case 2: return std::ceil(a);
        default: return std::trunc(a);
    }
}
// fist/fistp: round by the control word's rounding mode; out of range (or
// NaN) gives the integer indefinite.
inline uint32_t fist32(uint32_t cw, double a) {
    const double r = x87_round(cw, a);
    if (!(r >= -2147483648.0 && r <= 2147483647.0)) return 0x80000000u;
    return uint32_t(int32_t(r));
}
// fistp m64: the integer indefinite 0x8000000000000000 on overflow or NaN.
inline uint64_t fist64(uint32_t cw, double a) {
    const double r = x87_round(cw, a);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0)) return 0x8000000000000000ull;
    return uint64_t(int64_t(r));
}
// frndint: NaNs quietened, infinities and zeros unchanged.
inline double frndint(uint32_t cw, double a) {
    if (a != a) return x87_nan1(a);
    return x87_round(cw, a);
}
// f2xm1: 2^ST0 - 1, correctly rounded (x87_math.hpp). The x87 defines it for
// |x| <= 1 only; this is 2^x - 1 everywhere, as the difftest's reference is.
inline double f2xm1(double a) {
    if (a != a) return x87_nan1(a);
    if (a == 0) return a;
    if (std::isinf(a)) return a > 0 ? a : -1.0;
    return xm::exp2m1(a);
}
// fscale: ST0 * 2^trunc(ST1). Exact unless it leaves double's range.
inline double fscale(double a, double b) {
    if (a != a || b != b) return x87_nan(a, b);
    if (std::isinf(b)) {
        if ((b > 0 && a == 0) || (b < 0 && std::isinf(a))) return dfrom(kX87Indefinite);  // 0 * 2^inf, inf * 2^-inf
        return b > 0 ? (a == 0 ? a : std::copysign(dfrom(0x7FF0000000000000ull), a)) : std::copysign(0.0, a);
    }
    const double t = std::trunc(b);
    const int n = t > 100000.0 ? 100000 : t < -100000.0 ? -100000 : int(t);
    return std::ldexp(a, n);
}
// fxam: C3 C2 C0 classify ST0 (or an empty register), C1 is its sign.
inline uint32_t fxam_codes(uint32_t sw, bool empty, double a) {
    sw &= ~0x4700u;
    const uint64_t b = dbits(a);
    if (b >> 63) sw |= 0x0200u;  // C1
    if (empty) return sw | 0x4100u;                                        // C3 C0: empty
    if (a != a) return sw | 0x0100u;                                       // C0: NaN
    if (std::isinf(a)) return sw | 0x0500u;                                // C2 C0: infinity
    if (a == 0) return sw | 0x4000u;                                       // C3: zero
    if (((b >> 52) & 0x7FF) == 0) return sw | 0x4400u;                    // C3 C2: denormal
    return sw | 0x0400u;                                                   // C2: normal
}
// fld m80: the nearest double (64-bit significands are rounded, like the x87's own constants).
inline double fld80(uint64_t mant, uint16_t se) {
    const bool neg = se >> 15;
    const int e = se & 0x7FFF;
    if (e == 0x7FFF) {
        if ((mant << 1) == 0) return neg ? -dfrom(0x7FF0000000000000ull) : dfrom(0x7FF0000000000000ull);
        return dfrom(uint64_t(neg) << 63 | 0x7FF8000000000000ull | ((mant >> 11) & 0x0007FFFFFFFFFFFFull));  // quietened
    }
    if (mant == 0) return neg ? -0.0 : 0.0;
    // value = mant * 2^(e - 16383 - 63); ldexp of the 64-bit integer rounds once only if it's exact in double:
    // split into two exact halves and let one addition round.
    const double hi = double(mant >> 11) * 2048.0, lo = double(mant & 0x7FF);  // both exact
    const int sh = (e == 0 ? 1 : e) - 16383 - 63;
    const double r = std::ldexp(hi + lo, sh);  // hi + lo rounds to 53 bits once; ldexp is exact in range
    return neg ? -r : r;
}
// A register value as an 80-bit extended real (exact: every double is one).
inline void st80(const Mem& m, uint32_t a, double v, uint32_t at) {
    const uint64_t b = dbits(v);
    const uint16_t sign = uint16_t((b >> 63) << 15);
    const uint32_t e = uint32_t(b >> 52) & 0x7FF;
    const uint64_t frac = b & 0x000FFFFFFFFFFFFFull;
    uint64_t mant;
    uint32_t ex;
    if (e == 0x7FF) { ex = 0x7FFF; mant = 0x8000000000000000ull | (frac << 11); }
    else if (e != 0) { ex = e - 1023 + 16383; mant = 0x8000000000000000ull | (frac << 11); }
    else if (frac == 0) { ex = 0; mant = 0; }
    else {  // a double subnormal is a normal extended real
        const int sh = std::countl_zero(frac);
        mant = frac << sh;
        ex = uint32_t(15372 - sh);
    }
    st64(m, a, mant, at);
    st16(m, a + 8, uint16_t(sign | ex), at);
}
// fnsave / frstor, 32-bit protected-mode layout (108 bytes): control,
// status (with TOP) and tag words as dwords, the instruction and operand
// pointers (stored as 0, as the difftest's emulator does; nothing reads
// them back but frstor), then ST(0)..ST(7) as 80-bit reals. fnsave then
// reinitialises the unit like fninit.
inline uint32_t x87_tag2(bool full, double v) {
    if (!full) return 3;
    if (v == 0) return 1;
    return std::isfinite(v) ? 0 : 2;
}
inline void fnsave(const Mem& m, uint32_t a, double* st, uint32_t& top, uint32_t& tag, uint32_t& cw, uint32_t& sw, uint32_t at) {
    uint32_t tw = 0;
    for (uint32_t p = 0; p < 8; ++p) tw |= x87_tag2((tag >> p) & 1u, st[p]) << (2 * p);
    st32(m, a, cw & 0xFFFFu, at);
    st32(m, a + 4, ((sw & ~0x3800u) | (top << 11)) & 0xFFFFu, at);
    st32(m, a + 8, tw, at);
    for (uint32_t k = 12; k < 28; k += 4) st32(m, a + k, 0u, at);
    for (uint32_t i = 0; i < 8; ++i) st80(m, a + 28 + 10 * i, st[(top + i) & 7u], at);
    cw = 0x037F;
    sw = 0;
    tag = 0;
    top = 0;
}
inline void frstor(const Mem& m, uint32_t a, double* st, uint32_t& top, uint32_t& tag, uint32_t& cw, uint32_t& sw, uint32_t at) {
    cw = ld16(m, a, at);
    const uint32_t w = ld16(m, a + 4, at);
    top = (w >> 11) & 7u;
    sw = w & ~0x3800u;
    const uint32_t tw = ld16(m, a + 8, at);
    tag = 0;
    for (uint32_t p = 0; p < 8; ++p)
        if (((tw >> (2 * p)) & 3u) != 3u) tag |= 1u << p;
    for (uint32_t i = 0; i < 8; ++i) st[(top + i) & 7u] = fld80(ld64(m, a + 28 + 10 * i, at), ld16(m, a + 36 + 10 * i, at));
}
// fcom/fucom/ftst condition codes in the status word: C3 C2 C0 = 111
// unordered, 000 greater, 001 less, 100 equal; C1 cleared.
inline uint32_t fcom_codes(uint32_t sw, double a, double b) {
    sw &= ~0x4700u;
    if (a != a || b != b) return sw | 0x4500u;
    if (a < b) return sw | 0x0100u;
    if (a == b) return sw | 0x4000u;
    return sw;
}

// minss/maxss: return the second operand if either is NaN or both are zero.
inline uint32_t minss(uint32_t a, uint32_t b) { return f32(a) < f32(b) ? a : b; }
inline uint32_t maxss(uint32_t a, uint32_t b) { return f32(a) > f32(b) ? a : b; }

}  // namespace qilin::rt

// The generated function table (table.cpp), sorted by address.
namespace qilin::gen {
struct Entry {
    uint32_t addr;
    void (*fn)(qilin::rt::Context&);
};
extern const Entry kFunctions[];  // terminated by {0, nullptr}
extern const size_t kFunctionCount;
}  // namespace qilin::gen
