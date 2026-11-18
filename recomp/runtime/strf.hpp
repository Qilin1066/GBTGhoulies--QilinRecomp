// printf-style formatting into a std::string, for the runtime (what players
// run). The runtime doesn't use std::format: Apple's libc++ makes it require
// macOS 13.3, and the runtime is built for macOS 10.15 (docs/roadmap...md,
// "Hardware scope"). The recompiler's tools, which only run on developers'
// machines, use std::format freely.
#pragma once

#include <cstdarg>
#include <cstdio>
#include <string>

namespace qilin {

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
inline std::string strf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    const int n = std::vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    std::string out(n > 0 ? size_t(n) : 0, '\0');
    if (n > 0) std::vsnprintf(out.data(), out.size() + 1, fmt, ap2);
    va_end(ap2);
    return out;
}

}  // namespace qilin
