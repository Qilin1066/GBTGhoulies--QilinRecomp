#include "xbe.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <string_view>
#include <tuple>

#include "json.hpp"

static_assert(std::endian::native == std::endian::little,
              "qilin reads little-endian guest data with memcpy: big-endian hosts are not supported");

namespace qilin {

namespace {

constexpr uint32_t kRetailEntryKey = 0xA8FC57AB;
constexpr uint32_t kDebugEntryKey = 0x94859D4B;
constexpr uint32_t kRetailThunkKey = 0x5B6D40B6;
constexpr uint32_t kDebugThunkKey = 0xEFB1F152;

// Sections that are not x86 code. DOLBY is APU DSP microcode.
constexpr std::array<std::string_view, 8> kNonCode = {".rdata",    ".data",     "BINKDATA",         "DOLBY",
                                                      "$$XTIMAGE", "$$XSIMAGE", "dirtydisk_bitmap", ".XTLID"};

// Internal: parse failures unwind to Xbe::parse, which turns them into an
// std::unexpected. Keeps the many bounds checks below to one line each.
struct ParseError {
    std::string what;
};

[[noreturn]] void bad(std::string what) { throw ParseError{std::move(what)}; }

struct Reader {
    std::span<const uint8_t> d;
    uint32_t u32(size_t o) const {
        if (o + 4 > d.size()) bad(std::format("read past end of file at offset {:#x}", o));
        uint32_t v;
        std::memcpy(&v, d.data() + o, 4);
        return v;
    }
    uint16_t u16(size_t o) const {
        if (o + 2 > d.size()) bad(std::format("read past end of file at offset {:#x}", o));
        uint16_t v;
        std::memcpy(&v, d.data() + o, 2);
        return v;
    }
};

// UTF-16LE (with surrogate pairs) to UTF-8; stops at NUL or max_units.
std::string utf16le_to_utf8(std::span<const uint8_t> p, size_t max_units) {
    std::string out;
    const size_t units = std::min(max_units, p.size() / 2);
    auto unit = [&](size_t i) -> uint32_t { return uint32_t(p[2 * i]) | (uint32_t(p[2 * i + 1]) << 8); };
    for (size_t i = 0; i < units; ++i) {
        uint32_t c = unit(i);
        if (c == 0) break;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < units) {
            uint32_t lo = unit(i + 1);
            if (lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        if (c >= 0xD800 && c < 0xE000) c = 0xFFFD;  // unpaired surrogate
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 0x3F));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

}  // namespace

std::expected<Xbe, std::string> Xbe::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::unexpected("cannot open " + path);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parse(data);
}

std::expected<Xbe, std::string> Xbe::parse(std::span<const uint8_t> d) try {
    Xbe x;
    Reader r{d};
    if (d.size() < 0x178 || std::memcmp(d.data(), "XBEH", 4) != 0) bad("not an XBE (bad magic)");

    x.base = r.u32(0x104);
    x.size_headers = r.u32(0x108);
    x.size_image = r.u32(0x10C);
    x.timestamp = r.u32(0x114);
    const uint32_t cert_addr = r.u32(0x118);
    const uint32_t num_sections = r.u32(0x11C);
    const uint32_t sect_addr = r.u32(0x120);
    x.init_flags = r.u32(0x124);
    const uint32_t ep_raw = r.u32(0x128);
    x.tls_addr = r.u32(0x12C);
    x.stack_commit = r.u32(0x130);
    x.heap_reserve = r.u32(0x134);
    x.heap_commit = r.u32(0x138);
    const uint32_t debug_path_addr = r.u32(0x14C);
    const uint32_t kt_raw = r.u32(0x158);
    const uint32_t num_libs = r.u32(0x160);
    const uint32_t libs_addr = r.u32(0x164);

    if (x.size_headers > d.size()) bad("header size exceeds file size");
    // Offset of a header-area pointer, checking that n bytes from it are inside the headers.
    auto hdr = [&](uint32_t va, uint64_t n = 1) -> size_t {
        if (va < x.base || uint64_t(va - x.base) + n > x.size_headers)
            bad(std::format("header pointer {:#x} outside the header area", va));
        return va - x.base;
    };
    auto cstr = [&](uint32_t va) {
        size_t o = hdr(va);
        auto end = std::find(d.begin() + o, d.begin() + x.size_headers, uint8_t(0));
        return std::string(d.begin() + o, end);
    };

    if (num_sections == 0 || num_sections > 256) bad("implausible section count");
    const size_t sect_off = hdr(sect_addr, uint64_t(num_sections) * 56);
    for (uint32_t i = 0; i < num_sections; ++i) {
        size_t o = sect_off + size_t(i) * 56;
        XbeSection s;
        s.flags = r.u32(o + 0);
        s.va = r.u32(o + 4);
        s.vsize = r.u32(o + 8);
        s.raw = r.u32(o + 12);
        s.rsize = r.u32(o + 16);
        s.name = cstr(r.u32(o + 20));
        if (uint64_t(s.raw) + s.rsize > d.size()) bad("section " + s.name + " extends past end of file");
        if (s.rsize > s.vsize) bad("section " + s.name + " raw size exceeds virtual size");
        if (s.va < x.base) bad("section " + s.name + " lies below the image base");
        if (uint64_t(s.va) + s.vsize > 0x100000000ull) bad("section " + s.name + " wraps the address space");
        s.code = !std::ranges::contains(kNonCode, std::string_view(s.name));
        x.sections.push_back(std::move(s));
    }

    // Retail vs debug: whichever key puts the entry point inside an executable section.
    for (auto [kind, epk, ktk] : {std::tuple{"retail", kRetailEntryKey, kRetailThunkKey},
                                  std::tuple{"debug", kDebugEntryKey, kDebugThunkKey}}) {
        const uint32_t ep = ep_raw ^ epk;
        if (std::ranges::any_of(x.sections, [&](const XbeSection& s) { return s.executable() && s.contains(ep); })) {
            x.kind = kind;
            x.entry = ep;
            x.kthunk = kt_raw ^ ktk;
            break;
        }
    }
    if (x.kind.empty()) bad("entry point does not decode with the retail or debug key");

    if (debug_path_addr) x.debug_path = cstr(debug_path_addr);

    {
        const size_t o = hdr(cert_addr, 0xB0);
        x.cert.title_id = r.u32(o + 0x08);
        x.cert.title_name = utf16le_to_utf8(d.subspan(o + 0x0C, 80), 40);
        x.cert.allowed_media = r.u32(o + 0x9C);
        x.cert.region = r.u32(o + 0xA0);
        x.cert.ratings = r.u32(o + 0xA4);
        x.cert.disk_number = r.u32(o + 0xA8);
        x.cert.version = r.u32(o + 0xAC);
    }

    if (num_libs > 1024) bad("implausible library count");
    const size_t libs_off = num_libs ? hdr(libs_addr, uint64_t(num_libs) * 16) : 0;
    for (uint32_t i = 0; i < num_libs; ++i) {
        const size_t p = libs_off + size_t(i) * 16;
        XbeLibrary l;
        auto nend = std::find(d.begin() + p, d.begin() + p + 8, uint8_t(0));
        l.name.assign(d.begin() + p, nend);
        l.major = r.u16(p + 8);
        l.minor = r.u16(p + 10);
        l.build = r.u16(p + 12);
        const uint16_t fl = r.u16(p + 14);
        l.qfe = fl & 0x1FFF;
        l.approved = (fl >> 13) & 3;
        l.debug = fl >> 15;
        x.libraries.push_back(std::move(l));
    }

    // Flat image: headers + every section's file-backed bytes, BSS zero.
    uint32_t end = x.base + x.size_headers;
    for (auto& s : x.sections) end = std::max(end, s.end());
    if (end - x.base > 64u * 1024 * 1024) bad("image larger than 64 MB");
    x.image_.assign(end - x.base, 0);
    std::memcpy(x.image_.data(), d.data(), x.size_headers);
    for (auto& s : x.sections) std::memcpy(x.image_.data() + (s.va - x.base), d.data() + s.raw, s.rsize);
    x.index_code_sections();

    // Kernel thunk table: ordinals with the high bit set, zero-terminated.
    for (uint32_t a = x.kthunk;; a += 4) {
        if (!x.mapped(a, 4)) bad("kernel thunk table is not inside the image");
        const uint32_t v = x.u32(a);
        if (v == 0) break;
        if (!(v & 0x80000000u)) bad("kernel thunk entry without the ordinal flag");
        x.kernel_imports.push_back(v & 0x7FFFFFFF);
        if (x.kernel_imports.size() > 400) bad("kernel thunk table not terminated");
    }
    return x;
} catch (const ParseError& e) {
    return std::unexpected(e.what);
}

void Xbe::index_code_sections() {
    code_index_.clear();
    for (size_t i = 0; i < sections.size(); ++i)
        if (sections[i].code && sections[i].rsize) code_index_.push_back(uint16_t(i));
    std::ranges::sort(code_index_, {}, [&](uint16_t i) { return sections[i].va; });
}

uint8_t Xbe::u8(uint32_t va) const {
    if (!mapped(va, 1)) throw XbeError(std::format("u8 read outside the image at {:#x}", va));
    return image_[va - base];
}

uint32_t Xbe::u32(uint32_t va) const {
    if (!mapped(va, 4)) throw XbeError(std::format("u32 read outside the image at {:#x}", va));
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
    // Last code section starting at or below va (code sections don't overlap).
    auto it = std::ranges::upper_bound(code_index_, va, {}, [&](uint16_t i) { return sections[i].va; });
    if (it == code_index_.begin()) return nullptr;
    const XbeSection& s = sections[*std::prev(it)];
    return va < s.file_end() ? &s : nullptr;
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
    j.kv("title_id_str", std::format("{:c}{:c}-{:03}", char(cert.title_id >> 24), char((cert.title_id >> 16) & 0xFF),
                                     cert.title_id & 0xFFFF));
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
        j.kv("version", std::format("{}.{}.{}", l.major, l.minor, l.build));
        j.kv("qfe", l.qfe);
        j.kv("approved", l.approved);
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
