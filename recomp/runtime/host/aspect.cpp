// The game's picture shape. Ghoulies draws for a 4:3 or a 16:9 TV (the
// console's widescreen flag), always into its 640×480 frame: a 16:9 TV
// stretches the frame back out. Its cameras take their shape from a table of
// three "film" formats at 0x42A460, each {aspect, film width, default focal
// length}: 4:3, 16:9 and 2.35:1 (the letterboxed cut scenes). A camera's
// horizontal field of view is 2·atan(film width / 2·focal length)
// (0xF57F0), its viewport is the display format's frame cut to its own
// format (0xF56C0, 0xF5760: the 4:3 comic pages are pillarboxed that way),
// and the format in use is [0x5077F8], 0 or 1 from the widescreen flag
// [0x509703] (0x3321D, 0x58252).
//
// Any other shape is the 16:9 format made wider or narrower: its aspect set
// to the new one and its film width scaled with it, so every camera keeps
// 16:9's vertical view and sees more (or less) at the sides; the focal
// lengths, and the cut scenes' 2.35:1 framing, stay as they are. 4:3 is the
// console's own 4:3 setting.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "host.hpp"

namespace qilin::host {
namespace {

constexpr uint32_t kFormats = 0x42A460;          // {aspect, film width, focal length} × 3
constexpr uint32_t kWide = kFormats + 12;        // the 16:9 format
constexpr uint32_t kWidescreenFlag = 0x509703;   // byte: the console's widescreen setting, as the game read it
constexpr uint32_t kFormatInUse = 0x5077F8;      // 0 (4:3) or 1 (16:9); 0 for a moment around the comic pages
constexpr float k43 = 4.0f / 3.0f;

float ldf(const rt::Mem& m, uint32_t a) {
    const uint32_t v = rt::ld32(m, a, 0);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
void stf(const rt::Mem& m, uint32_t a, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    rt::st32(m, a, v, 0);
}

float g_wide_aspect = 0, g_wide_film = 0;  // the image's 16:9 format, read once
float g_applied = 0;                        // the shape the game was last given

}  // namespace

float game_aspect_ratio(float requested) {
    if (!(requested > 0)) return 16.0f / 9.0f;
    return std::clamp(requested, k43, kMaxGameAspect);
}

void set_game_aspect(const rt::Mem& m, float aspect, bool running) {
    aspect = game_aspect_ratio(aspect);
    if (!g_wide_aspect) {
        g_wide_aspect = ldf(m, kWide);
        g_wide_film = ldf(m, kWide + 4);
    }
    const bool wide = std::fabs(aspect - k43) > 1e-3f;
    if (running && g_applied == aspect) return;
    const bool was_wide = running && rt::ld8(m, kWidescreenFlag, 0) != 0;
    stf(m, kWide, wide ? aspect : g_wide_aspect);
    stf(m, kWide + 4, wide ? g_wide_film * aspect / g_wide_aspect : g_wide_film);
    set_console_widescreen(wide);
    if (running) {
        // The game read the flag at start; tell it again, and switch the
        // format in use unless the comic pages hold it at 4:3 this moment
        // (they set it back from the flag when they're done).
        rt::st8(m, kWidescreenFlag, wide ? 1 : 0, 0);
        if (rt::ld32(m, kFormatInUse, 0) == (was_wide ? 1u : 0u)) rt::st32(m, kFormatInUse, wide ? 1 : 0, 0);
    }
    g_applied = aspect;
}

}  // namespace qilin::host
