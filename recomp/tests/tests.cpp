// Self-contained tests: build a tiny synthetic XBE in memory and run the
// loader, decoder, CFG and lifter over it, and exercise the runtime helpers.
// No game files required (runs in CI).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include "cfg.hpp"
#include "decoder.hpp"
#include "guest.hpp"
#include "json.hpp"
#include "lifter.hpp"
#include "xbe.hpp"

using namespace qilin;

namespace {

int failures = 0;
#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::println(stderr, "{}:{}: CHECK failed: {}", __FILE__, __LINE__, #cond); \
            ++failures;                                                           \
        }                                                                         \
    } while (0)

void put32(std::vector<uint8_t>& d, size_t o, uint32_t v) { std::memcpy(&d[o], &v, 4); }

constexpr uint32_t kBase = 0x10000;

std::vector<uint8_t> make_xbe() {
    std::vector<uint8_t> d(0x1600, 0);
    std::memcpy(d.data(), "XBEH", 4);
    put32(d, 0x104, kBase);
    put32(d, 0x108, 0x1000);        // size of headers
    put32(d, 0x10C, 0x3000);        // size of image
    put32(d, 0x118, kBase + 0x400); // certificate
    put32(d, 0x11C, 2);             // sections
    put32(d, 0x120, kBase + 0x200); // section headers
    put32(d, 0x128, 0x11000 ^ 0xA8FC57AB);  // entry (retail key)
    put32(d, 0x158, 0x12000 ^ 0x5B6D40B6);  // kernel thunk (retail key)
    auto sect = [&](int i, uint32_t flags, uint32_t va, uint32_t size, uint32_t raw, uint32_t name) {
        const size_t o = 0x200 + size_t(i) * 56;
        put32(d, o + 0, flags);
        put32(d, o + 4, va);
        put32(d, o + 8, size);
        put32(d, o + 12, raw);
        put32(d, o + 16, size);
        put32(d, o + 20, kBase + name);
    };
    sect(0, 6, 0x11000, 0x400, 0x1000, 0x300);
    sect(1, 2, 0x12000, 0x100, 0x1400, 0x308);
    std::memcpy(&d[0x300], ".text", 6);
    std::memcpy(&d[0x308], ".rdata", 7);
    put32(d, 0x408, 0x4D530053);
    const char16_t title[] = u"T\U0001F600";  // 'T' + a surrogate pair
    std::memcpy(&d[0x40C], title, sizeof title);
    put32(d, 0x1400, 0x80000001);  // kernel thunk table: ordinal 1, terminated

    const uint8_t code[] = {
        // 11000 entry: calls the three test functions
        0xE8, 0x0B, 0x00, 0x00, 0x00,              // 11000 call 11010
        0xE8, 0xF6, 0x00, 0x00, 0x00,              // 11005 call 11100
        0xE8, 0x21, 0x01, 0x00, 0x00,              // 1100A call 11130
        0xC3,                                      // 1100F ret
        // 11010 switch on a dword index, bounded by cmp eax, 2
        0x8B, 0x44, 0x24, 0x04,                    // 11010 mov eax, [esp+4]
        0x83, 0xF8, 0x02,                          // 11014 cmp eax, 2
        0x77, 0x0D,                                // 11017 ja 11026
        0xFF, 0x24, 0x85, 0x40, 0x10, 0x01, 0x00,  // 11019 jmp [4*eax + 0x11040]
        0x31, 0xC0, 0xC3,                          // 11020 xor eax, eax; ret
        0xB0, 0x01, 0xC3,                          // 11023 mov al, 1; ret
        0x83, 0xC8, 0xFF, 0xC3,                    // 11026 or eax, -1; ret
    };
    std::memcpy(&d[0x1000], code, sizeof code);
    put32(d, 0x1040, 0x11020);
    put32(d, 0x1044, 0x11023);
    put32(d, 0x1048, 0x11026);

    // 11100: switch through a byte index table, bounded by `cmp al, 0x83`
    // (an imm8 the decoder sign-extends: the bound must be taken as 0x83).
    const uint8_t sw2[] = {
        0x8A, 0x44, 0x24, 0x04,                    // 11100 mov al, [esp+4]
        0x3C, 0x83,                                // 11104 cmp al, 0x83
        0x77, 0x1A,                                // 11106 ja 11122
        0x0F, 0xB6, 0xC0,                          // 11108 movzx eax, al
        0x0F, 0xB6, 0x88, 0x00, 0x12, 0x01, 0x00,  // 1110B movzx ecx, byte [eax + 0x11200]
        0xFF, 0x24, 0x8D, 0x90, 0x12, 0x01, 0x00,  // 11112 jmp [4*ecx + 0x11290]
        0x31, 0xC0, 0xC3,                          // 11119 xor eax, eax; ret
        0xB8, 0x01, 0x00, 0x00, 0x00,              // 1111C mov eax, 1
        0xC3,                                      // 11121 ret
        0x83, 0xC8, 0xFF, 0xC3,                    // 11122 or eax, -1; ret
    };
    std::memcpy(&d[0x1100], sw2, sizeof sw2);
    for (int k = 0; k < 0x84; ++k) d[0x1200 + size_t(k)] = uint8_t(k & 1);  // byte index table
    put32(d, 0x1290, 0x11119);  // dword table after the 0x84-byte index table
    put32(d, 0x1294, 0x1111C);

    // 11130: an epilogue fragment (pops its own return address)
    d[0x1130] = 0x5B;  // pop ebx
    d[0x1131] = 0xC3;  // ret

    std::memset(&d[0x1298], 0xCC, 0x1400 - 0x1298);  // int3 padding between functions
    // 112A0: reached only through a data pointer. A 16-aligned loop head at
    // 112B0 behind `jmp short` over MSVC alignment filler: a label, not a function.
    const uint8_t loop[] = {
        0x85, 0xC9,                    // 112A0 test ecx, ecx
        0x74, 0x0C,                    // 112A2 jz 112B0
        0xB8, 0x01, 0x00, 0x00, 0x00,  // 112A4 mov eax, 1
        0xEB, 0x05,                    // 112A9 jmp 112B0
        0x8D, 0x49, 0x00,              // 112AB lea ecx, [ecx+0]   (filler)
        0x8B, 0xFF,                    // 112AE mov edi, edi        (filler)
        0x49,                          // 112B0 dec ecx
        0x75, 0xFD,                    // 112B1 jnz 112B0
        0xC3,                          // 112B3 ret
    };
    std::memcpy(&d[0x12A0], loop, sizeof loop);
    put32(d, 0x1410, 0x112A0);  // .rdata: the pointer

    // C++ EH: FuncInfo at 12040 with one unwind action (funclet 112E0), and the
    // __ehhandler stub at 112D0: mov eax, &FuncInfo; jmp __CxxFrameHandler.
    put32(d, 0x1440, 0x19930520);  // magic
    put32(d, 0x1444, 1);           // maxState
    put32(d, 0x1448, 0x12060);     // pUnwindMap
    put32(d, 0x1460, 0xFFFFFFFF);  // toState
    put32(d, 0x1464, 0x112E0);     // action
    const uint8_t stub[] = {0xB8, 0x40, 0x20, 0x01, 0x00, 0xE9, 0x00, 0x00, 0x00, 0x00};
    std::memcpy(&d[0x12D0], stub, sizeof stub);
    put32(d, 0x12D6, uint32_t(0x11130 - 0x112DA));  // jmp 11130 (stands in for __CxxFrameHandler)
    const uint8_t funclet[] = {0xDF, 0xF1, 0x8D, 0x4D, 0xFC, 0xC3};  // fcomip st1; lea ecx, [ebp-4]; ret
    std::memcpy(&d[0x12E0], funclet, sizeof funclet);
    return d;
}

}  // namespace

