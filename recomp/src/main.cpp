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
constexpr Range kGameCode{0x11000, 0x14A895};
constexpr Range kRareLibs{0x152780, 0x165C00};

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
    auto wanted = [&](uint32_t a) {
        if (only) return a == *only;
        return std::ranges::any_of(ranges, [&](const Range& r) { return a >= r.lo && a < r.hi; });
    };

    Cfg cfg(xbe, dec);
    cfg.run();
    Lifter lifter(dec);
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
    for (auto& [a, fn] : cfg.functions()) {
        if (!wanted(a)) continue;
        ++total;
        auto r = lifter.lift(fn);
        if (!r) {
            failures += r.error().message() + '\n';
            buckets[r.error().category()]++;
            continue;
        }
        chunk += *r + '\n';
        done.push_back(a);
        called.insert(fn.calls.begin(), fn.calls.end());
        called.insert(fn.tails.begin(), fn.tails.end());
        if (++in_chunk == 200) flush();
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
            std::println("{:<12} {:>7} {:>9} {:>8} {:>8} {:>8}  {:>8.3f}%", r.name, r.lifted ? "yes" : "no", r.code, r.table,
                         r.padding, r.unknown, 100.0 * double(total - r.unknown) / double(std::max<uint64_t>(1, total)));
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
