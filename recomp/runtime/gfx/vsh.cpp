// Decoding and disassembling NV2A vertex-program microcode (vsh.hpp).
//
// Field layout of an instruction's four words (word, lowest bit, width),
// from the NV2A's documented-by-community vertex program format (as used by
// xemu and Cxbx-Reloaded): ILU (1, 25, 3), MAC (1, 21, 4), constant index
// (1, 13, 8), input index (1, 9, 4), A negate (1, 8), A swizzle (1, 0-7),
// A temp (2, 28, 4), A mux (2, 26, 2), B negate (2, 25), B swizzle (2,
// 17-24), B temp (2, 13, 4), B mux (2, 11, 2), C negate (2, 10), C swizzle
// (2, 2-9), C temp (2, 0, 2 high bits; 3, 30, 2 low bits), C mux (3, 28, 2),
// MAC temp mask (3, 24, 4), out temp (3, 20, 4), ILU temp mask (3, 16, 4),
// out mask (3, 12, 4), out to o/c (3, 11), out address (3, 3, 8), out from
// ILU (3, 2), a0.x relative (3, 1), final (3, 0).
#include <cstdio>

#include "vsh.hpp"

namespace qilin::gfx::vsh {
namespace {

uint32_t field(const uint32_t w[4], int word, int lo, int bits) { return (w[word] >> lo) & ((1u << bits) - 1u); }

Operand operand(const uint32_t w[4], int which, uint8_t input) {
    Operand o;
    uint32_t mux, temp, neg, sw;
    switch (which) {
        case 0: mux = field(w, 2, 26, 2); temp = field(w, 2, 28, 4); neg = field(w, 1, 8, 1); sw = field(w, 1, 0, 8); break;
        case 1: mux = field(w, 2, 11, 2); temp = field(w, 2, 13, 4); neg = field(w, 2, 25, 1); sw = field(w, 2, 17, 8); break;
        default:
            mux = field(w, 3, 28, 2);
            temp = (field(w, 2, 0, 2) << 2) | field(w, 3, 30, 2);
            neg = field(w, 2, 10, 1);
            sw = field(w, 2, 2, 8);
            break;
    }
    o.kind = Src(mux);
    o.index = o.kind == Src::Input ? input : uint8_t(temp);
    o.negate = neg != 0;
    for (int k = 0; k < 4; ++k) o.swizzle[k] = uint8_t((sw >> (6 - 2 * k)) & 3u);
    return o;
}

}  // namespace

Instruction decode_instruction(const uint32_t w[4]) {
    Instruction i;
    i.ilu = Ilu(field(w, 1, 25, 3));
    i.mac = Mac(field(w, 1, 21, 4));
    i.constant = uint8_t(field(w, 1, 13, 8));
    const uint8_t input = uint8_t(field(w, 1, 9, 4));
    i.a = operand(w, 0, input);
    i.b = operand(w, 1, input);
    i.c = operand(w, 2, input);
    i.mac_temp_mask = uint8_t(field(w, 3, 24, 4));
    i.temp = uint8_t(field(w, 3, 20, 4));
    i.ilu_temp_mask = uint8_t(field(w, 3, 16, 4));
    i.out_mask = uint8_t(field(w, 3, 12, 4));
    i.out_is_const = field(w, 3, 11, 1) == 0;  // 1 selects an o-register
    i.out_address = uint8_t(field(w, 3, 3, 8));
    i.out_from_ilu = field(w, 3, 2, 1) != 0;
    i.relative = field(w, 3, 1, 1) != 0;
    i.final = field(w, 3, 0, 1) != 0;
    return i;
}

std::optional<Program> decode(const uint32_t* words, size_t count, std::string* error) {
    if (count < 1) {
        *error = "empty function";
        return std::nullopt;
    }
    Program p;
    p.header = words[0];
    // Header: the type in the low 16 bits (0x2078 a vertex program, 0x7778 a
    // state program, 0x7378 a read/write state program), the instruction
    // count in the high 16.
    const uint32_t type = p.header & 0xFFFF, n = p.header >> 16;
    if (type != 0x2078 && type != 0x7778 && type != 0x7378) {
        char b[64];
        std::snprintf(b, sizeof b, "unknown program type %#x", type);
        *error = b;
        return std::nullopt;
    }
    if (n == 0 || n > 136 || 1 + 4 * size_t(n) > count) {
        *error = "bad instruction count";
        return std::nullopt;
    }
    for (uint32_t k = 0; k < n; ++k) p.code.push_back(decode_instruction(words + 1 + 4 * k));
    return p;
}

// ---------------------------------------------------------------- disassembly
namespace {

const char* kMac[] = {"nop", "mov", "mul", "add", "mad", "dp3", "dph", "dp4", "dst", "min", "max", "slt", "sge", "arl"};
const char* kIlu[] = {"nop", "mov", "rcp", "rcc", "rsq", "expp", "logp", "lit"};
int mac_args(Mac m) {
    switch (m) {
        case Mac::Mov: case Mac::Arl: return 1;
        case Mac::Mad: return 3;
        case Mac::Nop: return 0;
        default: return 2;
    }
}

std::string mask(uint8_t m) {
    if (m == 0xF) return "";
    std::string s = ".";
    if (m & 8) s += 'x';
    if (m & 4) s += 'y';
    if (m & 2) s += 'z';
    if (m & 1) s += 'w';
    return s;
}

std::string reg(const Instruction& i, const Operand& o) {
    std::string s = o.negate ? "-" : "";
    char b[32];
    switch (o.kind) {
        case Src::Temp: std::snprintf(b, sizeof b, o.index == 12 ? "oPos" : "r%u", o.index); break;
        case Src::Input: std::snprintf(b, sizeof b, "v%u", o.index); break;
        case Src::Const:
            if (i.relative) std::snprintf(b, sizeof b, "c[a0.x+%d]", int(i.constant) - 96);
            else std::snprintf(b, sizeof b, "c%d", int(i.constant) - 96);
            break;
        default: std::snprintf(b, sizeof b, "?%u", unsigned(o.kind)); break;
    }
    s += b;
    const char xyzw[] = "xyzw";
    const bool identity = o.swizzle[0] == 0 && o.swizzle[1] == 1 && o.swizzle[2] == 2 && o.swizzle[3] == 3;
    if (!identity) {
        s += '.';
        if (o.swizzle[0] == o.swizzle[1] && o.swizzle[1] == o.swizzle[2] && o.swizzle[2] == o.swizzle[3]) s += xyzw[o.swizzle[0]];
        else
            for (int k = 0; k < 4; ++k) s += xyzw[o.swizzle[k]];
    }
    return s;
}

std::string out_name(const Instruction& i) {
    if (i.out_is_const) return "c" + std::to_string(int(i.out_address) - 96);
    switch (i.out_address) {
        case kPos: return "oPos";
        case kD0: return "oD0";
        case kD1: return "oD1";
        case kFog: return "oFog";
        case kPts: return "oPts";
        case kB0: return "oB0";
        case kB1: return "oB1";
        case kT0: case kT1: case kT2: case kT3: return "oT" + std::to_string(i.out_address - kT0);
        default: return "o" + std::to_string(i.out_address);
    }
}

}  // namespace

std::string disassemble(const Program& p) {
    std::string s;
    for (const Instruction& i : p.code) {
        std::vector<std::string> parts;
        auto args = [&](const std::string& dst, int n, bool ilu) {
            std::string t = dst;
            // The ILU's one operand is C; the MAC's are A, B (and C for mad).
            if (ilu) return t + ", " + reg(i, i.c);
            if (n >= 1) t += ", " + reg(i, i.a);
            if (n >= 2) t += ", " + reg(i, i.mac == Mac::Add ? i.c : i.b);
            if (n >= 3) t += ", " + reg(i, i.c);
            return t;
        };
        if (i.mac != Mac::Nop) {
            const char* op = i.mac <= Mac::Arl ? kMac[size_t(i.mac)] : "mac?";
            if (i.mac == Mac::Arl) parts.push_back(std::string(op) + " " + args("a0.x", 1, false));
            if (i.mac != Mac::Arl && i.mac_temp_mask) parts.push_back(std::string(op) + " " + args("r" + std::to_string(i.temp) + mask(i.mac_temp_mask), mac_args(i.mac), false));
            if (i.mac != Mac::Arl && i.out_mask && !i.out_from_ilu)
                parts.push_back(std::string(op) + " " + args(out_name(i) + mask(i.out_mask), mac_args(i.mac), false));
        }
        if (i.ilu != Ilu::Nop) {
            const char* op = kIlu[size_t(i.ilu)];
            if (i.ilu_temp_mask) parts.push_back(std::string(op) + " " + args("r" + std::to_string(i.ilu_temp()) + mask(i.ilu_temp_mask), 1, true));
            if (i.out_mask && i.out_from_ilu) parts.push_back(std::string(op) + " " + args(out_name(i) + mask(i.out_mask), 1, true));
        }
        if (parts.empty()) parts.push_back("nop");
        for (size_t k = 0; k < parts.size(); ++k) s += (k ? "  + " : "") + parts[k] + "\n";
    }
    return s;
}

}  // namespace qilin::gfx::vsh
