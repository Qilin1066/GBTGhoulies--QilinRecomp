// x87 transcendentals (fsin, fcos, fptan, fpatan, fyl2x, f2xm1) with the same result
// on every host.
//
// The host's maths library differs between glibc, Apple's libm and the MSVC
// runtime, in the last bit and sometimes more. These functions use only IEEE
// addition, subtraction, multiplication and division of doubles (not even
// fma, whose library fallbacks have had bugs), which every supported host
// computes identically as long as multiply-add contraction is off
// (guest.hpp's job). So the bits are the same everywhere.
//
// Each function evaluates in double-double arithmetic, about 100 bits, and
// rounds once. The result is the correctly rounded double, subnormal results
// included, except where the true value lies within about 2^-100 of a
// rounding boundary. tools/check/x87/transcendental.py
// checks this against mpmath. The x87 itself computes 64-bit results with its
// own reduction; those aren't reproduced (see docs/recompiler-core.md).
#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <initializer_list>

// Every operation here must round exactly as written: no contraction into
// fused multiply-add (which Clang and GCC do by default on FMA hardware such
// as ARM64), no fast-math. Clang and MSVC take pragmas; GCC needs
// -ffp-contract=off on the command line (the qilin_runtime CMake target adds it).
#if defined(__FAST_MATH__)
#error "x87_math.hpp must not be built with -ffast-math: results would differ between hosts"
#endif
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#pragma float_control(precise, on)
#endif

// Counts slow-path fallbacks in tools/check/x87/consistency.cpp; nothing otherwise.
#ifndef QILIN_XM_FALLBACK
#define QILIN_XM_FALLBACK(fn)
#endif

