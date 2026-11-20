// Native replacements for game functions with latent bugs that frame rates
// above the console's bring out. Each is the original, instruction for
// instruction, but for the fix it documents; the address is in the retail
// US XBE, and qilin lift's kNative list keeps the original from being lifted.
#include <bit>
#include <cstdint>

#include "host.hpp"

namespace qilin::host {
namespace {

constexpr uint32_t kTimeStep = 0x5096C0;  // the frame's time step in seconds (the frame clock's)

float ldf(const rt::Mem& m, uint32_t a) { return std::bit_cast<float>(rt::ld32(m, a, 0)); }
void stf(const rt::Mem& m, uint32_t a, float v) { rt::st32(m, a, std::bit_cast<uint32_t>(v), 0); }

// A screen transition's countdown, as the originals run it: while the timer
// (+8) is above zero the time step comes off it, and the transition moves on
// only once it's below zero. A timer that lands on exactly zero is neither,
// and stays there for good. The console's time step is always a whole
// number of vblanks, two at least, and the timers it meets never do; a
// step that equals what's left does (the transition opened at 0x12BD2C
// starts at 1/60, so a first step of 1/60 left the game on its loading
// screen). Landing on zero counts as just past it here, which is what a step
// a hair larger would give; nothing else changes.
float count_down(const rt::Mem& m, uint32_t obj) {
    float t = ldf(m, obj + 8);
    if (t > 0.0f) {
        t -= ldf(m, kTimeStep);
        if (t == 0.0f) t = -1e-20f;
        stf(m, obj + 8, t);
    }
    return t;
}

// The transition's finish (0x12C830: ecx the object, edx 0), with esi the
// object as the original's caller has it.
void finish(rt::Context& c, uint32_t obj) {
    const uint32_t esi = c.esi;
    c.ecx = obj, c.edx = 0, c.esi = obj;
    call_guest(c, 0x12C830, {});
    c.esi = esi;
}

// int __cdecl update(object*) at 0x12BC80 (the first screen type's): state
// 1 (fading in) becomes 2 once the timer is below zero, clearing the flag at
// 0x510F80; state 3 (fading out) then finishes. Returns the state.
void screen_update_a(rt::Context& c) {
    const rt::Mem m{c.mem, c.mem_size};
    const uint32_t obj = arg(c, 0);
    const float t = count_down(m, obj);
    const uint32_t state = rt::ld32(m, obj + 4, 0);
    if (state == 1 && 0.0f > t) {
        rt::st32(m, 0x510F80, 0, 0);
        rt::st32(m, obj + 4, 2, 0);
    } else if (state == 3 && 0.0f > t) {
        finish(c, obj);
    }
    ret(c, rt::ld32(m, obj + 4, 0), 0);
}

// int __cdecl update(object*) at 0x12BDD0 (the third and fourth types'):
// state 1 becomes 2 once the timer is at zero or below, clearing 0x510F80;
// state 3 finishes once it's below zero, and before that plays a sound
// (0x39510(0x431370, 1, 0)) as the timer passes 0.8 of its start (+0xC).
void screen_update_b(rt::Context& c) {
    const rt::Mem m{c.mem, c.mem_size};
    const uint32_t obj = arg(c, 0);
    const float before = ldf(m, obj + 8);
    const float t = count_down(m, obj);
    const uint32_t state = rt::ld32(m, obj + 4, 0);
    if (state == 1) {
        if (!(0.0f < t)) {
            rt::st32(m, 0x510F80, 0, 0);
            rt::st32(m, obj + 4, 2, 0);
        }
    } else if (state == 3) {
        if (0.0f > t) {
            finish(c, obj);
        } else {
            const float mark = ldf(m, obj + 12) * ldf(m, 0x32B244);
            if (before > mark && mark >= t) call_guest(c, 0x39510, {0x431370, 1, 0});
        }
    }
    ret(c, rt::ld32(m, obj + 4, 0), 0);
}

}  // namespace

void register_game_fixes() {
    register_hle(0x12BC80, "a screen transition's update (first type)", screen_update_a);
    register_hle(0x12BDD0, "a screen transition's update (third and fourth types)", screen_update_b);
}

}  // namespace qilin::host
