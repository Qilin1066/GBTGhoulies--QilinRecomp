// Reads `op a b` lines (a, b: double bit patterns in hex) and prints the result:
//   + - * / s   rt::fadd/fsub/fmul/fdiv/fsqrt at 24-bit precision control (rounding.py)
//   S C T       rt::fsin/fcos/ftan of a                          (transcendental.py)
//   A L         rt::fpatan(a, b) = atan2(a, b), rt::fyl2x(a, b) = a * log2(b)
//   F           rt::f2xm1(a) = 2^a - 1
// For the transcendentals it also prints the host libm's answer.
#include <cstdio>
#include <cstdlib>
#include <cinttypes>
#include <cmath>
#include "guest.hpp"
namespace qilin::rt { [[noreturn]] void trap(uint32_t, const char*) { std::abort(); } }
using namespace qilin::rt;
int main() {
    char op; uint64_t a, b;
    while (std::scanf(" %c %" SCNx64 " %" SCNx64, &op, &a, &b) == 3) {
        const double x = dfrom(a), y = dfrom(b);
        double r = 0, h = 0;
        switch (op) { case '+': r = fadd(0x7F, x, y); break; case '-': r = fsub(0x7F, x, y); break;
                      case '*': r = fmul(0x7F, x, y); break; case '/': r = fdiv(0x7F, x, y); break; case 's': r = fsqrt(0x7F, x); break;
                      case 'S': r = fsin(x); h = std::sin(x); break; case 'C': r = fcos(x); h = std::cos(x); break;
                      case 'T': r = ftan(x); h = std::tan(x); break; case 'A': r = fpatan(x, y); h = std::atan2(x, y); break;
                      case 'L': r = fyl2x(x, y); h = x * std::log2(y); break;
                      case 'F': r = f2xm1(x); h = std::exp2(x) - 1.0; break; }
        if (op >= 'A' && op <= 'Z') std::printf("%016" PRIx64 " %016" PRIx64 "\n", dbits(r), dbits(h));
        else std::printf("%016" PRIx64 "\n", dbits(r));
    }
}
