// XBE (original Xbox executable) loader.
//
// Parses the image header, certificate, section table, library versions and
// kernel thunk table, and builds a flat guest memory image (base .. end of the
// last section, BSS zero-filled) that the analysis passes read from.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace qilin {

struct XbeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct XbeSection {
    std::string name;
    uint32_t flags = 0;
    uint32_t va = 0;      // virtual address
    uint32_t vsize = 0;   // virtual size (>= rsize; tail is BSS)
    uint32_t raw = 0;     // file offset
    uint32_t rsize = 0;   // bytes present in the file
    bool writable() const { return flags & 1; }
    bool preload() const { return flags & 2; }
    bool executable() const { return flags & 4; }
    bool inserted() const { return flags & 8; }
    // Holds x86 code (as opposed to data, images, or APU DSP microcode).
    bool code = false;
    uint32_t end() const { return va + vsize; }
    bool contains(uint32_t a) const { return a >= va && a < va + vsize; }
};

struct XbeLibrary {
    std::string name;
    uint16_t major = 0, minor = 0, build = 0;
    uint16_t qfe = 0;
    uint8_t approved = 0;
    bool debug = false;
};

struct XbeCertificate {
    uint32_t title_id = 0;
    std::string title_name;  // UTF-8
    uint32_t allowed_media = 0;
    uint32_t region = 0;
    uint32_t ratings = 0;
    uint32_t disk_number = 0;
    uint32_t version = 0;
};

class Xbe {
public:
    // Throws XbeError on anything malformed.
    static Xbe load(const std::string& path);
    static Xbe parse(std::vector<uint8_t> file);

    // Header fields
    std::string kind;  // "retail" or "debug" (which XOR keys decoded the entry point)
    uint32_t base = 0, size_headers = 0, size_image = 0, timestamp = 0;
    uint32_t init_flags = 0, tls_addr = 0;
    uint32_t stack_commit = 0, heap_reserve = 0, heap_commit = 0;
    uint32_t entry = 0;   // decoded
    uint32_t kthunk = 0;  // decoded kernel thunk table address
    std::string debug_path;

    XbeCertificate cert;
    std::vector<XbeSection> sections;
    std::vector<XbeLibrary> libraries;
    std::vector<uint32_t> kernel_imports;  // ordinals, in thunk-slot order

    // Flat guest image: image()[va - base]. BSS is zero.
    std::span<const uint8_t> image() const { return image_; }
    uint32_t image_end() const { return base + static_cast<uint32_t>(image_.size()); }
    bool mapped(uint32_t va, uint32_t n = 1) const {
        return va >= base && uint64_t(va) + n <= image_end();
    }
    uint8_t u8(uint32_t va) const;
    uint32_t u32(uint32_t va) const;

    const XbeSection* section_at(uint32_t va) const;
    // Code section containing va, using the file-backed extent (va .. va+rsize).
    const XbeSection* code_section_at(uint32_t va) const;

    std::string to_json() const;

private:
    std::vector<uint8_t> file_;
    std::vector<uint8_t> image_;
};

}  // namespace qilin
