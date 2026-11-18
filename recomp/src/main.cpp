// qilin: Grabbed by the Ghoulies static recompiler (work in progress).
//
//   qilin info     <default.xbe> [--json out.json]   XBE headers, sections, libraries, kernel imports
//   qilin cfg      <default.xbe> [--json out.json]   function discovery + control flow, summary
//   qilin suspects <default.xbe>                     kept functions with problems, for review
//   qilin insns    <default.xbe> [--linear] [-o out] instruction listing for the LLVM cross-check
//   qilin lift     <default.xbe> --out <dir> [--range lo-hi] [--only addr]
//                                                    lift to C++ (generated code: never commit it)
//   qilin image    <default.xbe> <out.bin>           flat guest image (for the differential tests)
//   qilin coverage <default.xbe> [--list]            byte accounting: code / tables / padding / unknown
//
// Exit status is non-zero on any error; nothing fails silently.
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <memory>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cfg.hpp"
#include "decoder.hpp"
#include "lifter.hpp"
#include "xbe.hpp"

#ifndef QILIN_VERSION
#define QILIN_VERSION "unknown"
#endif

using namespace qilin;

namespace {

// Ghoulies code regions that get lifted (see docs/code-analysis.md). XAPI,
// the CRT and the XDK library sections are replaced natively instead.
struct Range {
    uint32_t lo, hi;
};
constexpr Range kGameCode{0x11000, 0x14A840};  // XAPI starts with IsBadReadPtr at 0x14A840
constexpr Range kRareLibs{0x152780, 0x165C00};

// Functions inside those regions that belong to a library we replace, and
// that only that library calls. They aren't lifted: once the library is
// replaced, nothing calls them. `qilin lift` fails if anything it lifts could
// reach one (see check_replaced).
struct Replaced {
    uint32_t addr;
    std::string_view why;
};
constexpr Replaced kReplaced[] = {
    {0x162570, "Bink helper; its one caller is in BINK (0x1A958C)"},
    {0x1626B0, "Bink helper; its one caller is in BINK (0x1A9571)"},
    {0x162840, "Bink's MMX probe (pushfd, cpuid); its one caller is in BINK (0x1AE916), and only BINK reads its result"},
};

// The C runtime (LIBCMT) and XAPI aren't lifted as regions: only the
// functions lifted code can reach are (library_closure), and the runtime
// implements the kernel beneath them. The CRT's hand-written assembly breaks
// the usual function contract in a few reviewed places (crt_conventions).
constexpr Range kCrt{0x165C00, 0x1707B0};
constexpr Range kXapi{0x14A840, 0x152780};
constexpr Range kXact{0x1BABA0, 0x1C57D0};   // the XACTENG section
constexpr Range kXgrph{0x185440, 0x1A59D8};  // the XGRPH section: XGraphics (resource headers, swizzling, maths)
constexpr Range kLibraries[] = {kXapi, kCrt, kXact, kXgrph};

// DirectSound is replaced natively (it drives the APU), except these pure
// helpers XACT calls, which compute without touching the hardware and are
// lifted so their results are the original's: XAudioCalculatePitch (x87
// log2) with its lock helpers and fyl2x core, and the PCM and Xbox ADPCM
// WAVEFORMATEX builders (each a jump stub and its body).
constexpr uint32_t kLiftedDsound[] = {0x1C5AF1, 0x1C5849, 0x1C586E, 0x1CB40D, 0x1C5B3B, 0x1C8BDF, 0x1C5B40, 0x1C8C29};

// Direct3D is replaced natively, except its resource code, which is pure
// computation over the game's allocator (0xF7270): creating textures,
// surfaces and vertex buffers, surface levels, descriptions and locks.
// Lifted, the headers, sizes, pitches and swizzled layouts are exactly the
// original's. What it calls that touches the GPU or the command buffer
// (D3D_BlockOnResource, MakeSpace, kick-offs, fences) stays native.
constexpr uint32_t kLiftedD3d[] = {
    0x177840,  // D3DDevice_CreateTexture2
    0x1779C0,  // D3DDevice_CreateVertexBuffer2
    0x175C90,  // D3DDevice_CreateSurface2
    0x17B5D0,  // the pixel-container header builder they share
    0x175C00,  // a texture's surface-level header
    0x177900,  // D3DTexture_GetSurfaceLevel2
    0x17B300,  // a level's size, pitch and offset
    0x177950,  // D3DTexture_LockRect
    0x175D40,  // D3DSurface_LockRect
    0x17B530,  // Lock2DSurface
    0x175D20,  // D3DSurface_GetDesc
    0x1778F0,  // D3DTexture_GetLevelDesc (a jump to Get2DSurfaceDesc)
    0x17B240,  // Get2DSurfaceDesc
    0x17B0F0,  // a pixel container's dimensions
    0x17AE00,  // format tables
    0x175A40,  // D3DResource_GetType
    0x175930,  // whether a surface is a level of a texture
    0x1759B0,  // D3DResource_AddRef
};

// Entry points the runtime calls itself: XAPI's process start-up (heap,
// drive letters), which XAPI's main-thread procedure (0x14FA75) runs first.
constexpr uint32_t kStartupRoots[] = {0x151011};

// Library functions the runtime replaces natively instead: never lifted, and
// the closure doesn't follow them. Each needs a registered replacement.
constexpr Replaced kNative[] = {
    {0x1517B9, "XAPI start-up patch of kernel code for known kernel builds (cli; mov [kernel], …; sti)"},
    {0x151B45, "XAPI's shutdown notification (registered with HalRegisterShutdownNotification at 0x32CF18): reloads the GDT; the runtime never shuts the console down"},
    {0x14B924, "OutputDebugStringA: int 0x2D, the kernel debugger's print service; the runtime prints the string"},
    {0x14C29C, "QueryPerformanceCounter: rdtsc; the runtime reads the host clock"},
};
bool is_native(uint32_t a) { return std::ranges::any_of(kNative, [&](const Replaced& r) { return r.addr == a; }); }

Conventions crt_conventions() {
    constexpr uint8_t ZF = 8;
    Conventions c;
    // x87 maths: a classifier returns ZF (operand is inf/NaN), the next
    // routine branches on it.
    c.flags_out = {{0x16B7A5, ZF}, {0x16B7E8, ZF}};  // load-and-classify helpers
    c.flags_in = {{0x16751D, ZF}, {0x1706FD, ZF}};    // asin / acos cores
    c.stack_glue = {
        0x165F80,  // strchr's shared exit: lea eax, [edx-1]; pop ebx; ret
        0x166D58,  // __SEH_prolog: builds the caller's frame
        0x166D93,  // __SEH_epilog: tears it down
        0x1675FB,  // pop eax; pop ecx; xchg [esp], eax; jmp eax (calls a __finally block)
        0x1679A8,  // __EH_prolog
        0x16B7FE,  // x87 maths epilogues: pop the saved control word, return
        0x16B80B,
        0x16B849,
    };
    return c;
}

struct Failure {
    std::string message;
};

int usage() {
    std::print(stderr,
               "qilin " QILIN_VERSION "\n"
               "usage:\n"
               "  qilin info     <xbe> [--json out.json]\n"
               "  qilin cfg      <xbe> [--json out.json]\n"
               "  qilin suspects <xbe>\n"
               "  qilin insns    <xbe> [--linear] [-o out.txt]\n"
               "  qilin lift     <xbe> --out <dir> [--range lo-hi] [--only addr]\n"
               "  qilin image    <xbe> <out.bin>\n"
               "  qilin coverage <xbe> [--list]   (exit 3 while any lifted byte is unaccounted for)\n"
               "  qilin --version\n");
    return 2;
}

std::optional<std::string_view> opt(std::span<const std::string_view> a, std::string_view name) {
    for (size_t i = 0; i + 1 < a.size(); ++i)
        if (a[i] == name) return a[i + 1];
    return std::nullopt;
}
bool flag(std::span<const std::string_view> a, std::string_view name) { return std::ranges::contains(a, name); }

std::optional<uint32_t> parse_hex(std::string_view s) {
    if (s.starts_with("0x") || s.starts_with("0X")) s.remove_prefix(2);
    uint32_t v = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v, 16);
    if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
    return v;
}

