// qilin: Grabbed by the Ghoulies static recompiler (work in progress).
//
//   qilin info  <default.xbe> [--json out.json]   XBE headers, sections, libraries, kernel imports
//   qilin cfg   <default.xbe> [--json out.json]   function discovery + control flow, summary
//   qilin insns <default.xbe> [--linear] [-o out] instruction listing for the LLVM cross-check
//
// Exit status is non-zero on any error; nothing fails silently.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "cfg.hpp"
#include "decoder.hpp"
#include "xbe.hpp"

using namespace qilin;

namespace {

int usage() {
    std::fputs(
        "usage:\n"
        "  qilin info  <xbe> [--json out.json]\n"
        "  qilin cfg   <xbe> [--json out.json]\n"
        "  qilin insns <xbe> [--linear] [-o out.txt]\n",
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
