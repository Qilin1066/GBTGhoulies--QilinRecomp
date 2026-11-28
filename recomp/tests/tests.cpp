#include "path.hpp"
#include <fstream>
// Self-contained tests: build a tiny synthetic XBE in memory and run the
// loader, decoder, CFG and lifter over it, and exercise the runtime helpers.
// No game files required (runs in CI).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <iterator>
#include <print>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <sys/wait.h>
#endif

#include "cfg.hpp"
#include "decoder.hpp"
#include "guest.hpp"
#include "identify.hpp"
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

    // 11300: a switch bounded by a register holding a constant, with the bound
    // a block away from the jump; its table is followed directly by another
    // table, which a heuristic scan would run into.
    const uint8_t regbound[] = {
        0x8B, 0x4C, 0x24, 0x04,                    // 11300 mov ecx, [esp+4]
        0xBA, 0x03, 0x00, 0x00, 0x00,              // 11304 mov edx, 3
        0x3B, 0xCA,                                // 11309 cmp ecx, edx
        0x77, 0x11,                                // 1130B ja 1131E
        0x33, 0xC0,                                // 1130D xor eax, eax
        0xFF, 0x24, 0x8D, 0x20, 0x13, 0x01, 0x00,  // 1130F jmp [4*ecx + 0x11320]
        0x40, 0xC3, 0x48, 0xC3, 0x41, 0xC3, 0x49, 0xC3,  // 11316.. four cases
        0xC3,                                      // 1131E ret
    };
    std::memcpy(&d[0x1300], regbound, sizeof regbound);
    put32(d, 0x1320, 0x11316);
    put32(d, 0x1324, 0x11318);
    put32(d, 0x1328, 0x1131A);
    put32(d, 0x132C, 0x1131C);
    // 11340: MSVC's negative-index form: `sub ecx, 4; jb` then
    // jmp [4*ecx + 0x11340] reads the four slots at 11330 (ecx = -4..-1).
    const uint8_t negidx[] = {
        0x8B, 0x4C, 0x24, 0x04,                    // 11340 mov ecx, [esp+4]
        0x83, 0xE9, 0x04,                          // 11344 sub ecx, 4
        0x72, 0x03,                                // 11347 jb 1134C
        0x31, 0xC0, 0xC3,                          // 11349 xor eax, eax; ret
        0xFF, 0x24, 0x8D, 0x40, 0x13, 0x01, 0x00,  // 1134C jmp [4*ecx + 0x11340]
        0xB0, 0x01, 0xC3, 0xB0, 0x02, 0xC3, 0xB0, 0x03, 0xC3, 0xB0, 0x04, 0xC3,  // 11353.. cases
    };
    std::memcpy(&d[0x1340], negidx, sizeof negidx);
    put32(d, 0x1330, 0x11353);
    put32(d, 0x1334, 0x11356);
    put32(d, 0x1338, 0x11359);
    put32(d, 0x133C, 0x1135C);
    put32(d, 0x1414, 0x11300);  // .rdata pointers to both
    put32(d, 0x1418, 0x11340);
    // 11360: backward copy (std; rep movsd; cld), then repe cmpsb feeding jnz.
    const uint8_t strops[] = {
        0xFD,              // 11360 std
        0xF3, 0xA5,        // 11361 rep movsd
        0xFC,              // 11363 cld
        0x33, 0xC0,        // 11364 xor eax, eax  (ZF = 1 if no byte is compared)
        0xF3, 0xA6,        // 11366 repe cmpsb
        0x75, 0x01,        // 11368 jnz 1136B
        0x40,              // 1136A inc eax
        0xC3,              // 1136B ret
    };
    std::memcpy(&d[0x1360], strops, sizeof strops);
    put32(d, 0x141C, 0x11360);
    // 11370: x87: a float function returning in ST0, with fcomip feeding a branch.
    const uint8_t x87[] = {
        0xD9, 0x44, 0x24, 0x04,  // 11370 fld dword ptr [esp+4]
        0xD8, 0x4C, 0x24, 0x08,  // 11374 fmul dword ptr [esp+8]
        0xD9, 0xC0,              // 11378 fld st0
        0xDE, 0xC1,              // 1137A faddp st1, st0
        0xD9, 0xE8,              // 1137C fld1
        0xDF, 0xF1,              // 1137E fcomip st0, st1
        0x76, 0x02,              // 11380 jbe 11384
        0xD9, 0xE0,              // 11382 fchs
        0xC3,                    // 11384 ret
    };
    std::memcpy(&d[0x1370], x87, sizeof x87);
    put32(d, 0x1420, 0x11370);
    // 11390: a C++ exception frame linked into fs:[0] and unlinked again.
    const uint8_t ehframe[] = {
        0x6A, 0xFF,                                // 11390 push -1 (state)
        0x68, 0xD0, 0x12, 0x01, 0x00,              // 11392 push 0x112D0 (__ehhandler stub)
        0x64, 0xA1, 0x00, 0x00, 0x00, 0x00,        // 11397 mov eax, fs:[0]
        0x50,                                      // 1139D push eax
        0x64, 0x89, 0x25, 0x00, 0x00, 0x00, 0x00,  // 1139E mov fs:[0], esp
        0x8B, 0x0C, 0x24,                          // 113A5 mov ecx, [esp]
        0x64, 0x89, 0x0D, 0x00, 0x00, 0x00, 0x00,  // 113A8 mov fs:[0], ecx
        0x64, 0x8B, 0x41, 0x18,                    // 113AF mov eax, fs:[ecx+0x18]
        0x83, 0xC4, 0x0C,                          // 113B3 add esp, 0xC
        0xC3,                                      // 113B6 ret
    };
    std::memcpy(&d[0x1390], ehframe, sizeof ehframe);
    put32(d, 0x1424, 0x11390);
    std::memset(&d[0x13B7], 0xCC, 0x113C0 - 0x113B7);
    // 113C0: instructions the C runtime's hand-written assembly uses.
    const uint8_t crtops[] = {
        0x0F, 0xA4, 0xC2, 0x05,  // 113C0 shld edx, eax, 5
        0x0F, 0xAD, 0xC2,        // 113C4 shrd edx, eax, cl
        0xD1, 0xD8,              // 113C7 rcr eax, 1
        0xE3, 0x03,              // 113C9 jecxz 113CE
        0xF2, 0xAE,              // 113CB repne scasb
        0x90,                    // 113CD nop
        0xD9, 0xFC,              // 113CE frndint
        0xD9, 0xFD,              // 113D0 fscale
        0xD9, 0xE5,              // 113D2 fxam
        0xDB, 0xE2,              // 113D4 fnclex
        0xD9, 0xF0,              // 113D6 f2xm1
        0xDF, 0x7C, 0x24, 0xF8,  // 113D8 fistp qword ptr [esp-8]
        0xDB, 0x6C, 0x24, 0xF8,  // 113DC fld tbyte ptr [esp-8]
        0xDD, 0x74, 0x24, 0x80,  // 113E0 fnsave [esp-0x80]
        0xDD, 0x64, 0x24, 0x80,  // 113E4 frstor [esp-0x80]
        0xDD, 0xD8,              // 113E8 fstp st0
        0xC3,                    // 113EA ret
        // 113EB: returns ZF to its caller (a Conventions::flags_out helper)
        0x85, 0xC0,  // 113EB test eax, eax
        0xC3,        // 113ED ret
        // 113EE: branches on the ZF its caller left (flags_in)
        0x74, 0x01,  // 113EE je 113F1
        0x40,        // 113F0 inc eax
        0xC3,        // 113F1 ret
    };
    std::memcpy(&d[0x13C0], crtops, sizeof crtops);
    put32(d, 0x1428, 0x113C0);
    // 120A0 (in .rdata, after the EH tables; the code section is full):
    // instructions XACT and XGraphics use, lifted by hand in main.
    const uint8_t libops[] = {
        0x66, 0x0F, 0xAF, 0x30,        // 120A0 imul si, word ptr [eax]
        0xC1, 0xC0, 0x05,              // 120A4 rol eax, 5
        0xD3, 0xC9,                    // 120A7 ror ecx, cl
        0x0F, 0x6F, 0x06,              // 120A9 movq mm0, qword ptr [esi]
        0x0F, 0x61, 0xC5,              // 120AC punpcklwd mm0, mm5
        0x0F, 0x7F, 0x07,              // 120AF movq qword ptr [edi], mm0
        0x0F, 0x77,                    // 120B2 emms
        0x0F, 0x2B, 0x07,              // 120B4 movntps xmmword ptr [edi], xmm0
        0xA1, 0x0C, 0x20, 0x80, 0xFE,  // 120B7 mov eax, [0xFE80200C] (the APU's sample counter)
        0xC3,                          // 120BC ret
    };
    std::memcpy(&d[0x14A0], libops, sizeof libops);
    return d;
}

}  // namespace