void write_file(const std::filesystem::path& path, std::string_view s) {
    std::ofstream f(path, std::ios::binary);
    if (!f || !f.write(s.data(), std::streamsize(s.size()))) throw Failure{"cannot write " + path.string()};
}

using File = std::unique_ptr<FILE, int (*)(FILE*)>;
File open_out(std::optional<std::string_view> path) {
    if (!path) return File(stdout, [](FILE*) { return 0; });
    FILE* f = std::fopen(std::string(*path).c_str(), "wb");
    if (!f) throw Failure{std::format("cannot write {}", *path)};
    return File(f, &std::fclose);
}

// One line per instruction: "addr len bytes mnemonic<TAB>intel text".
void emit(FILE* out, const Decoder& dec, const Insn& i) {
    std::string hex;
    for (uint8_t b : dec.bytes(i)) std::format_to(std::back_inserter(hex), "{:02x}", b);
    std::println(out, "{:08x} {} {} {}\t{}", i.addr, i.len, hex, Decoder::mnemonic_name(i.mnem), dec.format(i));
}

// The reasons a lifted function could reach a replaced one: a direct call or
// tail call, or its address as an immediate or displacement (a function
// pointer for an indirect call). A data section holding the address could
// feed an indirect call from anywhere, so that counts too.
std::vector<std::string> check_replaced(const Xbe& xbe, Decoder& dec, const Cfg& cfg, std::span<const uint32_t> lifted) {
    std::vector<std::string> out;
    for (const Replaced& r : kReplaced) {
        if (!cfg.functions().contains(r.addr)) out.push_back(std::format("{:08X} is no longer a function", r.addr));
        for (uint32_t a : lifted) {
            const Function& fn = cfg.functions().at(a);
            if (fn.calls.contains(r.addr) || fn.tails.contains(r.addr))
                out.push_back(std::format("{:08X} calls or tail-calls {:08X}", a, r.addr));
            for (uint32_t ia : fn.insns) {
                const Insn* i = dec.decode(ia);
                for (int k = 0; i && k < i->nops; ++k) {
                    const Operand& o = i->op[k];
                    if ((o.kind == Operand::Kind::Imm && !o.imm_relative && uint32_t(o.imm) == r.addr) ||
                        (o.kind == Operand::Kind::Mem && uint32_t(o.disp) == r.addr))
                        out.push_back(std::format("{:08X} refers to {:08X} at {:08X}", a, r.addr, ia));
                }
            }
        }
        for (const XbeSection& s : xbe.sections) {
            if (s.code) continue;
            for (uint32_t va = s.va; va + 4 <= s.file_end(); ++va)
                if (xbe.u32(va) == r.addr) out.push_back(std::format("{:08X} is stored in {} at {:08X}", r.addr, s.name, va));
        }
    }
    return out;
}

