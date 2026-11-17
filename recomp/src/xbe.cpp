#include "xbe.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <tuple>

#include "json.hpp"

namespace qilin {

namespace {

constexpr uint32_t kRetailEntryKey = 0xA8FC57AB;
constexpr uint32_t kDebugEntryKey = 0x94859D4B;
constexpr uint32_t kRetailThunkKey = 0x5B6D40B6;
constexpr uint32_t kDebugThunkKey = 0xEFB1F152;

// Sections that are not x86 code. DOLBY is APU DSP microcode.
const std::set<std::string> kNonCode = {".rdata", ".data", "BINKDATA", "DOLBY", "$$XTIMAGE",
                                        "$$XSIMAGE", "dirtydisk_bitmap", ".XTLID"};

uint32_t rd32(const std::vector<uint8_t>& d, size_t o) {
    if (o + 4 > d.size()) throw XbeError("read past end of file at offset " + std::to_string(o));
    uint32_t v;
    std::memcpy(&v, d.data() + o, 4);  // XBE and all targets are little-endian
    return v;
}
uint16_t rd16(const std::vector<uint8_t>& d, size_t o) {
    if (o + 2 > d.size()) throw XbeError("read past end of file at offset " + std::to_string(o));
    uint16_t v;
    std::memcpy(&v, d.data() + o, 2);
    return v;
}

std::string utf16le_to_utf8(const uint8_t* p, size_t max_chars) {
    std::string out;
    for (size_t i = 0; i < max_chars; ++i) {
        uint32_t c = p[2 * i] | (p[2 * i + 1] << 8);
        if (c == 0) break;
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

}  // namespace

Xbe Xbe::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw XbeError("cannot open " + path);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parse(std::move(data));
}

Xbe Xbe::parse(std::vector<uint8_t> d) {
    Xbe x;
    if (d.size() < 0x178 || std::memcmp(d.data(), "XBEH", 4) != 0) throw XbeError("not an XBE (bad magic)");

    x.base = rd32(d, 0x104);
    x.size_headers = rd32(d, 0x108);
    x.size_image = rd32(d, 0x10C);
    x.timestamp = rd32(d, 0x114);
    const uint32_t cert_addr = rd32(d, 0x118);
    const uint32_t num_sections = rd32(d, 0x11C);
    const uint32_t sect_addr = rd32(d, 0x120);
    x.init_flags = rd32(d, 0x124);
    const uint32_t ep_raw = rd32(d, 0x128);
    x.tls_addr = rd32(d, 0x12C);
    x.stack_commit = rd32(d, 0x130);
    x.heap_reserve = rd32(d, 0x134);
    x.heap_commit = rd32(d, 0x138);
    const uint32_t debug_path_addr = rd32(d, 0x14C);
    const uint32_t kt_raw = rd32(d, 0x158);
    const uint32_t num_libs = rd32(d, 0x160);
    const uint32_t libs_addr = rd32(d, 0x164);

    if (x.size_headers > d.size()) throw XbeError("header size exceeds file size");
    auto hdr = [&](uint32_t va) -> size_t {
        if (va < x.base || va - x.base >= x.size_headers)
            throw XbeError("header pointer outside header area");
        return va - x.base;
    };
    auto cstr = [&](uint32_t va) {
        size_t o = hdr(va);
        auto end = std::find(d.begin() + o, d.begin() + x.size_headers, uint8_t(0));
        return std::string(d.begin() + o, end);
    };

    if (num_sections == 0 || num_sections > 256) throw XbeError("implausible section count");
    for (uint32_t i = 0; i < num_sections; ++i) {
        size_t o = hdr(sect_addr) + size_t(i) * 56;
        XbeSection s;
        s.flags = rd32(d, o + 0);
        s.va = rd32(d, o + 4);
        s.vsize = rd32(d, o + 8);
        s.raw = rd32(d, o + 12);
        s.rsize = rd32(d, o + 16);
        s.name = cstr(rd32(d, o + 20));
        if (uint64_t(s.raw) + s.rsize > d.size()) throw XbeError("section " + s.name + " extends past end of file");
        if (s.rsize > s.vsize) throw XbeError("section " + s.name + " raw size exceeds virtual size");
        s.code = !kNonCode.count(s.name);
        x.sections.push_back(std::move(s));
    }

    // Retail vs debug: whichever key puts the entry point inside an executable section.
    for (auto [kind, epk, ktk] : {std::tuple{"retail", kRetailEntryKey, kRetailThunkKey},
                                  std::tuple{"debug", kDebugEntryKey, kDebugThunkKey}}) {
        uint32_t ep = ep_raw ^ epk;
        bool ok = std::any_of(x.sections.begin(), x.sections.end(),
                              [&](const XbeSection& s) { return s.executable() && s.contains(ep); });
        if (ok) {
            x.kind = kind;
            x.entry = ep;
            x.kthunk = kt_raw ^ ktk;
            break;
        }
    }
    if (x.kind.empty()) throw XbeError("entry point does not decode with retail or debug key");

    if (debug_path_addr) x.debug_path = cstr(debug_path_addr);

    {
        size_t o = hdr(cert_addr);
        if (o + 0xB0 > x.size_headers) throw XbeError("certificate truncated");
        x.cert.title_id = rd32(d, o + 0x08);
        x.cert.title_name = utf16le_to_utf8(d.data() + o + 0x0C, 40);
        x.cert.allowed_media = rd32(d, o + 0x9C);
        x.cert.region = rd32(d, o + 0xA0);
        x.cert.ratings = rd32(d, o + 0xA4);
        x.cert.disk_number = rd32(d, o + 0xA8);
        x.cert.version = rd32(d, o + 0xAC);
    }

    for (uint32_t i = 0; i < num_libs; ++i) {
        size_t p = hdr(libs_addr) + size_t(i) * 16;
        XbeLibrary l;
        auto nend = std::find(d.begin() + p, d.begin() + p + 8, uint8_t(0));
        l.name.assign(d.begin() + p, nend);
        l.major = rd16(d, p + 8);
        l.minor = rd16(d, p + 10);
        l.build = rd16(d, p + 12);
        uint16_t fl = rd16(d, p + 14);
        l.qfe = fl & 0x1FFF;
        l.approved = (fl >> 13) & 3;
        l.debug = fl >> 15;
        x.libraries.push_back(std::move(l));
    }

    // Flat image: headers + every section's file-backed bytes, BSS zero.
    uint32_t end = x.base + x.size_headers;
    for (auto& s : x.sections) end = std::max(end, s.end());
    if (end - x.base > 64u * 1024 * 1024) throw XbeError("image larger than 64 MB");
    x.image_.assign(end - x.base, 0);
    std::memcpy(x.image_.data(), d.data(), x.size_headers);
    for (auto& s : x.sections) {
        if (s.va < x.base) throw XbeError("section " + s.name + " below image base");
        std::memcpy(x.image_.data() + (s.va - x.base), d.data() + s.raw, s.rsize);
    }

    // Kernel thunk table: ordinals with the high bit set, zero-terminated.
    if (!x.mapped(x.kthunk, 4)) throw XbeError("kernel thunk table not mapped");
    for (uint32_t a = x.kthunk;; a += 4) {
        uint32_t v = x.u32(a);
        if (v == 0) break;
        if (!(v & 0x80000000u)) throw XbeError("kernel thunk entry without ordinal flag");
        x.kernel_imports.push_back(v & 0x7FFFFFFF);
        if (x.kernel_imports.size() > 400) throw XbeError("kernel thunk table not terminated");
    }

    x.file_ = std::move(d);
    return x;
}

uint8_t Xbe::u8(uint32_t va) const {
    if (!mapped(va, 1)) throw XbeError("u8 read outside image");
    return image_[va - base];
}

uint32_t Xbe::u32(uint32_t va) const {
    if (!mapped(va, 4)) throw XbeError("u32 read outside image");
    uint32_t v;
    std::memcpy(&v, image_.data() + (va - base), 4);
    return v;
}

const XbeSection* Xbe::section_at(uint32_t va) const {
    for (auto& s : sections)
        if (s.contains(va)) return &s;
    return nullptr;
}

const XbeSection* Xbe::code_section_at(uint32_t va) const {
    for (auto& s : sections)
        if (s.code && va >= s.va && va < s.va + s.rsize) return &s;
    return nullptr;
}

std::string Xbe::to_json() const {
    JsonWriter j(true);
    j.begin_object();
    j.kv("kind", kind);
    j.key("base").hex32(base);
    j.key("entry").hex32(entry);
    j.key("kernel_thunk").hex32(kthunk);
    j.kv("size_image", size_image);
    j.kv("stack_commit", stack_commit);
    j.kv("heap_reserve", heap_reserve);
    j.kv("heap_commit", heap_commit);
    j.key("init_flags").hex32(init_flags);
    j.key("tls_addr").hex32(tls_addr);
    j.kv("timestamp", timestamp);
    j.key("debug_path");
    if (debug_path.empty()) j.null(); else j.value(debug_path);
    j.key("certificate").begin_object();
    j.key("title_id").hex32(cert.title_id);
    {
        char b[16];
        std::snprintf(b, sizeof b, "%c%c-%03u", char(cert.title_id >> 24), char((cert.title_id >> 16) & 0xFF),
                      cert.title_id & 0xFFFF);
        j.kv("title_id_str", std::string(b));
    }
    j.kv("title_name", cert.title_name);
    j.key("allowed_media").hex32(cert.allowed_media);
    j.key("region").hex32(cert.region);
    j.kv("ratings", cert.ratings);
    j.kv("disk_number", cert.disk_number);
    j.kv("version", cert.version);
    j.end_object();
    j.key("sections").begin_array();
    for (auto& s : sections) {
        j.begin_object();
        j.kv("name", s.name);
        j.key("flags").hex32(s.flags);
        j.key("va").hex32(s.va);
        j.kv("vsize", s.vsize);
        j.key("raw").hex32(s.raw);
        j.kv("rsize", s.rsize);
        j.kv("writable", s.writable());
        j.kv("preload", s.preload());
        j.kv("executable", s.executable());
        j.kv("inserted", s.inserted());
        j.end_object();
    }
    j.end_array();
    j.key("libraries").begin_array();
    for (auto& l : libraries) {
        j.begin_object();
        j.kv("name", l.name);
        j.kv("version", std::to_string(l.major) + "." + std::to_string(l.minor) + "." + std::to_string(l.build));
        j.kv("qfe", unsigned(l.qfe));
        j.kv("approved", unsigned(l.approved));
        j.kv("debug", l.debug);
        j.end_object();
    }
    j.end_array();
    j.key("kernel_imports").begin_array();
    for (auto o : kernel_imports) j.value(o);
    j.end_array();
    j.end_object();
    return j.str();
}

}  // namespace qilin