// The runtime's fail-loudly hook: tests turn traps into exceptions.
namespace qilin::rt {
void trap(uint32_t, const char* why) { throw std::runtime_error(why); }
}  // namespace qilin::rt

int main() {
    {
        const auto path = std::filesystem::temp_directory_path() / std::filesystem::path(u8"ghoulies-é-保存.xbe");
        const auto bytes = make_xbe();
        { std::ofstream output(path, std::ios::binary); output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size())); }
        CHECK(Xbe::load(utf8_path(path)).has_value());
        std::filesystem::remove(path);
    }

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
    CHECK(f.size() == 13);  // nothing seeded inside the jump/index tables or the filler
    if (f.contains(0x11300) && f.at(0x11300).jtabs.size() == 1) {
        auto& t = f.at(0x11300).jtabs[0];  // proven by value ranges, not by a cmp next to the jump
        CHECK(t.how == "range" && t.bounded && t.first == 0);
        CHECK((t.entries == std::vector<uint32_t>{0x11316, 0x11318, 0x1131A, 0x1131C}));
    } else CHECK(false);
    if (f.contains(0x11340) && f.at(0x11340).jtabs.size() == 1) {
        auto& t = f.at(0x11340).jtabs[0];
        CHECK(t.how == "range" && t.bounded && t.first == -4);
        CHECK((t.entries == std::vector<uint32_t>{0x11353, 0x11356, 0x11359, 0x1135C}));
        CHECK(t.data_lo() == 0x11330u && t.data_hi() == 0x11340u);
    } else CHECK(false);
    {
        const Insn* fc = dec.decode(0x112E0);  // fcomip: ZF, PF, CF set; OF, SF, AF cleared
        CHECK(fc && fc->flags_written == 63 && fc->flags_defined == 63 && fc->flags_read == 0);
    }
    {
        // Every byte of the synthetic code section accounted for. (It lies
        // outside the game's text regions, so it comes back as one region
        // that isn't marked lifted; counting only lifted regions, as this
        // did, counted nothing.)
        // The one span left is 113EB-113F1, the two functions off the
        // 16-byte grid that the lifter test below describes by hand, which
        // nothing calls.
        auto cov = cfg.coverage();
        uint64_t unknown = 0, code = 0;
        std::vector<std::pair<uint32_t, uint32_t>> spans;
        for (auto& r : cov) {
            unknown += r.unknown, code += r.code;
            for (auto& g : r.unknown_spans) spans.emplace_back(g.addr, uint32_t(g.size));
        }
        CHECK(!cov.empty() && code > 0);
        CHECK(unknown == 7 && (spans == std::vector<std::pair<uint32_t, uint32_t>>{{0x113EBu, 7u}}));
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

    // ---------------------------------------------------------------- SSE NaN rules (x86, host-independent)
    {
        using namespace qilin::rt;
        const uint32_t qa = 0x7FC00001u, qb = 0xFFC00002u, sa = 0x7F800003u;  // QNaN, QNaN, SNaN
        CHECK(addss(qa, qb) == qa);                  // both NaN: the first operand's
        CHECK(subss(qb, qa) == qb);
        CHECK(mulss(0x3F800000u, qb) == qb);         // one NaN: that one
        CHECK(divss(sa, 0x3F800000u) == 0x7FC00003u);  // SNaN: quietened
        CHECK(addss(0x7F800000u, 0xFF800000u) == 0xFFC00000u);  // inf - inf: default NaN
        CHECK(mulss(0u, 0x7F800000u) == 0xFFC00000u);          // 0 * inf
        CHECK(divss(0u, 0u) == 0xFFC00000u);                   // 0 / 0
        CHECK(sqrtss(0xBF800000u) == 0xFFC00000u);             // sqrt(-1)
        CHECK(sqrtss(sa) == 0x7FC00003u);
        CHECK(addss(0x3F800000u, 0x40000000u) == 0x40400000u);  // 1 + 2 = 3
    }

    // ---------------------------------------------------------------- x87 transcendentals: the same bits on every host
    {
        // Correctly rounded results from mpmath (tools/check/x87/transcendental.py --golden).
        // CI runs these on every platform, so a host whose arithmetic differs fails here.
        namespace rt = qilin::rt;
        const std::pair<uint64_t, uint64_t> golden[] = {
        {rt::dbits(rt::fsin(rt::dfrom(0x3FE0000000000000ull))), 0x3FDEAEE8744B05F0ull},  // fsin(0.5) = 0.479425538604203
        {rt::dbits(rt::fsin(rt::dfrom(0xC008000000000000ull))), 0xBFC210386DB6D55Bull},  // fsin(-3.0) = -0.1411200080598672
        {rt::dbits(rt::fsin(rt::dfrom(0x412E848000000000ull))), 0xBFD6664B2568D867ull},  // fsin(1000000.0) = -0.34999350217129294
        {rt::dbits(rt::fsin(rt::dfrom(0x400921FB54442D18ull))), 0x3CA1A62633145C07ull},  // fsin(3.141592653589793) = 1.2246467991473532e-16
        {rt::dbits(rt::fsin(rt::dfrom(0x43DFFFFFFFFFFFFFull))), 0x3FEFA7299B17573Dull},  // fsin(9.223372036854775e+18) = 0.989155581390811
        {rt::dbits(rt::fsin(rt::dfrom(0x0170000000000000ull))), 0x0170000000000000ull},  // fsin(9.332636185032189e-302) = 9.332636185032189e-302
        {rt::dbits(rt::fsin(rt::dfrom(0x4076300000000000ull))), 0xBEFF9BD0307D1DE3ull},  // fsin(355.0) = -3.014435335948845e-05
        {rt::dbits(rt::fsin(rt::dfrom(0x413921FB54442D18ull))), 0xBDD1A62633145C07ull},  // fsin(1647099.3291652855) = -6.420676210313675e-11
        {rt::dbits(rt::fsin(rt::dfrom(0x4336AC5B262CA1FFull))), 0xBFE29E22E425CB7Dull},  // fsin(6381956970095103.0) = -0.5818037467964668
        {rt::dbits(rt::fcos(rt::dfrom(0x3FE0000000000000ull))), 0x3FEC1528065B7D50ull},  // fcos(0.5) = 0.8775825618903728
        {rt::dbits(rt::fcos(rt::dfrom(0x3FF921FB54442D18ull))), 0x3C91A62633145C07ull},  // fcos(1.5707963267948966) = 6.123233995736766e-17
        {rt::dbits(rt::fcos(rt::dfrom(0x4036000000000000ull))), 0xBFEFFFADD8D4ACDAull},  // fcos(22.0) = -0.9999608263946371
        {rt::dbits(rt::fcos(rt::dfrom(0x430C6BF526340000ull))), 0xBFE06C154609D33Full},  // fcos(1000000000000000.0) = -0.5131937377869703
        {rt::dbits(rt::fcos(rt::dfrom(0xC01C000000000000ull))), 0x3FE81FF79ED92017ull},  // fcos(-7.0) = 0.7539022543433046
        {rt::dbits(rt::fcos(rt::dfrom(0x427921FB5445D85Aull))), 0xBEE19DC06A22DBBCull},  // fcos(1727108826205.522) = -8.400066978315561e-06
        {rt::dbits(rt::ftan(rt::dfrom(0x3FE8000000000000ull))), 0x3FEDCFA36110EEECull},  // ftan(0.75) = 0.9315964599440725
        {rt::dbits(rt::ftan(rt::dfrom(0x3FF921FB54442D18ull))), 0x434D02967C31CDB5ull},  // ftan(1.5707963267948966) = 1.633123935319537e+16
        {rt::dbits(rt::ftan(rt::dfrom(0xC059000000000000ull))), 0x3FE2CA74D62B5D38ull},  // ftan(-100.0) = 0.5872139151569291
        {rt::dbits(rt::ftan(rt::dfrom(0x4202A05F20000000ull))), 0xBFE1DE000F443F50ull},  // ftan(10000000000.0) = -0.5583496378112418
        {rt::dbits(rt::fpatan(rt::dfrom(0x3FF0000000000000ull), rt::dfrom(0x3FF0000000000000ull))), 0x3FE921FB54442D18ull},  // fpatan(1.0, 1.0) = 0.7853981633974483
        {rt::dbits(rt::fpatan(rt::dfrom(0xC004000000000000ull), rt::dfrom(0x4008000000000000ull))), 0xBFE63B4BC3F16A8Aull},  // fpatan(-2.5, 3.0) = -0.6947382761967031
        {rt::dbits(rt::fpatan(rt::dfrom(0x01A56E1FC2F8F359ull), rt::dfrom(0xBFF0000000000000ull))), 0x400921FB54442D18ull},  // fpatan(1e-300, -1.0) = 3.141592653589793
        {rt::dbits(rt::fpatan(rt::dfrom(0x4008000000000000ull), rt::dfrom(0xBDDB7CDFD9D7BDBBull))), 0x3FF921FB54467780ull},  // fpatan(3.0, -1e-10) = 1.57079632682823
        {rt::dbits(rt::fpatan(rt::dfrom(0x0000000000004000ull), rt::dfrom(0x0000000000000010ull))), 0x3FF91DFB5459826Dull},  // fpatan(8.095e-320, 8e-323) = 1.5698197646053373
        {rt::dbits(rt::fpatan(rt::dfrom(0x16687E92154EF7ACull), rt::dfrom(0x54B249AD2594C37Dull))), 0x01A56E1FC2F8F359ull},  // fpatan(1e-200, 1e+100) = 1e-300
        {rt::dbits(rt::fpatan(rt::dfrom(0xBFF0000000000000ull), rt::dfrom(0xBFF0000000000000ull))), 0xC002D97C7F3321D2ull},  // fpatan(-1.0, -1.0) = -2.356194490192345
        {rt::dbits(rt::fpatan(rt::dfrom(0x01A56E1FC2F8F359ull), rt::dfrom(0x4202A05F20000000ull))), 0x000012688B70E62Bull},  // fpatan(1e-300, 10000000000.0) = 1e-310
        {rt::dbits(rt::fyl2x(rt::dfrom(0x3FF0000000000000ull), rt::dfrom(0x4008000000000000ull))), 0x3FF95C01A39FBD68ull},  // fyl2x(1.0, 3.0) = 1.584962500721156
        {rt::dbits(rt::fyl2x(rt::dfrom(0xC004000000000000ull), rt::dfrom(0x3FB999999999999Aull))), 0x40209C1165EC0627ull},  // fyl2x(-2.5, 0.1) = 8.304820237218406
        {rt::dbits(rt::fyl2x(rt::dfrom(0x4008000000000000ull), rt::dfrom(0x3FF0000000000001ull))), 0x3CD14FF58BE0A23Eull},  // fyl2x(3.0, 1.0000000000000002) = 9.610279511444752e-16
        {rt::dbits(rt::fyl2x(rt::dfrom(0x000012688B70E62Bull), rt::dfrom(0x401C000000000000ull))), 0x000033ADC918D1D7ull},  // fyl2x(1e-310, 7.0) = 2.8073549220576e-310
        {rt::dbits(rt::fyl2x(rt::dfrom(0x7E5DDD4BAA009303ull), rt::dfrom(0x01A56E1FC2F8F359ull))), 0xFEFD109028C6770Eull},  // fyl2x(5e+300, 1e-300) = -4.9828921423310436e+303
        {rt::dbits(rt::fyl2x(rt::dfrom(0x3FE0000000000000ull), rt::dfrom(0x0000000000000001ull))), 0xC080C80000000000ull},  // fyl2x(0.5, 5e-324) = -537.0
        {rt::dbits(rt::fyl2x(rt::dfrom(0x3FF0000000000000ull), rt::dfrom(0x3FEFFFFFFFFFFFFFull))), 0xBCA71547652B82FEull},  // fyl2x(1.0, 0.9999999999999999) = -1.6017132519074588e-16
        {rt::dbits(rt::f2xm1(rt::dfrom(0x3FE0000000000000ull))), 0x3FDA827999FCEF32ull},  // f2xm1(0.5) = 0.41421356237309503
        {rt::dbits(rt::f2xm1(rt::dfrom(0xBFE0000000000000ull))), 0xBFD2BEC333018867ull},  // f2xm1(-0.5) = -0.2928932188134525
        {rt::dbits(rt::f2xm1(rt::dfrom(0x3FF0000000000000ull))), 0x3FF0000000000000ull},  // f2xm1(1.0) = 1.0
        {rt::dbits(rt::f2xm1(rt::dfrom(0xBFF0000000000000ull))), 0xBFE0000000000000ull},  // f2xm1(-1.0) = -0.5
        {rt::dbits(rt::f2xm1(rt::dfrom(0x3B90000000000000ull))), 0x3B862E42FEFA39EFull},  // f2xm1(8.470329472543003e-22) = 5.871184992306991e-22
        {rt::dbits(rt::f2xm1(rt::dfrom(0x8000000000000010ull))), 0x800000000000000Bull},  // f2xm1(-8e-323) = -5.4e-323
        {rt::dbits(rt::f2xm1(rt::dfrom(0x3FA0000000000000ull))), 0x3F966C34C5615D0Full},  // f2xm1(0.03125) = 0.02189714865411668
        {rt::dbits(rt::f2xm1(rt::dfrom(0xBF90000000000000ull))), 0xBF860F9F985BC9F4ull},  // f2xm1(-0.015625) = -0.010771986806024515
        {rt::dbits(rt::f2xm1(rt::dfrom(0x3FEFFFFFFFFFFFFFull))), 0x3FEFFFFFFFFFFFFFull},  // f2xm1(0.9999999999999999) = 0.9999999999999999
        {rt::dbits(rt::f2xm1(rt::dfrom(0x408FFC0000000000ull))), 0x7FE6A09E667F3BCDull},  // f2xm1(1023.5) = 1.2711610061536464e+308
        {rt::dbits(rt::f2xm1(rt::dfrom(0xC04B800000000000ull))), 0xBFF0000000000000ull},  // f2xm1(-55.0) = -1.0
        };
        for (size_t k = 0; k < std::size(golden); ++k)
            if (golden[k].first != golden[k].second) {
                std::println(stderr, "x87 transcendental vector {}: got {:016X}, want {:016X}", k, golden[k].first, golden[k].second);
                CHECK(false);
            }
        // Special operands follow the x87 tables.
        CHECK(rt::dbits(rt::fsin(rt::dfrom(0x8000000000000000ull))) == 0x8000000000000000ull);  // sin(-0) = -0
        CHECK(rt::dbits(rt::fsin(rt::dfrom(0x7FF0000000000000ull))) == rt::kX87Indefinite);   // sin(inf): invalid
        CHECK(rt::dbits(rt::fpatan(rt::dfrom(0x8000000000000000ull), -1.0)) == 0xC00921FB54442D18ull);  // atan2(-0, -1) = -pi
        CHECK(rt::fpatan(-1.0, 0.0) == -0x1.921fb54442d18p+0);
        CHECK(rt::fyl2x(2.0, 0.0) == -rt::dfrom(0x7FF0000000000000ull));                     // 2 log2(0) = -inf
        CHECK(rt::dbits(rt::f2xm1(rt::dfrom(0x8000000000000000ull))) == 0x8000000000000000ull);  // 2^-0 - 1 = -0
        CHECK(rt::f2xm1(-rt::dfrom(0x7FF0000000000000ull)) == -1.0);
        CHECK(rt::fscale(3.0, 4.9) == 48.0 && rt::fscale(3.0, -2.5) == 0.75);  // the scale is truncated
        CHECK(rt::dbits(rt::fscale(0.0, rt::dfrom(0x7FF0000000000000ull))) == rt::kX87Indefinite);  // 0 * 2^inf
        CHECK(rt::frndint(0x027F, 2.5) == 2.0 && rt::frndint(0x067F, 2.5) == 2.0 && rt::frndint(0x0A7F, 2.5) == 3.0 &&
              rt::frndint(0x0E7F, -2.5) == -2.0);  // nearest-even, down, up, toward zero
        CHECK(rt::fist64(0x027F, -3.5) == uint64_t(-4) && rt::fist64(0x027F, 1e19) == 0x8000000000000000ull);
        CHECK(rt::fxam_codes(0, true, 0.0) == 0x4100u && rt::fxam_codes(0, false, -1.0) == 0x0600u &&
              rt::fxam_codes(0, false, 0.0) == 0x4000u && rt::fxam_codes(0, false, 1e-310) == 0x4400u &&
              rt::fxam_codes(0x4700, false, rt::dfrom(0x7FF0000000000000ull)) == 0x0500u);
        {
            std::vector<uint8_t> mem(256);
            const rt::Mem m{mem.data(), mem.size()};
            for (double v : {1.0, -0.0, 1e-310, -3.25e300, rt::dfrom(0x7FF0000000000000ull), rt::dfrom(0x7FF8000000000123ull)}) {
                rt::st80(m, 0, v, 0);
                CHECK(rt::dbits(rt::fld80(rt::ld64(m, 0, 0), rt::ld16(m, 8, 0))) == rt::dbits(v));
            }
            CHECK(rt::ld16(m, 8, 0) == 0x7FFF);  // the NaN stored last
            double st[8] = {1, 2, 3, 4, 5, 6, 7, 8};
            uint32_t top = 3, tag = 0b00011000, cw = 0x027F, sw = 0x4100;
            rt::fnsave(m, 0, st, top, tag, cw, sw, 0);
            CHECK(cw == 0x037F && sw == 0 && tag == 0 && top == 0);  // fninit
            CHECK(rt::ld32(m, 4, 0) == (0x4100u | 3u << 11) && rt::ld32(m, 8, 0) == 0xFC3Fu);  // TOP in the status word; tags 3 and 4 valid
            for (double& v : st) v = 0;
            rt::frstor(m, 0, st, top, tag, cw, sw, 0);
            CHECK(cw == 0x027F && sw == 0x4100 && tag == 0b00011000 && top == 3 && st[3] == 4.0 && st[4] == 5.0 && st[2] == 3.0);
        }
        CHECK(rt::dbits(rt::fyl2x(0.0, 0.0)) == rt::kX87Indefinite);                          // 0 log2(0): invalid
        CHECK(rt::dbits(rt::fyl2x(1.0, -2.0)) == rt::kX87Indefinite);                         // log2 of a negative
        CHECK(rt::dbits(rt::fyl2x(rt::dfrom(0x8000000000000000ull), 0.5)) == 0x0000000000000000ull);  // -0 * log2(0.5) = +0
    }
    // ---------------------------------------------------------------- x87 arithmetic (24-bit precision control)
    {
        using namespace qilin::rt;
        constexpr uint32_t pc24 = 0x007F, pc53 = 0x027F;
        auto D = [](uint64_t b) { return dfrom(b); };
        // Ties after the first rounding: the round-to-odd path gets these right.
        CHECK(dbits(fadd(pc24, D(0x41c5696f50000000ull), D(0x3dcedeb160000000ull))) == 0x41c5696f60000000ull);
        CHECK(dbits(fadd(pc24, D(0xb9cd634e6eb1a730ull), D(0x417f26e8f0000000ull))) == 0x417f26e8e0000000ull);
        CHECK(dbits(fsub(pc24, D(0x41a204f570000000ull), D(0x3c101b5ba0000000ull))) == 0x41a204f560000000ull);
        CHECK(dbits(fsub(pc24, D(0x3ba858a5a0000000ull), D(0xc1b8cf60d0000000ull))) == 0x41b8cf60e0000000ull);
        // 53-bit precision control: plain double arithmetic.
        CHECK(fadd(pc53, D(0x41c5696f50000000ull), D(0x3dcedeb160000000ull)) ==
              D(0x41c5696f50000000ull) + D(0x3dcedeb160000000ull));
        CHECK(dbits(fsqrt(pc24, 2.0)) == 0x3FF6A09E60000000ull);  // sqrt(2) to 24 bits
        CHECK(dbits(fmul(pc24, 3.0, 1.0 / 3.0)) == 0x3FF0000000000000ull);
        // Exponent range stays the register's: no overflow at FLT_MAX.
        CHECK(std::isfinite(fmul(pc24, double(3.0e38f), 10.0)) && fmul(pc24, double(3.0e38f), 10.0) > 1e39);
        // NaN rules: indefinite for invalid operations; larger payload; QNaN beats SNaN.
        CHECK(dbits(fadd(pc24, INFINITY, -INFINITY)) == kX87Indefinite);
        CHECK(dbits(fdiv(pc24, 0.0, 0.0)) == kX87Indefinite);
        CHECK(dbits(fsqrt(pc24, -1.0)) == kX87Indefinite);
        CHECK(dbits(fadd(pc24, D(0x7FF8000000000001ull), D(0xFFF8000000000002ull))) == 0xFFF8000000000002ull);
        CHECK(dbits(fmul(pc24, D(0x7FF0000000000005ull), D(0x7FF8000000000001ull))) == 0x7FF8000000000001ull);
        CHECK(dbits(fsub(pc24, 1.0, D(0x7FF0000000000005ull))) == 0x7FF8000000000005ull);  // SNaN quietened
        // Memory conversions.
        CHECK(is_snan64(dbits(f32_raw(0x7F800001u))) && !is_snan64(dbits(fld32(0x7F800001u))));
        CHECK(fst32(1.0 + 0x1p-24) == 0x3F800000u);         // tie to even
        CHECK(fst32(1.0 + 0x1p-24 + 0x1p-50) == 0x3F800001u);
        CHECK(fst32(1e39) == 0x7F800000u);                  // overflow to infinity
        CHECK(fst32(dfrom(0xFFF8000000000000ull)) == 0xFFC00000u);
        // Compares and integer stores.
        CHECK(fcom_codes(0, 1.0, 2.0) == 0x0100u && fcom_codes(0, 2.0, 1.0) == 0u && fcom_codes(0, 1.0, 1.0) == 0x4000u);
        CHECK(fcom_codes(0x0200u, NAN, 1.0) == 0x4500u);
        CHECK(fist32(0x037F, 2.5) == 2u && fist32(0x0F7F, 2.5) == 2u && fist32(0x077F, -2.5) == uint32_t(-3) &&
              fist32(0x0B7F, 2.1) == 3u);
        CHECK(fist32(0x037F, 3e9) == 0x80000000u);
    }

    // ---------------------------------------------------------------- lifter
    Lifter lifter(dec);
    if (f.contains(0x11360)) {
        auto r = lifter.lift(f.at(0x11360));
        CHECK(r.has_value());
        if (r) {
            CHECK(r->contains("rt::rep_movs_back(m, esi, edi, ecx, 4u, 0x11361u);"));  // DF = 1 after std
            CHECK(r->contains("if (a != b) break;") && r->contains("if (ran) {") && r->contains("zf = r == 0;"));
            CHECK(r->contains("esi += 1u; edi += 1u;"));  // DF = 0 again after cld
        }
    }
    if (f.contains(0x11370)) {
        auto r = lifter.lift(f.at(0x11370));
        CHECK(r.has_value());
        if (r) {
            CHECK(r->contains("{ const double v_ = rt::fld32(rt::ld32(m, uint32_t(esp + 0x4u), 0x11370u)); ftop = (ftop - 1u) & 7u; "
                              "ftag |= 1u << ftop; ST(0u) = v_; }"));
            CHECK(r->contains("ST(0u) = rt::fmul(fpcw, ST(0u), rt::f32_raw(rt::ld32(m, uint32_t(esp + 0x8u), 0x11374u)));"));
            CHECK(r->contains("ST(1u) = rt::fadd(fpcw, ST(1u), ST(0u));"));
            CHECK(r->contains("zf = un_ || a_ == b_;") && r->contains("cf = un_ || a_ < b_;"));
            CHECK(r->contains("ST(0u) = rt::dfrom(rt::dbits(ST(0u)) ^ 0x8000000000000000ull);"));
            CHECK(r->contains("c.ftop = ftop; c.ftag = ftag;"));  // the return value stays on the x87 stack
        }
    }
    if (f.contains(0x11390)) {
        auto r = lifter.lift(f.at(0x11390));
        CHECK(r.has_value());
        if (r) {
            CHECK(r->contains("eax = rt::ld32(m, uint32_t(c.fs_base), 0x11397u);"));
            CHECK(r->contains("rt::st32(m, uint32_t(c.fs_base), esp, 0x1139Eu);"));
            CHECK(r->contains("rt::st32(m, uint32_t(c.fs_base), ecx, 0x113A8u);"));
            CHECK(r->contains("eax = rt::ld32(m, uint32_t(c.fs_base + ecx + 0x18u), 0x113AFu);"));
        }
    } else CHECK(false);
    if (f.contains(0x113C0)) {
        auto r = lifter.lift(f.at(0x113C0));
        CHECK(r.has_value());
        if (r) {
            CHECK(r->contains("const uint32_t r = (a << n) | (b >> (32u - n));"));  // shld
            CHECK(r->contains("const uint32_t n = (ecx & 31u);") && r->contains("const uint32_t r = (a >> n) | (b << (32u - n));"));
            CHECK(r->contains("const uint32_t c0 = cf ? 1u : 0u;"));               // rcr through carry
            CHECK(r->contains("if (ecx == 0u) { goto L_000113CE; }"));              // jecxz
            CHECK(r->contains("if (a == b) break;"));                                // repne scasb stops on a match
            CHECK(r->contains("ST(0u) = rt::frndint(fpcw, ST(0u));") && r->contains("ST(0u) = rt::fscale(ST(0u), ST(1u));"));
            CHECK(r->contains("fpsw = rt::fxam_codes(") && r->contains("fpsw &= 0x4700u;"));
            CHECK(r->contains("ST(0u) = rt::f2xm1(ST(0u));") && r->contains("rt::fist64(fpcw, ST(0u))"));
            CHECK(r->contains("rt::fld80(") && r->contains("rt::fnsave(m,") && r->contains("rt::frstor(m,"));
        }
    } else CHECK(false);
    {
        // 113EB and 113EE sit off the 16-byte grid the CFG wants for entries
        // here, so they're described by hand.
        Function out, in;
        out.start = 0x113EB;
        out.insns = {0x113EB, 0x113ED};
        in.start = 0x113EE;
        in.insns = {0x113EE, 0x113F0, 0x113F1};
        CHECK(!lifter.lift(in).has_value());  // without the convention: flags read before written
        Conventions conv;
        conv.flags_out = {{0x113EBu, uint8_t{8}}};
        conv.flags_in = {{0x113EEu, uint8_t{8}}};
        Lifter with(dec, &conv);
        auto o = with.lift(out);
        CHECK(o.has_value() && o->contains("c.eflags = 0u | (zf ? 0x40u : 0u);"));
        auto i = with.lift(in);
        CHECK(i.has_value() && i->contains("zf = (c.eflags & 0x40u) != 0;"));
    }
    {
        Function lib;
        lib.start = 0x120A0;
        lib.insns = {0x120A0, 0x120A4, 0x120A7, 0x120A9, 0x120AC, 0x120AF, 0x120B2, 0x120B4, 0x120B7, 0x120BC};
        auto r = lifter.lift(lib);
        CHECK(r.has_value());
        if (r) {
            CHECK(r->contains("const int32_t p = int32_t(int16_t((esi & 0xFFFFu))) * int32_t(int16_t("));  // 16-bit imul
            CHECK(r->contains("rt::set_lo16(esi, r);"));
            CHECK(r->contains("const uint32_t n0 = 5u;") && r->contains("n ? ((a << n) | (a >> (32 - n))) : a"));  // rol
            CHECK(r->contains("const uint32_t n0 = (ecx & 31u);") && r->contains("n ? ((a >> n) | (a << (32 - n))) : a"));  // ror
            CHECK(r->contains("mm0 = rt::ld64(m, uint32_t(esi), 0x120A9u);"));
            CHECK(r->contains("    uint64_t mm0 = c.mm[0]") && r->contains(" c.mm[0] = mm0;"));  // a local, stored at the exits
            CHECK(r->contains("ftop = 0u; ftag = 0xFFu;") && r->contains("ftag = 0u;"));  // MMX takes the x87 stack; emms frees it
            CHECK(r->contains("rt::st64(m, uint32_t(edi), mm0, 0x120AFu);"));
            CHECK(r->contains("rt::st128(m, uint32_t(edi), xmm0, true, 0x120B4u);"));  // movntps: aligned, as movaps
            CHECK(r->contains("eax = rt::mmio_read32(0xFE80200Cu, 0x120B7u);"));
        }
    }
    if (f.contains(0x11340)) {
        auto r = lifter.lift(f.at(0x11340));  // negative cases: unsigned values of ecx
        CHECK(r.has_value() && r->contains("case 4294967292u: goto L_00011353;") &&
              r->contains("case 4294967295u: goto L_0001135C;"));
    }
    if (f.contains(0x112A0)) {
        auto r = lifter.lift(f.at(0x112A0));  // the label is lifted as part of its function
        CHECK(r.has_value() && r->contains("L_000112B0"));
    }
    if (f.contains(0x11010)) {
        auto r = lifter.lift(f.at(0x11010));
        CHECK(r.has_value());
        if (r) {
            CHECK(r->contains("switch (eax)"));
            CHECK(r->contains("case 2u: goto L_00011026;"));
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
        // Calls: push the guest return address, write back, call, reload, check the return.
        auto r = lifter.lift(f.at(0x11000));
        CHECK(r.has_value());
        if (r) {
            CHECK(r->contains("esp -= 4u; rt::st32(m, esp, 0x11005u, 0x11000u); writeback(); fn_00011010(c); reload(); "
                              "rt::check_return(c, 0x11005u, 0x11000u); }"));
            CHECK(r->contains("fn_00011100(c);") && r->contains("fn_00011130(c);"));
            CHECK(r->contains("auto reload = [&] { esp = c.esp; };"));
        }
    }
    if (f.contains(0x112D0)) {
        auto r = lifter.lift(f.at(0x112D0));  // __ehhandler stub: mov eax, &FuncInfo; jmp __CxxFrameHandler
        CHECK(r.has_value() && r->contains("eax = 0x12040u;") && r->contains("writeback(); fn_00011130(c); return;  // tail call"));
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

    // ---------------------------------------------------------------- identify
    // The launcher's contract (identify.hpp): the supported build, another
    // game, another release, an incomplete or unreadable folder.
    {
        namespace fs = std::filesystem;
        auto with = [](uint32_t title_id, uint32_t timestamp, uint32_t region) {
            auto d = make_xbe();
            put32(d, 0x114, timestamp);
            put32(d, 0x408, title_id);
            put32(d, 0x4A0, region);  // the certificate's game region
            return d;
        };
        const auto right = with(kGhoulies.title_id, kGhoulies.timestamp, 1);
        Identity i = identify_xbe(right, "");
        CHECK(i.code == 0 && i.ok && i.title == "Grabbed by the Ghoulies (USA)" && i.id == "MS-083" && i.version == "0");
        CHECK(i.message == "This is Grabbed by the Ghoulies (USA), MS-083 (built 24 September 2003): the right disc.");

        i = identify_xbe(with(0x4D530004u, kGhoulies.timestamp, 1), "");
        CHECK(i.code == 3 && !i.ok && i.id == "MS-004" && i.title == "T\xF0\x9F\x98\x80 (USA)");
        CHECK(i.message.contains("(USA, MS-004), not Grabbed by the Ghoulies.") &&
              i.message.ends_with("This download is for Grabbed by the Ghoulies (USA), MS-083 (built 24 September 2003)."));

        i = identify_xbe(with(kGhoulies.title_id, kGhoulies.timestamp + 20 * 86400, 4), "");
        CHECK(i.code == 3 && !i.ok && i.id == "MS-083" && i.title == "T\xF0\x9F\x98\x80 (Europe)");
        CHECK(i.message.starts_with("This is Grabbed by the Ghoulies (Europe), but another release of it: its default.xbe was built 14 October 2003"));

        i = identify_xbe(right, "bundles");
        CHECK(i.code == 2 && !i.ok && i.title == "Grabbed by the Ghoulies (USA)" && i.message.contains("no bundles folder"));
        CHECK(identify_xbe(std::vector<uint8_t>{9, 0}, "").code == 2);
        CHECK(xbe_date(0) == "1 January 1970" && xbe_date(951782400) == "29 February 2000" && xbe_date(kGhoulies.timestamp) == "24 September 2003");

        // On a folder, as the launcher runs it (a Unicode name, any case).
        const fs::path dir = fs::temp_directory_path() / fs::path(u8"ghoulies-identify-\u4fdd\u5b58");
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir / "Bundles");
        fs::create_directories(dir / "xwavebank");
        auto put = [](const fs::path& path, std::span<const uint8_t> bytes) {
            std::ofstream output(path, std::ios::binary);
            output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        };
        put(dir / "default.xbe", right);
        i = identify(dir);
        CHECK(i.code == 2 && i.message.contains("no demand folder"));
        fs::create_directories(dir / "DEMAND");
        CHECK(identify(dir).code == 0);
        CHECK(identify(dir, "Grabbed by the Ghoulies (USA)").code == 0);
        CHECK(identify(dir / "default.xbe").code == 0);
        CHECK(identify(dir, "Grabbed by the Ghoulies (Europe)").code == 2);
        CHECK(identify(dir / "missing").code == 2);
        i = identify(dir / "Bundles");
        CHECK(i.code == 2 && i.message.starts_with("There's no default.xbe in this folder."));
        const uint8_t two[] = {9, 0};
        put(dir / "game.iso", two);
        i = identify(dir / "game.iso");
        CHECK(i.code == 2 && i.message.starts_with("This is a file, not a folder."));
        put(dir / "default.xbe", two);
        CHECK(identify(dir).code == 2);
        fs::remove_all(dir, ec);

#ifdef QILIN_EXE
        // The real qilin, started as the launcher starts it, on a folder
        // with a Japanese and an accented name: the name reaches it only
        // through its command line, which Windows gives a narrow main() in
        // the ANSI code page (command_line.hpp reads the UTF-16 one).
        {
            const fs::path unicode = fs::temp_directory_path() / fs::path(u8"ghoulies-identify-保存 été");
            fs::remove_all(unicode, ec);
            for (const char* sub : {"bundles", "demand", "xwavebank"}) fs::create_directories(unicode / sub);
            put(unicode / "default.xbe", right);
            const fs::path out = unicode / "identify.json";
            auto run_identify = [&](const fs::path& input) {
                fs::remove(out, ec);
#ifdef _WIN32
                const std::wstring line = L"\"\"" + qilin::native_path(QILIN_EXE).wstring() + L"\" identify \"" + input.wstring() +
                                          L"\" --json --build \"Grabbed by the Ghoulies (USA)\" > \"" + out.wstring() + L"\"\"";
                const int code = _wsystem(line.c_str());
#else
                const std::string line = std::string("'") + QILIN_EXE + "' identify '" + input.string() +
                                         "' --json --build 'Grabbed by the Ghoulies (USA)' > '" + out.string() + "'";
                const int status = std::system(line.c_str());
                const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
                std::ifstream in(out, std::ios::binary);
                return std::pair{code, std::string(std::istreambuf_iterator<char>(in), {})};
            };
            auto [code, text] = run_identify(unicode);
            CHECK(code == 0 && text.starts_with("{\"ok\": true, \"title\": \"Grabbed by the Ghoulies (USA)\""));
            std::tie(code, text) = run_identify(unicode / "default.xbe");
            CHECK(code == 0 && text.starts_with("{\"ok\": true"));
            // A name it can't find comes back in the message as it was given.
            const std::u8string missing = u8"é保存-missing";
            std::tie(code, text) = run_identify(unicode / fs::path(missing));
            CHECK(code == 2 && text.starts_with("{\"ok\": false") && text.contains(std::string(missing.begin(), missing.end())));
            fs::remove_all(unicode, ec);
        }
#endif

        Identity quoted;
        quoted.message = "a \"b\"\n";
        CHECK(to_json(quoted) == "{\"ok\": false, \"title\": \"\", \"id\": \"\", \"version\": \"\", \"message\": \"a \\\"b\\\"\\n\"}\n");
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