// Library (CRT, XAPI) functions reachable from `from` and the start-up
// roots: direct calls and tail calls, and function addresses as immediates or
// displacements (pointers for indirect calls), followed transitively; plus
// every library function whose address is stored in a data section (the
// CRT's own pointer tables, such as the floating-point formatting hooks
// printf calls through). Natively replaced functions aren't followed.
std::vector<uint32_t> library_closure(const Xbe& xbe, Decoder& dec, const Cfg& cfg, std::span<const uint32_t> from) {
    const auto& fns = cfg.functions();
    auto in_lib = [](uint32_t a) {
        return std::ranges::any_of(kLibraries, [&](const Range& r) { return a >= r.lo && a < r.hi; }) ||
               std::ranges::find(kLiftedDsound, a) != std::end(kLiftedDsound) ||
               std::ranges::find(kLiftedD3d, a) != std::end(kLiftedD3d);
    };
    std::set<uint32_t> seen;
    std::vector<uint32_t> work;
    auto add = [&](uint32_t t) {
        if (in_lib(t) && !is_native(t) && fns.contains(t) && seen.insert(t).second) work.push_back(t);
    };
    for (uint32_t r : kStartupRoots) add(r);
    auto refs = [&](const Function& fn) {
        for (uint32_t t : fn.calls) add(t);
        for (uint32_t t : fn.tails) add(t);
        for (uint32_t ia : fn.insns) {
            const Insn* i = dec.decode(ia);
            for (int k = 0; i && k < i->nops; ++k) {
                const Operand& o = i->op[k];
                if (o.kind == Operand::Kind::Imm && !o.imm_relative) add(uint32_t(o.imm));
                if (o.kind == Operand::Kind::Mem) add(uint32_t(o.disp));
            }
        }
    };
    for (uint32_t a : from) refs(fns.at(a));
    for (const XbeSection& s : xbe.sections) {
        if (s.code) continue;
        for (uint32_t va = s.va; va + 4 <= s.file_end(); ++va) add(xbe.u32(va));
    }
    while (!work.empty()) {
        const uint32_t a = work.back();
        work.pop_back();
        refs(fns.at(a));
    }
    return {seen.begin(), seen.end()};
}

