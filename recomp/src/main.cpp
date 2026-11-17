// qilin: Grabbed by the Ghoulies static recompiler (work in progress).
//
//   qilin info  <default.xbe> [--json out.json]   XBE headers, sections, libraries, kernel imports
//   qilin cfg   <default.xbe> [--json out.json]   function discovery + control flow, summary
//   qilin insns <default.xbe> [--linear] [-o out] instruction listing for the LLVM cross-check
//   qilin suspects <default.xbe>                    kept functions with problems, for review
//   qilin lift  <default.xbe> --out <dir> [--range lo-hi] [--only addr]
//                                                   lift to C++ (generated code: never commit it)
//   qilin image <default.xbe> <out.bin>             flat guest image (for the differential tests)
//
// Exit status is non-zero on any error; nothing fails silently.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <optional>

#include "cfg.hpp"
#include "lifter.hpp"
#include "decoder.hpp"
#include "xbe.hpp"

using namespace qilin;

namespace {

int usage() {
    std::fputs(
        "usage:\n"
        "  qilin info  <xbe> [--json out.json]\n"
        "  qilin cfg   <xbe> [--json out.json]\n"
        "  qilin insns <xbe> [--linear] [-o out.txt]\n"
        "  qilin suspects <xbe>\n"
        "  qilin lift  <xbe> --out <dir> [--range lo-hi] [--only addr]\n"
        "  qilin image <xbe> <out.bin>\n",
        stderr);
    return 2;
}

const char* opt(const std::vector<std::string>& a, const char* name) {
    for (size_t i = 0; i + 1 < a.size(); ++i)
        if (a[i] == name) return a[i + 1].c_str();
    return nullptr;
}
bool flag(const std::vector<std::string>& a, const char* name) {
    for (auto& s : a)
        if (s == name) return true;
    return false;
}

void write_file(const char* path, const std::string& s) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error(std::string("cannot write ") + path);
    f << s;
}

