// Ghoulies' .text regions (docs/code-analysis.md): which are lifted, and the
// names reports use. Shared by the CFG (coverage) and the runtime (reports).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace qilin {

struct TextRegion {
    std::string name;
    uint32_t lo, hi;
    bool lifted;  // lifted as a whole region (the CRT and XAPI are lifted function by function)
};
inline const std::vector<TextRegion> kTextRegions = {
    {"game", 0x11000, 0x14A840, true},       {"xapi", 0x14A840, 0x152780, false},
    {"rare-libs", 0x152780, 0x165C00, true}, {"crt", 0x165C00, 0x1707B0, false},
    {"eh-funclets", 0x1707B0, 0x171020, true},
};

}  // namespace qilin