int cmd_lift(const Xbe& xbe, Decoder& dec, std::span<const std::string_view> args) {
    auto outdir = opt(args, "--out");
    if (!outdir) return usage();
    std::vector<Range> ranges{kGameCode, kRareLibs};
    if (auto r = opt(args, "--range")) {
        auto dash = r->find('-');
        auto lo = parse_hex(r->substr(0, dash)), hi = dash == std::string_view::npos ? std::nullopt : parse_hex(r->substr(dash + 1));
        if (!lo || !hi) return usage();
        ranges = {{*lo, *hi}};
    }
    std::optional<uint32_t> only;
    if (auto o = opt(args, "--only")) {
        only = parse_hex(*o);
        if (!only) return usage();
    }
    auto replaced = [](uint32_t a) { return std::ranges::any_of(kReplaced, [&](const Replaced& r) { return r.addr == a; }); };
    auto wanted = [&](uint32_t a) {
        if (only) return a == *only;
        return !replaced(a) && std::ranges::any_of(ranges, [&](const Range& r) { return a >= r.lo && a < r.hi; });
    };

    Cfg cfg(xbe, dec);
    cfg.run();
    const Conventions conv = crt_conventions();
    Lifter lifter(dec, &conv);
    const std::filesystem::path dir(*outdir);
    std::filesystem::create_directories(dir);

    std::vector<uint32_t> done;
    std::set<uint32_t> called;  // direct call and tail-call targets of lifted code
    std::map<std::string, int> buckets;
    std::string failures, chunk;
    int nchunk = 0, in_chunk = 0, total = 0;
    auto flush = [&] {
        if (chunk.empty()) return;
        write_file(dir / std::format("lifted_{:03}.cpp", nchunk++),
                   "// Generated by qilin lift. Do not edit, do not commit.\n"
                   "#include \"guest.hpp\"\n#include \"functions.hpp\"\n\n"
                   "namespace qilin::gen {\nnamespace rt = qilin::rt;\n\n" + chunk + "}  // namespace qilin::gen\n");
        chunk.clear();
        in_chunk = 0;
    };
    int lib_failed = 0;
    auto lift_one = [&](uint32_t a, const Function& fn) {
        ++total;
        auto r = lifter.lift(fn);
        if (!r) {
            failures += r.error().message() + '\n';
            buckets[r.error().category()]++;
            return false;
        }
        chunk += *r + '\n';
        done.push_back(a);
        called.insert(fn.calls.begin(), fn.calls.end());
        called.insert(fn.tails.begin(), fn.tails.end());
        if (++in_chunk == 200) flush();
        return true;
    };
    for (auto& [a, fn] : cfg.functions())
        if (wanted(a)) lift_one(a, fn);
    // The library (CRT, XAPI) functions lifted code can reach, transitively (default run only).
    size_t lib_n = 0;
    if (!only && !opt(args, "--range")) {
        std::vector<uint32_t> lib = library_closure(xbe, dec, cfg, done);
        lib_n = lib.size();
        for (uint32_t a : lib)
            if (!lift_one(a, cfg.functions().at(a))) ++lib_failed;
        std::ranges::sort(done);
    }
    flush();

    // Targets without a lifted body (library code, game code that doesn't
    // lift yet) get a stub that forwards to rt::call_unlifted, so every
    // direct call links.
    std::vector<uint32_t> stubs;
    std::ranges::set_difference(called, done, std::back_inserter(stubs));
    std::string decls = "// Generated by qilin lift.\n#pragma once\n#include \"guest.hpp\"\n\nnamespace qilin::gen {\n";
    for (uint32_t a : done) decls += std::format("void {}(qilin::rt::Context&);\n", Lifter::name(a));
    decls += "// Stubs: no lifted body.\n";
    for (uint32_t a : stubs) decls += std::format("void {}(qilin::rt::Context&);\n", Lifter::name(a));
    decls += "}  // namespace qilin::gen\n";
    write_file(dir / "functions.hpp", decls);
    std::string stub_src = "// Generated by qilin lift.\n#include \"functions.hpp\"\n\nnamespace qilin::gen {\n";
    for (uint32_t a : stubs)
        stub_src += std::format("void {}(qilin::rt::Context& c) {{ qilin::rt::call_unlifted(c, 0x{:08X}u); }}\n", Lifter::name(a), a);
    stub_src += "}  // namespace qilin::gen\n";
    write_file(dir / "stubs.cpp", stub_src);

    // Dispatch table for indirect calls, sorted by address (binary-searchable).
    std::string table = "// Generated by qilin lift.\n#include \"functions.hpp\"\n\nnamespace qilin::gen {\n";
    table += "extern const Entry kFunctions[] = {\n";
    for (uint32_t a : done) table += std::format("    {{0x{:08X}u, {}}},\n", a, Lifter::name(a));
    table += std::format("    {{0u, nullptr}}}};\nextern const size_t kFunctionCount = {};\n}}  // namespace qilin::gen\n", done.size());
    write_file(dir / "table.cpp", table);
    write_file(dir / "failures.txt", failures);

    std::println("lifted {} of {} functions ({} call targets without a lifted body get stubs)", done.size(), total, stubs.size());
    if (lib_n) std::println("  {} of them are C runtime, XAPI, XACT, XGraphics, DirectSound-helper and D3D-resource functions that lifted code can reach", lib_n);
    if (lib_failed) {
        std::println(stderr, "qilin: error: {} reachable library functions don't lift (see failures.txt)", lib_failed);
        return 3;
    }
    if (!only) {
        size_t n = 0;
        for (const Replaced& r : kReplaced)
            n += std::ranges::any_of(ranges, [&](const Range& g) { return r.addr >= g.lo && r.addr < g.hi; });
        if (n) std::println("  {} functions in these regions belong to a replaced library and aren't lifted", n);
        if (auto bad = check_replaced(xbe, dec, cfg, done); !bad.empty()) {
            for (auto& b : bad) std::println(stderr, "qilin: error: a replaced function is reachable from lifted code: {}", b);
            return 3;
        }
    }
    std::vector<std::pair<int, std::string>> rs;
    for (auto& [w, n] : buckets) rs.emplace_back(n, w);
    std::ranges::sort(rs, std::greater{});
    for (auto& [n, w] : rs | std::views::take(25)) std::println("  {:5}  {}", n, w);
    return 0;
}