namespace qilin::rt::xm {

struct dd {
    double hi, lo;
};

// ---- Error-free transformations from + and * only.
inline dd two_sum(double a, double b) {
    const double s = a + b, bb = s - a;
    return {s, (a - (s - bb)) + (b - bb)};
}
inline dd fast_two_sum(double a, double b) {  // |a| >= |b|, or a == 0
    const double s = a + b;
    return {s, b - (s - a)};
}
inline void split(double a, double& h, double& l) {  // Veltkamp: a = h + l, 26 bits each
    const double c = 134217729.0 * a;
    h = c - (c - a);
    l = a - h;
}
// The exact product as p + e. With a hardware fma (ARM64, x86-64 built with
// FMA) that's two instructions; otherwise Dekker's method, which needs
// |a|, |b| < 2^995. Both are exact, so the bits don't depend on which ran.
inline dd two_prod(double a, double b) {
    const double p = a * b;
#if defined(__FMA__) || defined(__ARM_FEATURE_FMA)
    return {p, std::fma(a, b, -p)};
#else
    double ah, al, bh, bl;
    split(a, ah, al);
    split(b, bh, bl);
    return {p, ((ah * bh - p) + ah * bl + al * bh) + al * bl};
#endif
}

// ---- Double-double arithmetic (relative error about 2^-104 per operation).
inline dd neg(dd a) { return {-a.hi, -a.lo}; }
inline dd add(dd a, dd b) {
    dd s = two_sum(a.hi, b.hi);
    const dd t = two_sum(a.lo, b.lo);
    s.lo += t.hi;
    s = fast_two_sum(s.hi, s.lo);
    s.lo += t.lo;
    return fast_two_sum(s.hi, s.lo);
}
inline dd sub(dd a, dd b) { return add(a, neg(b)); }
inline dd mul(dd a, dd b) {
    dd p = two_prod(a.hi, b.hi);
    p.lo += a.hi * b.lo + a.lo * b.hi;
    return fast_two_sum(p.hi, p.lo);
}
inline dd mul(dd a, double b) {
    dd p = two_prod(a.hi, b);
    p.lo += a.lo * b;
    return fast_two_sum(p.hi, p.lo);
}
inline dd div(dd a, dd b) {
    const double q1 = a.hi / b.hi;
    dd r = sub(a, mul(b, q1));
    const double q2 = r.hi / b.hi;
    r = sub(r, mul(b, q2));
    const double q3 = r.hi / b.hi;
    return add(fast_two_sum(q1, q2), dd{q3, 0.0});
}
inline dd div(dd a, double b) { return div(a, dd{b, 0.0}); }
inline dd sqrt(dd a) {  // a > 0: one Newton step from the IEEE square root, which is correctly rounded everywhere
    const double s = std::sqrt(a.hi);
    const dd r = sub(a, two_prod(s, s));
    return fast_two_sum(s, r.hi / (2.0 * s));
}
inline dd scale(dd a, double p2) { return {a.hi * p2, a.lo * p2}; }  // by a power of two: exact (no Dekker split to overflow)
inline double round(dd a) { return a.hi + a.lo; }  // one IEEE addition: the nearest double
inline double pow2(int k) { return std::bit_cast<double>(uint64_t(1023 + k) << 52); }  // -1022 <= k <= 1023

// The double nearest (a.hi + a.lo) * 2^-shift, for a normalised a and
// 53 <= shift <= 1000, rounding correctly into the subnormal range too.
inline double round_scaled(dd a, int shift) {
    const double down = pow2(-shift / 2) * pow2(-(shift - shift / 2));
    const double n = round(a) * down;
    if (n >= 0x1p-1022 || n <= -0x1p-1022 || n != n) return n;  // normal: the scaling is exact
    const double r = a.hi * down;                    // one rounding, to a multiple of 2^-1074
    const double back = r * (1.0 / down);            // exact
    const dd res = two_sum(a.hi - back, a.lo);       // what that rounding left out, exactly
    const double half = pow2(shift - 1075);          // half a subnormal step, scaled
    const double step = std::bit_cast<double>(uint64_t(1));  // 2^-1074
    const bool odd = (std::bit_cast<uint64_t>(r) & 1) != 0;
    if (res.hi > half || (res.hi == half && (res.lo > 0 || (res.lo == 0 && odd)))) return r + step;
    if (res.hi < -half || (res.hi == -half && (res.lo < 0 || (res.lo == 0 && odd)))) return r - step;
    return r;
}

// ---- Constants (mpmath, 600 bits).
inline constexpr dd kPi2{0x1.921fb54442d18p+0, 0x1.1a62633145c07p-54};    // pi/2
inline constexpr dd kPi{0x1.921fb54442d18p+1, 0x1.1a62633145c07p-53};     // pi
inline constexpr dd kInvLn2{0x1.71547652b82fep+0, 0x1.777d0ffda0d24p-56};  // 1/ln 2
// 2/pi as a 288-bit fraction, most significant word first.
inline constexpr uint32_t kTwoOverPi[9] = {0xA2F9836Eu, 0x4E441529u, 0xFC2757D1u, 0xF534DDC0u, 0xDB629599u,
                                           0x3C439041u, 0xFE5163ABu, 0xDEBBC561u, 0xB7246E3Au};

// ---- sin and cos of r in [-pi/4, pi/4], Taylor series in nested form.
inline dd sin_kernel(dd r) {
    const dd u = mul(r, r);
    dd p{1.0, 0.0};
    for (int k = 15; k >= 1; --k) p = sub(dd{1.0, 0.0}, div(mul(u, p), double(2 * k * (2 * k + 1))));
    return mul(r, p);
}
inline dd cos_kernel(dd r) {
    const dd u = mul(r, r);
    dd p{1.0, 0.0};
    for (int k = 15; k >= 1; --k) p = sub(dd{1.0, 0.0}, div(mul(u, p), double((2 * k - 1) * (2 * k))));
    return p;
}

// x = r + q * pi/2 with r in [-pi/4, pi/4], for finite |x| < 2^63. Beyond
// pi/4 the reduction is exact Payne-Hanek: the fraction of x * 2/pi is
// computed with integers, to 2^-190, which leaves over 100 significant bits
// even where x is closest to a multiple of pi/2.
inline dd reduce(double x, unsigned& q) {
    q = 0;
    const uint64_t bits = std::bit_cast<uint64_t>(x);
    const bool negative = bits >> 63;
    if (std::bit_cast<double>(bits & ~(1ull << 63)) <= 0x1.921fb54442d18p-1) return {x, 0.0};  // below pi/4
    const int e = int((bits >> 52) & 0x7FF) - 1075;  // x = m * 2^e, -53 <= e <= 10
    const uint64_t m = (bits & ((1ull << 52) - 1)) | (1ull << 52);
    // B = m * floor(2/pi * 2^288), in 32-bit limbs, least significant first.
    constexpr int kWords = 9, kLimbs = kWords + 3;
    uint32_t b[kLimbs] = {};
    const uint64_t m_lo = m & 0xFFFFFFFFu, m_hi = m >> 32;
    for (int w = 0; w < kWords; ++w) {
        const uint64_t word = kTwoOverPi[kWords - 1 - w];
        uint64_t carry = 0;
        for (int part = 0; part < 2; ++part) {
            uint64_t t = (part ? m_hi : m_lo) * word;
            for (int i = w + part; t || carry; ++i) {
                const uint64_t sum = uint64_t(b[i]) + (t & 0xFFFFFFFFu) + carry;
                b[i] = uint32_t(sum);
                carry = sum >> 32;
                t >>= 32;
            }
        }
    }
    // x * 2/pi = B * 2^(e - 288): the units bit of the quarter-turn count is at P.
    const int P = 32 * kWords - e;
    auto bit = [&](int i) { return i >= 0 && i < 32 * kLimbs ? (b[i >> 5] >> (i & 31)) & 1u : 0u; };
    q = bit(P) | bit(P + 1) << 1;
    // The fraction below P, as a magnitude; at or above one half it rounds up
    // to the next quadrant and becomes negative.
    const bool upper = bit(P - 1);
    if (upper) {  // two's complement of the P-bit fraction
        q = (q + 1) & 3;
        uint32_t borrow = 1;
        for (int i = 0; i < kLimbs; ++i) {
            const uint64_t v = uint64_t(uint32_t(~b[i])) + borrow;
            b[i] = uint32_t(v);
            borrow = uint32_t(v >> 32);
        }
    }
    int h = P - 1;
    while (h >= 0 && !bit(h)) --h;
    if (h < 0) return {0.0, 0.0};
    auto take = [&](int top) {  // 53 bits from `top` down, as an integer
        uint64_t v = 0;
        for (int i = top; i > top - 53; --i) v = v << 1 | bit(i);
        return double(v);
    };
    const double f_hi = take(h) * pow2(h - 52 - P), f_lo = take(h - 53) * pow2(h - 105 - P);
    dd r = mul(fast_two_sum(f_hi, f_lo), kPi2);
    if (upper) r = neg(r);
    if (negative) {  // sin and cos of -x: reflect the quadrant
        r = neg(r);
        q = (4 - q) & 3;
    }
    return r;
}

// ---- Slow paths: about 100 bits, used when the fast path can't decide the rounding.

// sin(x) and cos(x) for finite x, |x| < 2^63, x != 0.
inline double sin_slow(double x) {
    unsigned q;
    const dd r = reduce(x, q);
    const dd v = q & 1 ? cos_kernel(r) : sin_kernel(r);
    return round(q & 2 ? neg(v) : v);
}
inline double cos_slow(double x) {
    unsigned q;
    const dd r = reduce(x, q);
    const dd v = q & 1 ? sin_kernel(r) : cos_kernel(r);
    return round((q + 1) & 2 ? neg(v) : v);
}
inline double tan_slow(double x) {
    unsigned q;
    const dd r = reduce(x, q);
    const dd s = sin_kernel(r), c = cos_kernel(r);
    return round(q & 1 ? neg(div(c, s)) : div(s, c));
}

// atan(t) for t in [0, 1]: three halvings, atan(t) = 2 atan(t / (1 + sqrt(1 + t^2))),
// bring t below tan(pi/32), then the series t - t^3/3 + t^5/5 ...
inline dd atan_kernel(dd t) {
    for (int k = 0; k < 3; ++k) t = div(t, add(dd{1.0, 0.0}, sqrt(add(dd{1.0, 0.0}, mul(t, t)))));
    const dd u = mul(t, t);
    dd p = div(dd{1.0, 0.0}, 37.0);
    for (int n = 17; n >= 0; --n) p = sub(div(dd{1.0, 0.0}, double(2 * n + 1)), mul(u, p));
    const dd a = mul(t, p);
    return {a.hi * 8.0, a.lo * 8.0};
}

// atan2(y, x) for finite, nonzero y and x.
inline double atan2_slow(double y, double x) {
    double ay = y < 0 ? -y : y, ax = x < 0 ? -x : x;
    const bool swap = ay > ax;
    auto expo = [](double v) { return int((std::bit_cast<uint64_t>(v) >> 52) & 0x7FF); };
    // |y/x| below 2^-60 with x > 0: atan(t) = t (1 - t^2/3 ...) is t to 120
    // bits, so the IEEE quotient, which is correctly rounded even into the
    // subnormal range, is the answer. (With x < 0 or |y| > |x| the result is
    // near pi or pi/2, where a tiny t doesn't reach the rounding point.)
    if (!swap && x > 0 && expo(ax) - expo(ay) > 60) return y / x;
    double num = swap ? ax : ay, den = swap ? ay : ax;
    // Scale both by the same power of two so den is near 1 (exact unless num
    // underflows, which only touches results far below the rounding point).
    const int k = int((std::bit_cast<uint64_t>(den) >> 52) & 0x7FF) - 1023;
    if (k > 500 || k < -500) {
        const double s = std::bit_cast<double>(uint64_t(1023 - (k > 0 ? 500 : -500)) << 52);
        num *= s;
        den *= s;
    }
    dd a = atan_kernel(div(dd{num, 0.0}, den));
    if (swap) a = sub(kPi2, a);
    if (x < 0) a = sub(kPi, a);
    return round(y < 0 ? neg(a) : a);
}

// log2(x) for finite x > 0: x = 2^e m with m in [sqrt(1/2), sqrt(2)], and
// ln m = 2 atanh(u), u = (m - 1) / (m + 1), |u| <= 0.172.
inline dd log2(double x) {
    uint64_t bits = std::bit_cast<uint64_t>(x);
    int e = 0;
    if ((bits >> 52) == 0) {  // subnormal: scale into the normal range (exact)
        bits = std::bit_cast<uint64_t>(x * 0x1p54);
        e = -54;
    }
    e += int(bits >> 52) - 1023;
    double m = std::bit_cast<double>((bits & ((1ull << 52) - 1)) | (1023ull << 52));
    if (m > 0x1.6a09e667f3bcdp+0) {  // above sqrt(2)
        m *= 0.5;
        ++e;
    }
    const dd u = div(dd{m - 1.0, 0.0}, two_sum(m, 1.0));  // m - 1 is exact (Sterbenz)
    const dd u2 = mul(u, u);
    dd p = div(dd{1.0, 0.0}, 47.0);
    for (int n = 22; n >= 0; --n) p = add(div(dd{1.0, 0.0}, double(2 * n + 1)), mul(u2, p));
    const dd ln = mul(u, mul(p, 2.0));
    return add(dd{double(e), 0.0}, mul(ln, kInvLn2));
}

// y * log2(x) for finite y != 0 and finite x > 0, x != 1, with one rounding.
inline double yl2x_slow(double y, double x) {
    const dd l = log2(x);
    const double ay = y < 0 ? -y : y;
    if (ay > 0x1p900) return round(mul(l, y * 0x1p-200)) * 0x1p200;  // keep Dekker's split in range
    if (ay < 0x1p-900) return round_scaled(mul(l, y * 0x1p200), 200);  // and exact for subnormal y
    return round(mul(l, y));
}

// ---- Fast paths (Ziv's strategy). Each evaluates with a relative error
// below 2^-64, mostly in plain doubles with double-double only for the
// leading terms, then checks that the error bound can't change the rounding.
// When it can't, the result is the correctly rounded double, so the same
// one the slow path returns. Otherwise (about 1 call in 300) the slow path
// runs. tools/check/x87 checks both against mpmath, and the fast path against
// the slow one on millions of inputs.
inline constexpr double kZiv = 0x1p-62;  // the error bound tested, with margin

inline bool rounded(dd r, double& out) {
    const double e = (r.hi < 0 ? -r.hi : r.hi) * kZiv;
    const double a = r.hi + (r.lo + e), b = r.hi + (r.lo - e);
    out = a;
    return a == b;
}
inline dd div2(dd a, dd b) {  // a / b to about 2^-104
    const double q1 = a.hi / b.hi;
    const dd r = sub(a, mul(b, q1));
    return fast_two_sum(q1, r.hi / b.hi);
}

// Cody-Waite reduction for |x| <= 2^20: pi/2 in four parts, the first two of
// 33 bits so k times them is exact. False if x is within 2^-20 of a multiple
// of pi/2, where the slow path's exact reduction is needed.
inline bool reduce_fast(double x, dd& r, unsigned& q) {
    const double ax = x < 0 ? -x : x;
    if (ax <= 0x1.921fb54442d18p-1) {
        r = {x, 0.0};
        q = 0;
        return true;
    }
    if (ax > 0x1p20) return false;
    const double k = (x * 0x1.45f306dc9c883p-1 + 0x1.8p52) - 0x1.8p52;  // nearest integer to x * 2/pi
    const double y0 = x - k * 0x1.921fb54400000p+0;                       // exact (Sterbenz)
    dd t = two_sum(y0, -(k * 0x1.0b4611a600000p-34));
    t = add(t, neg(two_prod(k, 0x1.3198a2e037073p-69)));
    t = add(t, dd{-(k * 0x1.129024e088a68p-123), 0.0});
    if ((t.hi < 0 ? -t.hi : t.hi) < 0x1p-20) return false;
    r = t;
    q = unsigned(int64_t(k)) & 3u;
    return true;
}

// sin r = r - r^3/6 + r^5/120 - ..., |r| <= pi/4: the first three terms in
// double-double, the rest (at most 2^-14 r) in double.
inline dd sin_fast(dd r) {
    const dd u = mul(r, r), r3 = mul(u, r), r5 = mul(r3, u);
    const double v = u.hi;
    double t = 0x1.3f3ccdd165fa9p-84;
    for (double c : {-0x1.761b41316381ap-75, 0x1.71b8ef6dcf572p-66, -0x1.2f49b46814157p-57, 0x1.952c77030ad4ap-49,
                     -0x1.ae7f3e733b81fp-41, 0x1.6124613a86d09p-33, -0x1.ae64567f544e4p-26, 0x1.71de3a556c734p-19,
                     -0x1.a01a01a01a01ap-13})
        t = t * v + c;
    const dd tail = add(mul(r5, dd{0x1.1111111111111p-7, 0x1.1111111111111p-63}), dd{r5.hi * v * t, 0.0});
    return add(r, add(mul(r3, dd{-0x1.5555555555555p-3, -0x1.5555555555555p-57}), tail));
}
// cos r = 1 - r^2/2 + r^4/24 - r^6/720 + ...: four terms in double-double.
inline dd cos_fast(dd r) {
    const dd u = mul(r, r), u2 = mul(u, u), u3 = mul(u2, u);
    const double v = u.hi;
    double t = -0x1.88e85fc6a4e5ap-89;
    for (double c : {0x1.f2cf01972f578p-80, -0x1.0ce396db7f853p-70, 0x1.e542ba4020225p-62, -0x1.6827863b97d97p-53,
                     0x1.ae7f3e733b81fp-45, -0x1.93974a8c07c9dp-37, 0x1.1eed8eff8d898p-29, -0x1.27e4fb7789f5cp-22,
                     0x1.a01a01a01a01ap-16})
        t = t * v + c;
    const dd tail = add(mul(u3, dd{-0x1.6c16c16c16c17p-10, 0x1.f49f49f49f49fp-65}), dd{u2.hi * u2.hi * t, 0.0});
    return add(dd{1.0, 0.0}, add(dd{-0.5 * u.hi, -0.5 * u.lo}, add(mul(u2, dd{0x1.5555555555555p-5, 0x1.5555555555555p-59}), tail)));
}

// x87 fsin, fcos and fptan for finite x, |x| < 2^63, x != 0. Below 2^-27,
// sin x and tan x round to x and cos x to 1.
inline double sin(double x) {
    if ((x < 0 ? -x : x) < 0x1p-27) return x;
    dd r;
    unsigned q;
    double out;
    if (reduce_fast(x, r, q)) {
        const dd v = q & 1 ? cos_fast(r) : sin_fast(r);
        if (rounded(q & 2 ? neg(v) : v, out)) return out;
    }
    QILIN_XM_FALLBACK(sin);
    return sin_slow(x);
}
inline double cos(double x) {
    if ((x < 0 ? -x : x) < 0x1p-27) return 1.0;
    dd r;
    unsigned q;
    double out;
    if (reduce_fast(x, r, q)) {
        const dd v = q & 1 ? sin_fast(r) : cos_fast(r);
        if (rounded((q + 1) & 2 ? neg(v) : v, out)) return out;
    }
    QILIN_XM_FALLBACK(cos);
    return cos_slow(x);
}
inline double tan(double x) {
    if ((x < 0 ? -x : x) < 0x1p-27) return x;
    dd r;
    unsigned q;
    double out;
    if (reduce_fast(x, r, q)) {
        const dd s = sin_fast(r), c = cos_fast(r);
        if (rounded(q & 1 ? neg(div2(c, s)) : div2(s, c), out)) return out;
    }
    QILIN_XM_FALLBACK(tan);
    return tan_slow(x);
}

// atan2 for finite, nonzero y and x: t = |y/x| or |x/y| in [0, 1], then
// atan t = atan(j/16) + atan v, v = (t - j/16) / (1 + t j/16), |v| <= 1/32.
inline constexpr dd kAtanTable[17] = {
    dd{0x0.0p+0, 0x0.0p+0},
    dd{0x1.ff55bb72cfdeap-5, -0x1.c934d86d23f1dp-60},
    dd{0x1.fd5ba9aac2f6ep-4, -0x1.cd37686760c17p-59},
    dd{0x1.7b97b4bce5b02p-3, 0x1.347b0b4f881cap-58},
    dd{0x1.f5b75f92c80ddp-3, 0x1.8ab6e3cf7afbdp-57},
    dd{0x1.362773707ebccp-2, -0x1.963a544b672d8p-57},
    dd{0x1.6f61941e4def1p-2, -0x1.c63aae6f6e918p-56},
    dd{0x1.a64eec3cc23fdp-2, -0x1.24dec1b50b7ffp-56},
    dd{0x1.dac670561bb4fp-2, 0x1.a2b7f222f65e2p-56},
    dd{0x1.0657e94db30d0p-1, -0x1.d5b495f6349e6p-56},
    dd{0x1.1e00babdefeb4p-1, -0x1.928df287a668fp-58},
    dd{0x1.345f01cce37bbp-1, 0x1.1021137c71102p-55},
    dd{0x1.4978fa3269ee1p-1, 0x1.2419a87f2a458p-56},
    dd{0x1.5d58987169b18p-1, 0x1.0028e4bc5e7cap-57},
    dd{0x1.700a7c5784634p-1, -0x1.8c34d25aadef6p-56},
    dd{0x1.819d0b7158a4dp-1, -0x1.bf76229d3b917p-56},
    dd{0x1.921fb54442d18p-1, 0x1.1a62633145c07p-55},
};
inline double atan2(double y, double x) {
    const double ay = y < 0 ? -y : y, ax = x < 0 ? -x : x;
    const bool swap = ay > ax;
    const double num = swap ? ax : ay, den = swap ? ay : ax;
    if (num >= 0x1p-500 && den <= 0x1p500 && num >= den * 0x1p-60) {
        const double q1 = num / den;
        const dd p = two_prod(q1, den);
        const dd t = fast_two_sum(q1, ((num - p.hi) - p.lo) / den);
        const int j = int(t.hi * 16.0 + 0.5);
        const double c = j * 0.0625;
        const dd v = div2(fast_two_sum(t.hi - c, t.lo), add(dd{1.0, 0.0}, mul(t, c)));  // t.hi - c is exact
        const double w = v.hi * v.hi;
        double s = 0x1.e1e1e1e1e1e1ep-5;
        for (double k : {-0x1.1111111111111p-4, 0x1.3b13b13b13b14p-4, -0x1.745d1745d1746p-4, 0x1.c71c71c71c71cp-4,
                         -0x1.2492492492492p-3, 0x1.999999999999ap-3})
            s = s * w + k;
        const dd v3 = mul(mul(v, v), v);
        const dd at = add(v, add(mul(v3, dd{-0x1.5555555555555p-2, -0x1.5555555555555p-56}), dd{v3.hi * w * s, 0.0}));
        dd a = add(kAtanTable[j], at);
        if (swap) a = sub(kPi2, a);
        if (x < 0) a = sub(kPi, a);
        double out;
        if (rounded(y < 0 ? neg(a) : a, out)) return out;
    }
    QILIN_XM_FALLBACK(atan2);
    return atan2_slow(y, x);
}

// y log2 x for finite y != 0 and finite x > 0, x != 1: ln m / 2 = atanh u =
// u + u^3/3 + u^5/5 + ..., |u| <= 0.172, three terms in double-double.
inline double yl2x(double y, double x) {
    const double ay = y < 0 ? -y : y;
    const uint64_t bits = std::bit_cast<uint64_t>(x);
    if (ay >= 0x1p-800 && ay <= 0x1p800 && (bits >> 52) != 0) {
        int e = int(bits >> 52) - 1023;
        double m = std::bit_cast<double>((bits & ((1ull << 52) - 1)) | (1023ull << 52));
        if (m > 0x1.6a09e667f3bcdp+0) {
            m *= 0.5;
            ++e;
        }
        const dd den = two_sum(m, 1.0);
        const double num = m - 1.0;  // exact
        const double u1 = num / den.hi;
        const dd p = two_prod(u1, den.hi);
        const dd u = fast_two_sum(u1, (((num - p.hi) - p.lo) - u1 * den.lo) / den.hi);
        const dd u2 = mul(u, u), u3 = mul(u2, u), u5 = mul(u3, u2);
        const double w = u2.hi;
        double s = 0x1.0842108421084p-5;
        for (double k : {0x1.1a7b9611a7b96p-5, 0x1.2f684bda12f68p-5, 0x1.47ae147ae147bp-5, 0x1.642c8590b2164p-5,
                         0x1.8618618618618p-5, 0x1.af286bca1af28p-5, 0x1.e1e1e1e1e1e1ep-5, 0x1.1111111111111p-4,
                         0x1.3b13b13b13b14p-4, 0x1.745d1745d1746p-4, 0x1.c71c71c71c71cp-4, 0x1.2492492492492p-3})
            s = s * w + k;
        const dd tail = add(mul(u5, dd{0x1.999999999999ap-3, -0x1.999999999999ap-57}), dd{u5.hi * w * s, 0.0});
        const dd half_ln = add(u, add(mul(u3, dd{0x1.5555555555555p-2, 0x1.5555555555555p-56}), tail));
        const dd l = add(dd{double(e), 0.0}, mul(half_ln, dd{2.0 * kInvLn2.hi, 2.0 * kInvLn2.lo}));
        double out;
        if (rounded(mul(l, y), out)) return out;
    }
    QILIN_XM_FALLBACK(yl2x);
    return yl2x_slow(y, x);
}


// ---- 2^x - 1 (f2xm1). x = n + j/32 + r with |r| <= 1/64, so
// 2^x - 1 = 2^n (1 + T_j)(1 + E) - 1 with T_j = 2^(j/32) - 1 from the table
// and E = expm1(r ln 2), |r ln 2| <= 0.0109. Defined for every finite x
// (the x87 only defines |x| <= 1; the result is the same there).
inline constexpr dd kLn2{0x1.62e42fefa39efp-1, 0x1.abc9e3b39803fp-56};
inline constexpr dd kExp2m1Table[32] = {
    dd{0.0, 0.0},
    dd{0x1.66c34c5615d0fp-6, -0x1.183ab7149735cp-60},
    dd{0x1.6ab0d9f3121ecp-5, 0x1.4c5c95b8c2155p-59},
    dd{0x1.1301d0125b50ap-4, 0x1.3aefc6bb64c63p-58},
    dd{0x1.72b83c7d517aep-4, -0x1.9041b9d78a75bp-59},
    dd{0x1.d4873168b9aa8p-4, -0x1.fe91ff5d9bc3ep-58},
    dd{0x1.1c3d373ab11c3p-3, 0x1.b07eb6c70572dp-58},
    dd{0x1.4f4efa8fef709p-3, 0x1.84ba2beb44954p-57},
    dd{0x1.837f0518db8a9p-3, 0x1.bd1ab48c60b91p-57},
    dd{0x1.b8d39b9d54e55p-3, 0x1.c51540bd151e6p-58},
    dd{0x1.ef5326091a112p-3, -0x1.497dbb83d8512p-57},
    dd{0x1.13821818624b4p-2, 0x1.89b7a04ef80d0p-59},
    dd{0x1.2ff6b54d8a89cp-2, 0x1.d4397afec42e2p-56},
    dd{0x1.4d0ad5a753e07p-2, 0x1.f0a83c49d86a6p-56},
    dd{0x1.6ac1f752150a5p-2, 0x1.8c93015191eb3p-56},
    dd{0x1.891fac0e95613p-2, -0x1.c1e0bf205a4b8p-57},
    dd{0x1.a827999fcef32p-2, 0x1.08b2fb1366ea9p-56},
    dd{0x1.c7dd7a3b17dcfp-2, 0x1.d2370f2ef0acdp-56},
    dd{0x1.e8451cfac061bp-2, 0x1.7d51023f6cda2p-56},
    dd{0x1.04b1332999c25p-1, 0x1.59f115f566940p-57},
    dd{0x1.159ca845541b7p-1, -0x1.22c1d52f369b2p-55},
    dd{0x1.26e6f619b8bcap-1, -0x1.75fc781b57ebcp-57},
    dd{0x1.389230547e120p-1, 0x1.c7c46b071f2bep-56},
    dd{0x1.4aa07647c4ab9p-1, 0x1.684892395f0f8p-57},
    dd{0x1.5d13f32b5a75bp-1, -0x1.0bc65974466fdp-55},
    dd{0x1.6feede5f6bc8ep-1, -0x1.5584f7e54ac3bp-56},
    dd{0x1.83337bb0aa538p-1, 0x1.11065895048ddp-55},
    dd{0x1.96e41b9df20d2p-1, 0x1.503cbd1e949dbp-56},
    dd{0x1.ab031b9f7490ep-1, 0x1.2ed02d75b3707p-55},
    dd{0x1.bf92e66f736bdp-1, 0x1.cb46561cf6949p-55},
    dd{0x1.d495f454921b3p-1, 0x1.63dce863d76ccp-58},
    dd{0x1.ea0ecb6dc8a81p-1, -0x1.8b07b489d79d4p-56}
};
// expm1(s) for |s| <= 0.011: Taylor to s^15/15! (about 2^-104 relative).
inline constexpr dd kInvFact[14] = {  // 1/n!, n = 2..15 (mpmath)
    {0x1.0p-1, 0.0},
    {0x1.5555555555555p-3, 0x1.5555555555555p-57},
    {0x1.5555555555555p-5, 0x1.5555555555555p-59},
    {0x1.1111111111111p-7, 0x1.1111111111111p-63},
    {0x1.6c16c16c16c17p-10, -0x1.f49f49f49f49fp-65},
    {0x1.a01a01a01a01ap-13, 0x1.a01a01a01a01ap-73},
    {0x1.a01a01a01a01ap-16, 0x1.a01a01a01a01ap-76},
    {0x1.71de3a556c734p-19, -0x1.c154f8ddc6c00p-73},
    {0x1.27e4fb7789f5cp-22, 0x1.cbbc05b4fa99ap-76},
    {0x1.ae64567f544e4p-26, -0x1.c062e06d1f209p-80},
    {0x1.1eed8eff8d898p-29, -0x1.2aec959e14c06p-83},
    {0x1.6124613a86d09p-33, 0x1.f28e0cc748ebep-87},
    {0x1.93974a8c07c9dp-37, 0x1.05d6f8a2efd1fp-92},
    {0x1.ae7f3e733b81fp-41, 0x1.1d8656b0ee8cbp-97},
};
inline dd expm1_slow_kernel(dd s) {
    dd p = kInvFact[13];
    for (int k = 12; k >= 0; --k) p = add(kInvFact[k], mul(s, p));
    return add(s, mul(mul(s, s), p));
}
// The same to about 2^-67: s + s^2/2 in double-double, the rest (to s^9/9!) in doubles.
inline dd expm1_fast_kernel(dd s) {
    const double h = s.hi;
    const double q = 0x1.5555555555555p-3 +
                     h * (0x1.5555555555555p-5 + h * (0x1.1111111111111p-7 + h * (0x1.6c16c16c16c17p-10 +
                     h * (0x1.a01a01a01a01ap-13 + h * (0x1.a01a01a01a01ap-16 + h * 0x1.71de3a556c734p-19)))));
    const dd s2 = mul(s, s);
    return add(s, add(dd{0.5 * s2.hi, 0.5 * s2.lo}, dd{s2.hi * h * q, 0.0}));
}
// 2^x - 1 as double-double from the kernel, for -60 <= x < 1024 with |x| >= 2^-60.
// When n == 1024 the value is halved (and then rounded and doubled by the caller).
template <dd (*Kernel)(dd)>
inline dd exp2m1_dd(double x, bool& halved) {
    const double k = std::nearbyint(x * 32.0);  // x * 32 is exact
    const double r = x - k * 0x1p-5;           // exact (Sterbenz, or k == 0)
    const int ki = int(k);
    const int n = ki >> 5, j = ki & 31;        // floor division: n = floor(k / 32)
    const dd em = Kernel(mul(kLn2, r));
    const dd t = kExp2m1Table[j];
    const dd pm1 = add(t, add(em, mul(t, em)));  // (1 + T_j)(1 + E) - 1
    halved = false;
    if (n == 0) return pm1;
    if (n == 1024) {
        halved = true;
        return sub(scale(add(dd{1.0, 0.0}, pm1), pow2(1023)), dd{0.5, 0.0});
    }
    return sub(scale(add(dd{1.0, 0.0}, pm1), pow2(n)), dd{1.0, 0.0});
}
// Tiny |x| < 2^-60: s + s^2/2 with s = x ln 2 decides the rounding.
inline double exp2m1_tiny(double x) {
    const double ax = x < 0 ? -x : x;
    if (ax < 0x1p-900) return round_scaled(mul(kLn2, x * 0x1p200), 200);  // s^2 is far below the last bit
    const dd s = mul(kLn2, x);
    return round(add(s, dd{0.5 * s.hi * s.hi, 0.0}));
}
inline double exp2m1_slow(double x) {
    bool halved;
    const double v = round(exp2m1_dd<expm1_slow_kernel>(x, halved));
    return halved ? 2.0 * v : v;
}
// For finite x != 0.
inline double exp2m1(double x) {
    if (x >= 1024.0) return std::bit_cast<double>(0x7FF0000000000000ull);
    if (x < -60.0) return -1.0;  // 2^x < 2^-60: -1 + 2^x rounds to -1
    const double ax = x < 0 ? -x : x;
    if (ax < 0x1p-60) return exp2m1_tiny(x);
    bool halved;
    double out;
    if (rounded(exp2m1_dd<expm1_fast_kernel>(x, halved), out)) return halved ? 2.0 * out : out;
    QILIN_XM_FALLBACK(exp2m1);
    return exp2m1_slow(x);
}

}  // namespace qilin::rt::xm
