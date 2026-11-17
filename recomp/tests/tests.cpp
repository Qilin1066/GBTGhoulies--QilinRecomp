// Self-contained tests: build a tiny synthetic XBE in memory and run the
// loader, decoder and CFG over it. No game files required (runs in CI).
#include <cstdio>
#include <cstring>
#include <vector>

#include "cfg.hpp"
#include "decoder.hpp"
#include "lifter.hpp"
#include "xbe.hpp"

using namespace qilin;

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static void put32(std::vector<uint8_t>& d, size_t o, uint32_t v) { std::memcpy(&d[o], &v, 4); }

static std::vector<uint8_t> make_xbe() {
    const uint32_t base = 0x10000;
    std::vector<uint8_t> d(0x1200, 0);
    std::memcpy(d.data(), "XBEH", 4);
    put32(d, 0x104, base);
    put32(d, 0x108, 0x1000);       // size of headers
    put32(d, 0x10C, 0x3000);       // size of image
    put32(d, 0x118, base + 0x400); // certificate
    put32(d, 0x11C, 2);            // sections
    put32(d, 0x120, base + 0x200); // section headers
    put32(d, 0x128, 0x11000 ^ 0xA8FC57AB);  // entry (retail key)
    put32(d, 0x158, 0x12000 ^ 0x5B6D40B6);  // kernel thunk (retail key)
    // section headers (56 bytes each)
    auto sect = [&](int i, uint32_t flags, uint32_t va, uint32_t size, uint32_t raw, uint32_t name) {
        size_t o = 0x200 + i * 56;
        put32(d, o + 0, flags);
        put32(d, o + 4, va);
        put32(d, o + 8, size);
        put32(d, o + 12, raw);
        put32(d, o + 16, size);
        put32(d, o + 20, base + name);
    };
    sect(0, 6, 0x11000, 0x100, 0x1000, 0x300);
    sect(1, 2, 0x12000, 0x10, 0x1100, 0x308);
    std::memcpy(&d[0x300], ".text", 6);
    std::memcpy(&d[0x308], ".rdata", 7);
    // certificate
    put32(d, 0x408, 0x4D530053);
    const char16_t* name = u"Test";
    std::memcpy(&d[0x40C], name, 8);
    // kernel thunk table: one import (ordinal 1), terminated
    put32(d, 0x1100, 0x80000001);

    // code
    const uint8_t code[] = {
        0xE8, 0x0B, 0x00, 0x00, 0x00,              // 11000 call 11010
        0xC3,                                      // 11005 ret
        0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
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
    return d;
}

int main() {
    Xbe x = Xbe::parse(make_xbe());
    CHECK(x.kind == "retail");
    CHECK(x.entry == 0x11000);
    CHECK(x.kthunk == 0x12000);
    CHECK(x.kernel_imports.size() == 1 && x.kernel_imports[0] == 1);
    CHECK(x.cert.title_name == "Test");
    CHECK(x.sections.size() == 2 && x.sections[0].code && !x.sections[1].code);

    Decoder dec(x);
    const Insn* call = dec.decode(0x11000);
    CHECK(call && call->is_call() && call->len == 5 && call->rel_target() == 0x11010u);
    const Insn* jmp = dec.decode(0x11019);
    CHECK(jmp && jmp->is_jmp() && jmp->op[0].kind == Operand::Kind::Mem && jmp->op[0].scale == 4);
    CHECK(dec.decode(0x11017)->is_cond_branch());

    Cfg cfg(x, dec);
    cfg.run();
    auto& f = cfg.functions();
    CHECK(f.count(0x11000) && f.count(0x11010));
    if (f.count(0x11010)) {
        auto& fn = f.at(0x11010);
        CHECK(fn.jtabs.size() == 1);
        if (!fn.jtabs.empty()) {
            CHECK(fn.jtabs[0].how == "cmp");
            CHECK((fn.jtabs[0].targets == std::vector<uint32_t>{0x11020, 0x11023, 0x11026}));
        }
        CHECK(fn.insns.size() == 10);
    }

    // Lifter: the jump-table function lifts to a switch; the caller (not a leaf) is refused.
    Lifter lifter(x, dec, cfg);
    if (f.count(0x11010)) {
        LiftResult r = lifter.lift(f.at(0x11010));
        CHECK(r.ok);
        CHECK(r.code.find("switch (eax)") != std::string::npos);
        CHECK(r.code.find("case 2: goto L_00011026;") != std::string::npos);
        // cmp eax, 2 feeds only ja: CF and ZF are computed, OF/SF/AF are not.
        CHECK(r.code.find("cf = a < b;") != std::string::npos);
        CHECK(r.code.find("of = ((") == std::string::npos);
    }
    if (f.count(0x11000)) {
        LiftResult r = lifter.lift(f.at(0x11000));
        CHECK(!r.ok && r.error.find("not a leaf") != std::string::npos);
    }

    // Malformed input must throw, not misbehave.
    bool threw = false;
    try {
        auto d = make_xbe();
        d[0] = 'Y';
        Xbe::parse(d);
    } catch (const XbeError&) {
        threw = true;
    }
    CHECK(threw);

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all tests passed");
    return 0;
}