int run(std::string_view cmd, const std::string& path, std::span<const std::string_view> args) {
    auto loaded = Xbe::load(path);
    if (!loaded) {
        std::println(stderr, "qilin: error: {}", loaded.error());
        return 1;
    }
    const Xbe& xbe = *loaded;
    if (cmd == "info") {
        const std::string js = xbe.to_json();
        if (auto p = opt(args, "--json")) write_file(std::string(*p), js);
        std::println("{}", js);
        return 0;
    }
    if (cmd == "image") {
        if (args.empty()) return usage();
        auto img = xbe.image();
        write_file(std::string(args[0]), std::string_view(reinterpret_cast<const char*>(img.data()), img.size()));
        std::println("base {:08x} size {}", xbe.base, img.size());
        return 0;
    }
    Decoder dec(xbe);
    if (cmd == "cfg") {
        Cfg cfg(xbe, dec);
        cfg.run();
        std::print("{}", cfg.summary());
        if (auto p = opt(args, "--json")) write_file(std::string(*p), cfg.to_json());
        return 0;
    }
    if (cmd == "suspects") {
        Cfg cfg(xbe, dec);
        cfg.run();
        for (auto& s : cfg.suspects()) {
            const XbeSection* sec = xbe.code_section_at(s.start);
            std::println("{:08x} {:<8} {}", s.start, sec ? sec->name : "?", s.why);
        }
        for (uint32_t c : cfg.conflicts()) std::println("{:08x} call target inside another instruction", c);
        return 0;
    }
    if (cmd == "insns") {
        File out = open_out(opt(args, "-o"));
        if (flag(args, "--linear")) {
            // Linear sweep of every code section; undecodable bytes are listed as "bad"
            // with up to 15 following bytes (within the section) so a checker can confirm.
            Insn ins;
            for (auto& s : xbe.sections) {
                if (!s.code) continue;
                for (uint32_t a = s.va, end = s.file_end(); a < end;) {
                    if (dec.decode_uncached(a, ins) && ins.end() <= end) {
                        emit(out.get(), dec, ins);
                        a = ins.end();
                    } else {
                        std::string hex;
                        for (uint32_t k = 0; k < 15 && a + k < end; ++k) std::format_to(std::back_inserter(hex), "{:02x}", xbe.u8(a + k));
                        std::println(out.get(), "{:08x} 0 {} bad\t(bad)", a, hex);
                        a += 1;
                    }
                }
            }
        } else {
            Cfg cfg(xbe, dec);
            cfg.run();
            for (uint32_t a : cfg.all_instructions()) emit(out.get(), dec, *dec.decode(a));
        }
        return 0;
    }
    if (cmd == "coverage") {
        Cfg cfg(xbe, dec);
        cfg.run();
        const bool list = flag(args, "--list");
        std::println("{:<12} {:>7} {:>9} {:>8} {:>8} {:>8}  {:>8}", "region", "lifted", "code", "tables", "padding", "UNKNOWN", "accounted");
        uint64_t lifted_unknown = 0;
        for (auto& r : cfg.coverage()) {
            const uint64_t total = r.code + r.table + r.padding + r.unknown;
            std::println("{:<12} {:>7} {:>9} {:>8} {:>8} {:>8}  {:>8}%", r.name, r.lifted ? "yes" : "no", r.code, r.table,
                         r.padding, r.unknown, percent(total - r.unknown, total, 3));
            if (r.lifted) lifted_unknown += r.unknown;
            if (list && r.lifted)
                for (auto& g : r.unknown_spans) std::println("    unknown {:08x} +{}", g.addr, g.size);
        }
        std::println("unknown bytes in lifted regions: {}", lifted_unknown);
        return lifted_unknown ? 3 : 0;
    }
    if (cmd == "lift") return cmd_lift(xbe, dec, args);
    return usage();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && (std::string_view(argv[1]) == "--version" || std::string_view(argv[1]) == "version")) {
        std::println("qilin {}", QILIN_VERSION);
        return 0;
    }
    if (argc < 3) return usage();
    const std::vector<std::string_view> args(argv + 3, argv + argc);
    try {
        return run(argv[1], argv[2], args);
    } catch (const Failure& f) {
        std::println(stderr, "qilin: error: {}", f.message);
    } catch (const std::exception& e) {
        std::println(stderr, "qilin: internal error: {}", e.what());
    }
    return 1;
}
