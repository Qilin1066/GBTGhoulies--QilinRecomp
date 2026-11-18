// Reads `op a b` lines (op: + - * / s; a, b: double bit patterns in hex) and
// prints rt::fadd/fsub/fmul/fdiv/fsqrt at 24-bit precision control. Used by
// rounding.py.
#include <cstdio>
#include <cstdlib>
#include <cinttypes>
#include "guest.hpp"
namespace qilin::rt { [[noreturn]] void trap(uint32_t, const char*) { std::abort(); } }
using namespace qilin::rt;
int main() {
    char op; uint64_t a, b;
    while (std::scanf(" %c %" SCNx64 " %" SCNx64, &op, &a, &b) == 3) {
        const double x = dfrom(a), y = dfrom(b);
        double r = 0;
        switch (op) { case '+': r = fadd(0x7F, x, y); break; case '-': r = fsub(0x7F, x, y); break;
                      case '*': r = fmul(0x7F, x, y); break; case '/': r = fdiv(0x7F, x, y); break; case 's': r = fsqrt(0x7F, x); break; }
        std::printf("%016" PRIx64 "\n", dbits(r));
    }
}
