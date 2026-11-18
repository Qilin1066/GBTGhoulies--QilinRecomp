// The x87 transcendentals' fast paths against their slow paths, on millions
// of random operands: wherever the fast path answers, it must give the slow
// path's bits. Also prints how often the fast path hands over to the slow one.
// Built as the qilin_x87_consistency test (200,000 cases per range); run it
// with a larger count by hand: qilin_x87_consistency [cases per range].
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>

static long g_fallbacks = 0;
#define QILIN_XM_FALLBACK(fn) ++g_fallbacks
#include "x87_math.hpp"

using namespace qilin::rt;

int main(int argc, char** argv) {
    const long n = argc > 1 ? std::atol(argv[1]) : 2000000;
    std::mt19937_64 rng(12345);
    auto uni = [&](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng); };
    auto logu = [&](double lo, double hi) { return (rng() & 1 ? -1.0 : 1.0) * std::exp2(uni(lo, hi)); };
    struct Range {
        const char* name;
        double (*fast)(double, double);
        double (*slow)(double, double);
        std::function<std::pair<double, double>()> gen;
    };
    const Range ranges[] = {
        {"sin |x| < 8", [](double x, double) { return xm::sin(x); }, [](double x, double) { return xm::sin_slow(x); },
         [&] { return std::pair{uni(-8, 8), 0.0}; }},
        {"sin 2^-27..2^20", [](double x, double) { return xm::sin(x); }, [](double x, double) { return xm::sin_slow(x); },
         [&] { return std::pair{logu(-27, 20), 0.0}; }},
        {"cos |x| < 8", [](double x, double) { return xm::cos(x); }, [](double x, double) { return xm::cos_slow(x); },
         [&] { return std::pair{uni(-8, 8), 0.0}; }},
        {"cos 2^-27..2^20", [](double x, double) { return xm::cos(x); }, [](double x, double) { return xm::cos_slow(x); },
         [&] { return std::pair{logu(-27, 20), 0.0}; }},
        {"tan |x| < 8", [](double x, double) { return xm::tan(x); }, [](double x, double) { return xm::tan_slow(x); },
         [&] { return std::pair{uni(-8, 8), 0.0}; }},
        {"atan2 2^-20..2^20", [](double y, double x) { return xm::atan2(y, x); },
         [](double y, double x) { return xm::atan2_slow(y, x); }, [&] { return std::pair{logu(-20, 20), logu(-20, 20)}; }},
        {"yl2x x 2^-30..2^30", [](double y, double x) { return xm::yl2x(y, x); },
         [](double y, double x) { return xm::yl2x_slow(y, x); }, [&] { return std::pair{logu(-20, 20), std::exp2(uni(-30, 30))}; }},
        {"yl2x x near 1", [](double y, double x) { return xm::yl2x(y, x); }, [](double y, double x) { return xm::yl2x_slow(y, x); },
         [&] { return std::pair{logu(-20, 20), 1.0 + (rng() & 1 ? -1 : 1) * std::exp2(uni(-52, -1))}; }},
        {"2^x-1 |x| <= 1", [](double x, double) { return xm::exp2m1(x); }, [](double x, double) { return xm::exp2m1_slow(x); },
         [&] { return std::pair{uni(-1, 1), 0.0}; }},
        {"2^x-1 2^-60..1", [](double x, double) { return xm::exp2m1(x); }, [](double x, double) { return xm::exp2m1_slow(x); },
         [&] { return std::pair{logu(-59.9, 0), 0.0}; }},
        {"2^x-1 -60..1024", [](double x, double) { return xm::exp2m1(x); }, [](double x, double) { return xm::exp2m1_slow(x); },
         [&] { return std::pair{uni(-60, 1023.99), 0.0}; }},
    };
    long bad = 0;
    std::printf("%-22s %10s %10s %12s\n", "operands", "cases", "differ", "fallbacks");
    for (const Range& r : ranges) {
        long differ = 0;
        g_fallbacks = 0;
        for (long i = 0; i < n; ++i) {
            const auto [a, b] = r.gen();
            if (a == 1.0 && b == 1.0) continue;
            const double f = r.fast(a, b), s = r.slow(a, b);
            if (std::bit_cast<uint64_t>(f) != std::bit_cast<uint64_t>(s) && differ++ < 5)
                std::printf("  DIFFER %s(%a, %a): fast %a, slow %a\n", r.name, a, b, f, s);
        }
        std::printf("%-22s %10ld %10ld %11.3f%%\n", r.name, n, differ, 100.0 * double(g_fallbacks) / double(n));
        bad += differ;
    }
    std::printf("differences: %ld\n", bad);
    return bad != 0;
}