// One line per instruction: "addr len bytes mnemonic<TAB>intel text".
void emit(FILE* out, const Decoder& dec, const Insn& i) {
    char hex[40];
    for (int k = 0; k < i.len; ++k) std::snprintf(hex + 2 * k, 3, "%02x", i.bytes[k]);
    std::fprintf(out, "%08x %u %s %s\t%s\n", i.addr, i.len, hex, Decoder::mnemonic_name(i.mnem), dec.format(i).c_str());
    if (const char* why = i.implausible_reason(); why && std::getenv("QILIN_SHOW_IMPLAUSIBLE"))
        std::fprintf(stderr, "implausible %08x %s: %s\n", i.addr, dec.format(i).c_str(), why);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    std::string cmd = argv[1];
    std::vector<std::string> args(argv + 3, argv + argc);
    try {
        Xbe xbe = Xbe::load(argv[2]);
        if (cmd == "info") {
            std::string js = xbe.to_json();
            if (auto p = opt(args, "--json")) write_file(p, js);
            std::puts(js.c_str());
            return 0;
        }
        Decoder dec(xbe);
        if (cmd == "cfg") {
            Cfg cfg(xbe, dec);
            cfg.run();
            std::fputs(cfg.summary().c_str(), stdout);
            if (auto p = opt(args, "--json")) write_file(p, cfg.to_json());
            return 0;
        }
        if (cmd == "suspects") {
            Cfg cfg(xbe, dec);
            cfg.run();
            for (auto& s : cfg.suspects()) {
                const XbeSection* sec = xbe.code_section_at(s.start);
                std::printf("%08x %-8s %s\n", s.start, sec ? sec->name.c_str() : "?", s.why.c_str());
            }
            for (uint32_t c : cfg.conflicts()) std::printf("%08x call target inside another instruction\n", c);
            return 0;
        }
        if (cmd == "image") {
            if (args.empty()) return usage();
            auto img = xbe.image();
            write_file(args[0].c_str(), std::string(reinterpret_cast<const char*>(img.data()), img.size()));
            std::printf("base %08x size %zu\n", xbe.base, img.size());
            return 0;
        }
        if (cmd == "lift") {
            const char* outdir = opt(args, "--out");
            if (!outdir) return usage();
            uint32_t lo = 0x11000, hi = 0x14A895;  // Ghoulies game code (see docs/code-analysis.md)
            if (auto r = opt(args, "--range")) {
                if (std::sscanf(r, "%x-%x", &lo, &hi) != 2) return usage();
            }
            std::optional<uint32_t> only;
            if (auto o = opt(args, "--only")) only = uint32_t(std::strtoul(o, nullptr, 16));
            Cfg cfg(xbe, dec);
            cfg.run();
            Lifter lifter(xbe, dec, cfg);
            std::filesystem::create_directories(outdir);
            std::vector<uint32_t> done;
            std::map<std::string, int> reasons;
            std::string failures;
            std::string chunk;
            int nchunk = 0;
            auto flush = [&] {
                if (chunk.empty()) return;
                std::string f = "// Generated by qilin lift. Do not edit, do not commit.\n"
                                "#include \"guest.hpp\"\n#include <cmath>\n#include <cstdint>\n\n"
                                "namespace qilin::gen {\nnamespace rt = qilin::rt;\n\n" + chunk + "}  // namespace qilin::gen\n";
                char name[64];
                std::snprintf(name, sizeof name, "/lifted_%03d.cpp", nchunk++);
                write_file((std::string(outdir) + name).c_str(), f);
                chunk.clear();
            };
            size_t total = 0, in_chunk = 0;
            for (auto& [a, fn] : cfg.functions()) {
                if (only ? a != *only : !(a >= lo && a < hi)) continue;
                ++total;
                LiftResult r = lifter.lift(fn);
                if (!r.ok) {
                    failures += r.error + "\n";
                    // bucket by reason (text after the instruction)
                    auto p = r.error.rfind(": ");
                    std::string why = p == std::string::npos ? r.error : r.error.substr(p + 2);
                    if (why.rfind("instruction not supported", 0) == 0 || why.rfind("x87", 0) == 0) {
                        auto q = r.error.find(' ');
                        auto mn = r.error.substr(q + 1, r.error.find_first_of(" :", q + 1) - q - 1);
                        why += " [" + mn + "]";
                    }
                    reasons[why]++;
                    continue;
                }
                chunk += r.code + "\n";
                done.push_back(a);
                if (++in_chunk == 200) { flush(); in_chunk = 0; }
            }
            flush();
            std::string table = "// Generated by qilin lift.\n#include \"guest.hpp\"\n#include <cstddef>\n\nnamespace qilin::gen {\n";
            for (uint32_t a : done) table += "void " + Lifter::name(a) + "(qilin::rt::Context&);\n";
            table += "struct Entry { uint32_t addr; void (*fn)(qilin::rt::Context&); };\nextern const Entry kFunctions[] = {\n";
            for (uint32_t a : done) { char b[64]; std::snprintf(b, sizeof b, "    {0x%08Xu, %s},\n", a, Lifter::name(a).c_str()); table += b; }
            table += "    {0u, nullptr}};\nextern const size_t kFunctionCount = " + std::to_string(done.size()) + ";\n}  // namespace qilin::gen\n";
            write_file((std::string(outdir) + "/table.cpp").c_str(), table);
            write_file((std::string(outdir) + "/failures.txt").c_str(), failures);
            std::printf("lifted %zu of %zu functions\n", done.size(), total);
            std::vector<std::pair<int, std::string>> rs;
            for (auto& [w, n] : reasons) rs.push_back({n, w});
            std::sort(rs.rbegin(), rs.rend());
            for (size_t k = 0; k < rs.size() && k < 25; ++k) std::printf("  %5d  %s\n", rs[k].first, rs[k].second.c_str());
            return 0;
        }
        if (cmd == "insns") {
            FILE* out = stdout;
            if (auto p = opt(args, "-o")) {
                out = std::fopen(p, "wb");
                if (!out) throw std::runtime_error(std::string("cannot write ") + p);
            }
            if (flag(args, "--linear")) {
                // Linear sweep of every code section; undecodable bytes are listed as "bad".
                for (auto& s : xbe.sections) {
                    if (!s.code) continue;
                    uint32_t a = s.va, end = s.va + s.rsize;
                    while (a < end) {
                        const Insn* i = dec.decode(a);
                        if (i && i->end() <= end) {
                            emit(out, dec, *i);
                            a = i->end();
                        } else {
                            // Undecodable: list the next (up to) 15 raw bytes so the checker can confirm.
                            std::fprintf(out, "%08x 0 ", a);
                            for (uint32_t k = 0; k < 15 && a + k < end; ++k) std::fprintf(out, "%02x", xbe.u8(a + k));
                            std::fprintf(out, " bad\t(bad)\n");
                            a += 1;
                        }
                    }
                }
            } else {
                Cfg cfg(xbe, dec);
                cfg.run();
                for (uint32_t a : cfg.all_instructions()) emit(out, dec, *dec.decode(a));
            }
            if (out != stdout) std::fclose(out);
            return 0;
        }
        return usage();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "qilin: error: %s\n", e.what());
        return 1;
    }
}