// The runtime's fail-loudly hook: tests turn traps into exceptions.
namespace qilin::rt {
void trap(uint32_t, const char* why) { throw std::runtime_error(why); }
}  // namespace qilin::rt

int main() {
    // ---------------------------------------------------------------- loader
    auto loaded = Xbe::parse(make_xbe());
    CHECK(loaded.has_value());
    if (!loaded) {
        std::println(stderr, "parse failed: {}", loaded.error());
        return 1;
    }
    const Xbe& x = *loaded;
    CHECK(x.kind == "retail");
    CHECK(x.entry == 0x11000);
    CHECK(x.kthunk == 0x12000);
    CHECK(x.kernel_imports == std::vector<uint32_t>{1});
    CHECK(x.cert.title_name == "T\xF0\x9F\x98\x80");  // surrogate pair -> 4-byte UTF-8
    CHECK(x.sections.size() == 2 && x.sections[0].code && !x.sections[1].code);
    CHECK(x.code_section_at(0x11000) == &x.sections[0]);
    CHECK(x.code_section_at(0x113FF) == &x.sections[0]);
    CHECK(x.code_section_at(0x11400) == nullptr);
    CHECK(x.code_section_at(0x12000) == nullptr);  // .rdata is not code

    {
        auto bad = make_xbe();
        bad[0] = 'Y';
        auto r = Xbe::parse(bad);
        CHECK(!r && r.error().contains("bad magic"));
        auto truncated = make_xbe();
        truncated.resize(0x200);
        CHECK(!Xbe::parse(truncated).has_value());
    }

    // ---------------------------------------------------------------- decoder
    Decoder dec(x);
    const Insn* call = dec.decode(0x11000);
    CHECK(call && call->is_call() && call->len == 5 && call->rel_target() == 0x11010u);
    CHECK(dec.decode(0x11000) == call);  // cached
    const Insn* jmp = dec.decode(0x11019);
    CHECK(jmp && jmp->is_jmp() && jmp->op[0].kind == Operand::Kind::Mem && jmp->op[0].scale == 4);
    CHECK(dec.decode(0x11017)->is_cond_branch());
    CHECK(dec.bytes(*call).size() == 5 && dec.bytes(*call)[0] == 0xE8);
    CHECK(dec.format(*jmp).contains("jmp"));
    CHECK(dec.decode(0x11000)->implausible_reason() == nullptr);

    // ---------------------------------------------------------------- CFG
    Cfg cfg(x, dec);
    cfg.run();
    auto& f = cfg.functions();
    CHECK(f.contains(0x11000) && f.contains(0x11010) && f.contains(0x11100) && f.contains(0x11130));
    CHECK(f.contains(0x112A0));  // after int3 padding, and pointed to from .rdata
    CHECK(!f.contains(0x112B0));  // loop head behind alignment filler is a label
    if (f.contains(0x112A0)) CHECK(std::ranges::binary_search(f.at(0x112A0).insns, 0x112B0u));
    CHECK(f.contains(0x112D0) && f.at(0x112D0).origin == Origin::Eh);  // __ehhandler stub
    CHECK(f.contains(0x112E0));  // unwind funclet
    CHECK(f.size() == 7);  // nothing seeded inside the jump/index tables or the filler
    {
        const Insn* fc = dec.decode(0x112E0);  // fcomip: ZF, PF, CF set; OF, SF, AF cleared
        CHECK(fc && fc->flags_written == 63 && fc->flags_defined == 63 && fc->flags_read == 0);
    }
    {
        auto cov = cfg.coverage();
        uint64_t unknown = 0;
        for (auto& r : cov)
            if (r.lifted) unknown += r.unknown;
        CHECK(unknown == 0);
    }
    if (f.contains(0x11010)) {
        auto& fn = f.at(0x11010);
        CHECK(fn.jtabs.size() == 1);
        if (!fn.jtabs.empty()) {
            CHECK(fn.jtabs[0].how == "cmp" && fn.jtabs[0].bounded);
            CHECK((fn.jtabs[0].targets == std::vector<uint32_t>{0x11020, 0x11023, 0x11026}));
            CHECK(fn.jtabs[0].entries.size() == 3);
        }
        CHECK(fn.insns.size() == 10);
    }
    if (f.contains(0x11100)) {
        auto& fn = f.at(0x11100);
        CHECK(fn.jtabs.size() == 1);
        if (!fn.jtabs.empty()) {
            CHECK(fn.jtabs[0].how == "cmp+bytetable");
            CHECK(fn.jtabs[0].index_table == 0x11200 && fn.jtabs[0].index_count == 0x84);
            CHECK((fn.jtabs[0].entries == std::vector<uint32_t>{0x11119, 0x1111C}));
        }
    }

    // ---------------------------------------------------------------- lifter
    Lifter lifter(dec);
    if (f.contains(0x112A0)) {
        auto r = lifter.lift(f.at(0x112A0));  // the label is lifted as part of its function
        CHECK(r.has_value() && r->contains("L_000112B0"));
    }
    if (f.contains(0x11010)) {
        auto r = lifter.lift(f.at(0x11010));
        CHECK(r.has_value());
        if (r) {
            CHECK(r->contains("switch (eax)"));
            CHECK(r->contains("case 2: goto L_00011026;"));
            // cmp eax, 2 feeds only ja: CF and ZF are computed, OF/SF/AF are not.
            CHECK(r->contains("cf = a < b;"));
            CHECK(!r->contains("of = (("));
            // Only the registers it modifies are stored back.
            CHECK(r->contains("c.eax = eax;") && r->contains("c.esp = esp;"));
            CHECK(!r->contains("c.ebx = ebx;") && !r->contains("c.xmm[0] = xmm0;"));
        }
    }
    if (f.contains(0x11100)) {
        auto r = lifter.lift(f.at(0x11100));
        CHECK(r.has_value() && r->contains("switch (ecx)"));
    }
    if (f.contains(0x11000)) {
        auto r = lifter.lift(f.at(0x11000));
        CHECK(!r && r.error().kind == LiftFailure::Kind::NotLeaf);
    }
    if (f.contains(0x11130)) {
        auto r = lifter.lift(f.at(0x11130));
        CHECK(!r && r.error().kind == LiftFailure::Kind::Fragment && r.error().detail.contains("return address"));
    }

    // ---------------------------------------------------------------- runtime helpers
    CHECK(rt::parity8(0) && !rt::parity8(1) && rt::parity8(3) && rt::parity8(0x100));
    CHECK(rt::cvtt_f32_i32(-1.9f) == uint32_t(-1));
    CHECK(rt::cvtt_f32_i32(std::numeric_limits<float>::quiet_NaN()) == 0x80000000u);
    CHECK(rt::cvtt_f32_i32(3e9f) == 0x80000000u);
    CHECK(rt::cvtt_f32_i32(-2147483648.0f) == 0x80000000u);  // exactly INT_MIN: representable
    CHECK(rt::cvt_f32_i32(2.5f) == 2 && rt::cvt_f32_i32(3.5f) == 4);  // round half to even
    {
        const uint32_t one = rt::u32(1.0f), nan = 0x7FC00000u, pz = 0, nz = 0x80000000u;
        CHECK(rt::minss(nan, one) == one && rt::minss(one, nan) == nan);  // second operand on NaN
        CHECK(rt::minss(pz, nz) == nz && rt::maxss(nz, pz) == pz);        // second operand on ±0
    }
    {
        std::vector<uint8_t> mem(64, 0);
        const rt::Mem m{mem.data(), mem.size()};
        for (int k = 0; k < 8; ++k) mem[size_t(k)] = uint8_t(k + 1);
        // Overlapping forward copy (dst = src + 1) replicates the first byte, like the CPU.
        uint32_t esi = 0, edi = 1, ecx = 6;
        rt::rep_movs(m, esi, edi, ecx, 1, 0);
        CHECK(mem[1] == 1 && mem[6] == 1 && mem[7] == 8 && esi == 6 && edi == 7 && ecx == 0);
        // Non-overlapping dword copy.
        esi = 0, edi = 32, ecx = 2;
        rt::rep_movs(m, esi, edi, ecx, 4, 0);
        CHECK(std::memcmp(&mem[32], &mem[0], 8) == 0 && edi == 40);
        // Non-uniform dword fill.
        edi = 16, ecx = 3;
        rt::rep_stos32(m, edi, ecx, 0x11223344u, 0);
        uint32_t v;
        std::memcpy(&v, &mem[24], 4);
        CHECK(v == 0x11223344u && edi == 28 && ecx == 0);
    }

    // ---------------------------------------------------------------- JSON
    {
        JsonWriter j;
        j.begin_object().kv("a", uint8_t(7)).kv("b", true).kv("s", std::string("x\"y")).key("n").null().end_object();
        // (kept out of CHECK: MSVC's preprocessor mangles raw strings when stringizing)
        const std::string expected = R"({"a":7,"b":true,"s":"x\"y","n":null})";
        CHECK(j.str() == expected);
    }

    if (failures) {
        std::println(stderr, "{} check(s) failed", failures);
        return 1;
    }
    std::println("all tests passed");
    return 0;
}
