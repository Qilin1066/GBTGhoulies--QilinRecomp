// Tests for the runtime's host side; no game files needed.
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <cstdio>
#include <string>

#include "crypto.hpp"
#include "strf.hpp"
#include "host.hpp"

using namespace qilin;

namespace {
int failures = 0;
#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                  \
        }                                                                                \
    } while (0)

// A guest with 1 MB of RAM for the I/O manager's tests: a bump allocator for
// strings and structures, and stdcall calls into the kernel replacements.
struct Guest {
    std::unique_ptr<uint8_t[]> ram = std::make_unique<uint8_t[]>(0x100000);
    rt::Context c{};
    uint32_t next = 0x1000;
    Guest() {
        std::memset(ram.get(), 0, 0x100000);
        c.mem = ram.get();
        c.mem_size = 0x100000;
    }
    uint32_t alloc(uint32_t n) {
        const uint32_t at = next;
        next = (next + n + 15) & ~15u;
        return at;
    }
    uint32_t u32(uint32_t a) const {
        uint32_t v;
        std::memcpy(&v, ram.get() + a, 4);
        return v;
    }
    void put32(uint32_t a, uint32_t v) { std::memcpy(ram.get() + a, &v, 4); }
    uint32_t bytes(const void* p, uint32_t n) {
        const uint32_t at = alloc(n);
        std::memcpy(ram.get() + at, p, n);
        return at;
    }
    uint32_t object_string(const std::string& s) {  // Length, MaximumLength, Buffer
        const uint32_t buf = bytes(s.c_str(), uint32_t(s.size() + 1)), str = alloc(8);
        const uint16_t len = uint16_t(s.size()), max = uint16_t(s.size() + 1);
        std::memcpy(ram.get() + str, &len, 2);
        std::memcpy(ram.get() + str + 2, &max, 2);
        put32(str + 4, buf);
        return str;
    }
    uint32_t attributes(const std::string& name, uint32_t root = 0) {  // OBJECT_ATTRIBUTES
        const uint32_t oa = alloc(12);
        put32(oa, root);
        put32(oa + 4, object_string(name));
        put32(oa + 8, 0x40);  // OBJ_CASE_INSENSITIVE
        return oa;
    }
    uint32_t call(const char* fn, std::initializer_list<uint32_t> args) {
        const host::KernelExport* e = host::kernel_export(std::string_view(fn));
        c.esp = 0xF0000 - 4 * uint32_t(args.size() + 1);
        put32(c.esp, 0x5555);
        uint32_t k = 4;
        for (uint32_t a : args) put32(c.esp + (k += 4) - 4, a);
        rt::call_unlifted(c, host::kernel_addr(e->ordinal));
        CHECK(c.ret_to == 0x5555 && c.esp == 0xF0000);  // stdcall: every argument popped
        return c.eax;
    }
};

uint32_t g_seen = 0;
void fake_create_file(rt::Context& c) {  // stdcall, two arguments: returns their sum
    g_seen = host::arg(c, 0) + host::arg(c, 1);
    host::ret(c, g_seen, 8);
}
}  // namespace

int main() {
    // The kernel export table: names, argument sizes, data exports, fastcall.
    const host::KernelExport* e = host::kernel_export(190);
    CHECK(e && std::string(e->name) == "NtCreateFile" && e->arg_bytes == 36 && !e->data);
    e = host::kernel_export(156);
    CHECK(e && std::string(e->name) == "KeTickCount" && e->data);
    e = host::kernel_export(160);
    CHECK(e && std::string(e->name) == "KfRaiseIrql" && e->fastcall);
    CHECK(host::kernel_export(0) == nullptr && host::kernel_export(4000) == nullptr);
    CHECK(host::describe(host::kernel_addr(190)) == "kernel NtCreateFile (ordinal 190)");
    host::set_regions({{0x11000, 0x14A840, "game"}});
    CHECK(host::describe(0x12345) == "game code at 00012345");

    // A registered replacement runs through rt::call_unlifted with stdcall
    // conventions: arguments from the stack, ret pops them and the return address.
    auto ram = std::make_unique<uint8_t[]>(0x10000);
    rt::Context c{};
    c.mem = ram.get();
    c.mem_size = 0x10000;
    c.esp = 0x8000 - 12;
    const uint32_t frame[3] = {0x1234u, 40u, 2u};  // return address, then the arguments
    std::memcpy(ram.get() + c.esp, frame, sizeof frame);
    host::register_hle(host::kernel_addr(190), "NtCreateFile", fake_create_file);
    CHECK(host::hle_count() == 1);
    rt::call_unlifted(c, host::kernel_addr(190));
    CHECK(g_seen == 42 && c.eax == 42 && c.ret_to == 0x1234u && c.esp == 0x8000);

    // Virtual memory: 64 KB reservations, committed pages zeroed, queries.
    host::VirtualMemory v;
    auto vram = std::make_unique<uint8_t[]>(0x100000);
    std::memset(vram.get(), 0xAB, 0x100000);
    v.init(vram.get(), 0x10000, 0x100000);
    const uint32_t r1 = v.reserve(0, 0x3000, 4, false), r2 = v.reserve(0, 0x1000, 4, false), r3 = v.reserve(0, 0x1000, 4, true);
    CHECK(r1 == 0x10000 && r2 == 0x20000 && r3 == 0xFF000 - 0xF000);  // granularity 64 KB; top down from the end
    CHECK(v.reserve(0x20000, 0x1000, 4, false) == 0);                 // taken
    CHECK(v.commit(r1 + 0x1800, 0x10, 4) && vram[r1 + 0x1000] == 0 && vram[r1] == 0xAB);  // only the touched page, zeroed
    auto q = v.query(r1 + 0x1004);
    CHECK(q && q->base == r1 + 0x1000 && q->size == 0x1000 && q->state == host::VirtualMemory::kMemCommit && q->allocation_base == r1);
    q = v.query(r1);
    CHECK(q && q->state == host::VirtualMemory::kMemReserve && q->size == 0x1000);
    q = v.query(0x30000);
    CHECK(q && q->state == host::VirtualMemory::kMemFree && q->size == r3 - 0x30000);
    CHECK(!v.commit(r1 + 0x2000, 0x2000, 4));  // runs past the reservation
    CHECK(v.committed_bytes() == 0x1000 && v.reserved_bytes() == 0x5000);
    vram[r1 + 0x1000] = 7;
    CHECK(v.decommit(r1 + 0x1000, 0x1000) && v.commit(r1 + 0x1000, 1, 4) && vram[r1 + 0x1000] == 0);  // recommitted: zero again
    CHECK(v.release(r1) && !v.release(r1) && v.reserve(0, 0x1000, 4, false) == 0x10000);

    // Guest RAM: the same pages at 0, 0x80000000 and 0xF0000000, zero to start.
    {
        std::string error;
        uint8_t* g = host::map_guest_ram(0x100000, &error);
        CHECK(g != nullptr);
        if (g) {
            CHECK(g[0x1234] == 0 && g[rt::kRamCachedAlias + 0xFFFFF] == 0);
            g[0x1234] = 0x5A;
            CHECK(g[rt::kRamCachedAlias + 0x1234] == 0x5A && g[rt::kRamWriteCombinedAlias + 0x1234] == 0x5A);
            g[rt::kRamWriteCombinedAlias + 0xFFFF0] = 0xA5;
            CHECK(g[0xFFFF0] == 0xA5);
            // The virtual memory above RAM, on the host's pages: committed
            // pages read zero, and again after a decommit (whose pages the
            // host side keeps, to give out again), a release, and a
            // commit across pages kept and pages never used.
            host::VirtualMemory hv;
            hv.init(g, 0x100000, 0x400000, true);
            const uint32_t a = hv.reserve(0, 0x40000, 4, false);
            CHECK(a == 0x100000 && hv.commit(a, 0x8000, 4));
            bool zero = true;
            for (uint32_t k = 0; k < 0x8000; ++k) zero = zero && g[a + k] == 0;
            CHECK(zero);
            std::memset(g + a, 0xEE, 0x8000);
            CHECK(hv.decommit(a + 0x2000, 0x4000) && g[a + 0x1FFF] == 0xEE && g[a + 0x6000] == 0xEE);
            CHECK(hv.commit(a + 0x1000, 0x20000, 4));  // some still committed, some kept, most new
            zero = g[a + 0x1000] == 0xEE && g[a + 0x7FFF] == 0xEE;
            for (uint32_t k = 0x2000; k < 0x6000; ++k) zero = zero && g[a + k] == 0;
            for (uint32_t k = 0x8000; k < 0x21000; ++k) zero = zero && g[a + k] == 0;
            CHECK(zero);
            std::memset(g + a, 0xDD, 0x21000);
            CHECK(hv.release(a));
            const uint32_t b = hv.reserve(0, 0x40000, 4, false);
            CHECK(b == a && hv.commit(b, 0x40000, 4));
            zero = true;
            for (uint32_t k = 0; k < 0x40000; ++k) zero = zero && g[b + k] == 0;
            CHECK(zero);
            CHECK(hv.release(b));
        } else {
            std::fprintf(stderr, "map_guest_ram: %s\n", error.c_str());
        }
        rt::g_ram_aliases = false;
    }

    // The I/O manager, over a disc and a hard disk in a scratch directory.
    {
        namespace fs = std::filesystem;
        const fs::path root = fs::temp_directory_path() / "qilin_host_tests_io";
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root / "disc" / "Bundles");
        std::ofstream(root / "disc" / "Bundles" / "Common.bnl", std::ios::binary) << "hello disc";
        std::ofstream(root / "disc" / "Bundles" / "notes.txt", std::ios::binary) << "x";
        host::register_kernel_io({root / "disc", root / "hdd"});
        Guest g;
        constexpr uint32_t kDos = 0xFFFFFFFD;  // ObDosDevicesDirectory()
        const uint32_t handle = g.alloc(4), iosb = g.alloc(8), buf = g.alloc(0x1000), off = g.alloc(8);

        CHECK(g.call("IoCreateSymbolicLink", {g.object_string("\\??\\D:"), g.object_string("\\Device\\CdRom0")}) == 0);
        CHECK(g.call("IoCreateSymbolicLink", {g.object_string("\\??\\D:"), g.object_string("\\Device\\CdRom0")}) == 0xC0000035);

        // A disc file through the drive letter, matched case-insensitively;
        // reads at an offset and then from the file position, to the end.
        CHECK(g.call("NtOpenFile", {handle, 0x80100000, g.attributes("D:\\BUNDLES\\common.BNL", kDos), iosb, 1, 0x60}) == 0);
        const uint32_t h = g.u32(handle);
        CHECK(h != 0 && g.u32(iosb) == 0 && g.u32(iosb + 4) == 1);  // FILE_OPENED
        g.put32(off, 0);
        g.put32(off + 4, 0);
        CHECK(g.call("NtReadFile", {h, 0, 0, 0, iosb, buf, 5, off}) == 0 && g.u32(iosb + 4) == 5);
        CHECK(std::memcmp(g.ram.get() + buf, "hello", 5) == 0);
        CHECK(g.call("NtReadFile", {h, 0, 0, 0, iosb, buf, 100, 0}) == 0 && g.u32(iosb + 4) == 5);
        CHECK(std::memcmp(g.ram.get() + buf, " disc", 5) == 0);
        CHECK(g.call("NtReadFile", {h, 0, 0, 0, iosb, buf, 100, 0}) == 0xC0000011);  // STATUS_END_OF_FILE
        CHECK(g.call("NtQueryInformationFile", {h, iosb, buf, 24, 5}) == 0);           // FileStandardInformation
        CHECK(g.u32(buf + 8) == 10 && g.ram[buf + 21] == 0);                              // EndOfFile; not a directory
        CHECK(g.call("NtClose", {h}) == 0 && g.call("NtClose", {h}) == 0xC0000008);

        // The disc is read-only; names can't climb out of a device.
        CHECK(g.call("NtOpenFile", {handle, 0x40100000, g.attributes("D:\\bundles\\common.bnl", kDos), iosb, 0, 0x60}) == 0xC00000A2);
        CHECK(g.call("NtOpenFile", {handle, 0x80100000, g.attributes("D:\\..\\x", kDos), iosb, 1, 0x60}) == 0xC000003A);
        CHECK(g.call("NtOpenFile", {handle, 0x80100000, g.attributes("D:\\missing", kDos), iosb, 1, 0x60}) == 0xC0000034);

        // A directory, enumerated through a mask, one entry per call.
        CHECK(g.call("NtOpenFile", {handle, 0x80100001, g.attributes("\\Device\\CdRom0\\bundles"), iosb, 3, 0x21}) == 0);
        const uint32_t dir = g.u32(handle), mask = g.object_string("*.BNL");
        CHECK(g.call("NtQueryDirectoryFile", {dir, 0, 0, 0, iosb, buf, 0x200, 1, mask, 0}) == 0);
        CHECK(g.u32(buf + 40) == 10 && g.u32(buf + 60) == 10 && std::memcmp(g.ram.get() + buf + 64, "Common.bnl", 10) == 0);
        CHECK(g.call("NtQueryDirectoryFile", {dir, 0, 0, 0, iosb, buf, 0x200, 1, 0, 0}) == 0x80000006);  // STATUS_NO_MORE_FILES
        CHECK(g.call("NtClose", {dir}) == 0);

        // The hard disk: create, write, close, and the file is on the host.
        CHECK(g.call("NtCreateFile", {handle, 0xC0100000, g.attributes("\\Device\\Harddisk0\\Partition1\\save.dat"), iosb, 0, 0x80, 0, 5, 0x60}) == 0);
        CHECK(g.u32(iosb + 4) == 2);  // FILE_CREATED
        const uint32_t w = g.u32(handle), text = g.bytes("abc", 3);
        CHECK(g.call("NtWriteFile", {w, 0, 0, 0, iosb, text, 3, 0}) == 0 && g.u32(iosb + 4) == 3);
        CHECK(g.call("NtClose", {w}) == 0);
        std::string saved;
        std::getline(std::ifstream(root / "hdd" / "partition1" / "save.dat", std::ios::binary), saved);
        CHECK(saved == "abc");
        CHECK(g.call("NtQueryFullAttributesFile", {g.attributes("\\Device\\Harddisk0\\Partition1\\SAVE.DAT"), buf}) == 0);
        CHECK(g.u32(buf + 40) == 3 && g.u32(buf + 48) == 0x80);  // EndOfFile; FILE_ATTRIBUTE_NORMAL

        // Five pre-write generations, one per handle even across multiple writes.
        const auto save_name = g.attributes("\\Device\\Harddisk0\\Partition1\\save.dat");
        const auto history = root / "hdd" / "save-history" / "partition1" / "save.dat";
        for (uint32_t version = 1; version <= 6; ++version) {
            CHECK(g.call("NtCreateFile", {handle, 0xC0100000, save_name, iosb, 0, 0x80, 0, 5, 0x60}) == 0);
            const auto out = g.u32(handle);
            g.ram[text] = uint8_t('0' + version);
            CHECK(g.call("NtWriteFile", {out, 0, 0, 0, iosb, text, 1, 0}) == 0);
            CHECK(g.call("NtFlushBuffersFile", {out, iosb}) == 0);
            CHECK(g.call("NtClose", {out}) == 0);
        }
        std::string previous;
        std::getline(std::ifstream(history / "1.bin"), previous); CHECK(previous == "5");
        std::getline(std::ifstream(history / "5.bin"), previous); CHECK(previous == "1");
        CHECK(!fs::exists(history / "6.bin"));
        // Backup failure must not truncate or modify the existing save.
        fs::remove_all(root / "hdd" / "save-history");
        std::ofstream(root / "hdd" / "save-history") << "blocked";
        CHECK(g.call("NtCreateFile", {handle, 0xC0100000, save_name, iosb, 0, 0x80, 0, 5, 0x60}) == 0xC0000022);
        CHECK(fs::file_size(root / "hdd" / "partition1" / "save.dat") == 1);
        CHECK(g.call("NtOpenFile", {handle, 0xC0100000, save_name, iosb, 0, 0x60}) == 0);
        const auto protected_save = g.u32(handle);
        CHECK(g.call("NtWriteFile", {protected_save, 0, 0, 0, iosb, text, 3, 0}) == 0xC0000022);
        CHECK(fs::file_size(root / "hdd" / "partition1" / "save.dat") == 1);
        CHECK(g.call("NtClose", {protected_save}) == 0);
        fs::remove(root / "hdd" / "save-history");
        CHECK(g.call("NtOpenFile", {handle, 0xC0100000, save_name, iosb, 0, 0x60}) == 0);
        const auto rw = g.u32(handle);
        // Reject negative offsets and short information structures without changing the file position.
        g.put32(off, 0); g.put32(off + 4, 0x80000000);
        CHECK(g.call("NtReadFile", {rw, 0, 0, 0, iosb, buf, 1, off}) == 0xC000000D && g.u32(iosb + 4) == 0);
        CHECK(g.call("NtSetInformationFile", {rw, iosb, off, 8, 14}) == 0xC000000D);
        CHECK(g.call("NtSetInformationFile", {rw, iosb, off, 1, 20}) == 0xC0000004);
        // Append is the NT sentinel, not a negative seek.
        g.put32(off, 0xffffffff); g.put32(off + 4, 0xffffffff);
        CHECK(g.call("NtWriteFile", {rw, 0, 0, 0, iosb, text, 1, off}) == 0);
        CHECK(fs::file_size(root / "hdd" / "partition1" / "save.dat") == 2);
        g.put32(off, 0); g.put32(off + 4, 0);
        CHECK(g.call("NtReadFile", {rw, 0, 0, 0, iosb, buf, 8, off}) == 0 && g.u32(iosb + 4) == 2);
        CHECK(g.call("NtFlushBuffersFile", {rw, iosb}) == 0); // EOF on a read/write handle is benign
        CHECK(g.call("NtClose", {rw}) == 0);
        // A read-only handle on a writable partition must see another handle's growth.
        CHECK(g.call("NtOpenFile", {handle, 0x80100000, save_name, iosb, 0, 0x60}) == 0);
        const auto reader = g.u32(handle);
        CHECK(g.call("NtReadFile", {reader, 0, 0, 0, iosb, buf, 8, 0}) == 0 && g.u32(iosb + 4) == 2);
        { std::ofstream append(root / "hdd" / "partition1" / "save.dat", std::ios::app); append << "more"; }
        CHECK(g.call("NtReadFile", {reader, 0, 0, 0, iosb, buf, 8, 0}) == 0 && g.u32(iosb + 4) == 4);
        CHECK(g.call("NtReadFile", {reader, 0, 0, 0, iosb, buf, 8, 0}) == 0xC0000011);
        CHECK(g.call("NtFlushBuffersFile", {reader, iosb}) == 0);
        CHECK(g.call("NtClose", {reader}) == 0);

        g.put32(off, 0); g.put32(off + 4, 0);
        // Formatting a cache partition: the FATX header empties it and sets
        // its cluster size, which the volume then reports.
        fs::create_directories(root / "hdd" / "partition3" / "old");
        CHECK(g.call("NtOpenFile", {handle, 0xC0100000, g.attributes("\\Device\\Harddisk0\\Partition3"), iosb, 0, 0x18}) == 0);
        const uint32_t raw = g.u32(handle), geo = g.alloc(32);
        CHECK(g.call("NtDeviceIoControlFile", {raw, 0, 0, 0, iosb, 0x74004, 0, 0, geo, 32}) == 0);
        CHECK(g.u32(geo) == 0x80000 && g.u32(geo + 8) == 0x2EE00000 && g.u32(geo + 20) == 3);
        std::memset(g.ram.get() + buf, 0, 0x1000);
        std::memcpy(g.ram.get() + buf, "FATX", 4);
        g.put32(buf + 8, 128);  // SectorsPerCluster: 64 KB clusters
        CHECK(g.call("NtWriteFile", {raw, 0, 0, 0, iosb, buf, 0x1000, off}) == 0);
        CHECK(g.call("NtFsControlFile", {raw, 0, 0, 0, iosb, 0x90020, 0, 0, 0, 0}) == 0 && g.call("NtClose", {raw}) == 0);
        CHECK(!fs::exists(root / "hdd" / "partition3" / "old"));
        CHECK(g.call("NtOpenFile", {handle, 0x80100001, g.attributes("\\Device\\Harddisk0\\Partition3\\"), iosb, 3, 0x21}) == 0);
        CHECK(g.call("NtQueryVolumeInformationFile", {g.u32(handle), iosb, buf, 24, 3}) == 0);
        CHECK(g.u32(buf) == 0x2EE00000 / 0x10000 && g.u32(buf + 16) == 128 && g.u32(buf + 20) == 512);

        // The configuration area: sectors read back what was written, and zero before.
        CHECK(g.call("NtOpenFile", {handle, 0xC0100000, g.attributes("\\Device\\Harddisk0\\partition0"), iosb, 3, 0x10}) == 0);
        const uint32_t cfg = g.u32(handle);
        g.put32(off, 0x800);
        CHECK(g.call("NtReadFile", {cfg, 0, 0, 0, iosb, buf, 0x200, off}) == 0 && g.u32(buf) == 0);
        g.put32(buf, 0x97315286);
        CHECK(g.call("NtWriteFile", {cfg, 0, 0, 0, iosb, buf, 0x200, off}) == 0);
        g.put32(buf, 0);
        CHECK(g.call("NtReadFile", {cfg, 0, 0, 0, iosb, buf, 0x200, off}) == 0 && g.u32(buf) == 0x97315286);
        CHECK(g.call("NtClose", {cfg}) == 0);
#ifndef _WIN32
        // Real host write failure: never turn an exhausted device into success,
        // and retain the failure across flush, another write and close.
        if (fs::exists("/dev/full")) {
            fs::create_symlink("/dev/full", root / "hdd" / "partition3" / "full");
            CHECK(g.call("NtOpenFile", {handle, 0xC0100000, g.attributes("\\Device\\Harddisk0\\Partition3\\full"), iosb, 0, 0x60}) == 0);
            const auto full = g.u32(handle);
            CHECK(g.call("NtWriteFile", {full, 0, 0, 0, iosb, text, 1, 0}) == 0xC0000022 && g.u32(iosb + 4) == 0);
            CHECK(g.call("NtFlushBuffersFile", {full, iosb}) == 0xC0000022);
            CHECK(g.call("NtWriteFile", {full, 0, 0, 0, iosb, text, 1, 0}) == 0xC0000022);
            CHECK(g.call("NtClose", {full}) == 0xC0000022);
        }
#endif
        fs::remove_all(root, ec);
    }

    // SHA-1 and HMAC-SHA1 (the kernel's XcSHA*, XcHMAC): FIPS 180 and RFC 2202 vectors.
    {
        auto hex = [](const std::array<uint8_t, 20>& d) {
            std::string s;
            for (uint8_t b : d) s += strf("%02x", b);
            return s;
        };
        host::Sha1 s;
        s.update(reinterpret_cast<const uint8_t*>("abc"), 3);
        CHECK(hex(s.final()) == "a9993e364706816aba3e25717850c26c9cd0d89d");
        host::Sha1 m;  // a million 'a's, in uneven pieces across block boundaries
        const std::string chunk(997, 'a');
        size_t left = 1000000;
        while (left) {
            const size_t n = std::min(left, chunk.size());
            m.update(reinterpret_cast<const uint8_t*>(chunk.data()), n);
            left -= n;
        }
        CHECK(hex(m.final()) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
        const uint8_t key[20] = {0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
                                 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b};
        CHECK(hex(host::hmac_sha1(key, 20, reinterpret_cast<const uint8_t*>("Hi "), 3, reinterpret_cast<const uint8_t*>("There"), 5)) ==
              "b617318655057264e28bc0b6fb378c8ef146be00");
        const std::string big(80, '\xaa');  // a key longer than a block is hashed first
        CHECK(hex(host::hmac_sha1(reinterpret_cast<const uint8_t*>(big.data()), 80,
                                  reinterpret_cast<const uint8_t*>("Test Using Larger Than Block-Size Key - Hash Key First"), 54)) ==
              "aa4ae5e15272d00e95705637ce8a3b55ed402112");
    }

    // The MMX helpers WMADEC's lifted code uses: arithmetic shifts (past the
    // width, the sign everywhere), saturating packs, pmaddwd.
    {
        namespace mmx = rt::mmx;
        CHECK(mmx::sar<int16_t>(0x8000'7FFF'FFFE'0004ull, 1) == 0xC000'3FFF'FFFF'0002ull);
        CHECK(mmx::sar<int16_t>(0x8000'7FFF'0001'F000ull, 15) == 0xFFFF'0000'0000'FFFFull);
        CHECK(mmx::sar<int16_t>(0x8000'7FFF'0001'F000ull, 40) == 0xFFFF'0000'0000'FFFFull);
        CHECK(mmx::sar<int32_t>(0x8000'0000'0000'0010ull, 4) == 0xF800'0000'0000'0001ull);
        CHECK(mmx::packssdw(0xFFFF'8000'0001'0000ull, 0x0000'0005'FFFF'FFFFull) == 0x0005'FFFF'8000'7FFFull);
        CHECK(mmx::packuswb(0x0100'FFFF'007F'0080ull, 0) == 0x00000000'FF00'7F80ull);
        CHECK(mmx::pmaddwd(0x0002'0003'FFFF'0004ull, 0x0005'0006'0007'0008ull) == (uint64_t(2 * 5 + 3 * 6) << 32 | uint32_t(-7 + 32)));
        CHECK(mmx::add<uint32_t>(0xFFFF'FFFF'0000'0001ull, 0x0000'0001'0000'0001ull) == 0x0000'0000'0000'0002ull);
        // Each operation's host vector instruction against its definition:
        // the edges of every lane (the saturations, the signs) and 200,000
        // random pairs, with every shift count there is.
        {
            uint64_t x = 0x9E3779B97F4A7C15ull;
            auto next = [&] { return x ^= x << 13, x ^= x >> 7, x ^= x << 17; };
            static const uint16_t edges[] = {0, 1, 2, 0x7F, 0x80, 0xFF, 0x100, 0x7FFE, 0x7FFF, 0x8000, 0x8001, 0xFF7F, 0xFF80, 0xFFFE, 0xFFFF};
            auto same = [&](uint64_t a, uint64_t b) {
                namespace s = mmx::scalar;
                bool ok = mmx::paddw(a, b) == s::paddw(a, b) && mmx::paddsw(a, b) == s::paddsw(a, b) && mmx::psubusw(a, b) == s::psubusw(a, b) &&
                          mmx::psubsw(a, b) == s::psubsw(a, b) && mmx::pmullw(a, b) == s::pmullw(a, b) && mmx::pmulhw(a, b) == s::pmulhw(a, b) &&
                          mmx::pmaddwd(a, b) == s::pmaddwd(a, b) && mmx::packssdw(a, b) == s::packssdw(a, b) &&
                          mmx::packsswb(a, b) == s::packsswb(a, b) && mmx::packuswb(a, b) == s::packuswb(a, b);
                ok = ok && mmx::add<uint8_t>(a, b) == s::add<uint8_t>(a, b) && mmx::add<uint16_t>(a, b) == s::add<uint16_t>(a, b) &&
                     mmx::add<uint32_t>(a, b) == s::add<uint32_t>(a, b) && mmx::add<uint64_t>(a, b) == s::add<uint64_t>(a, b) &&
                     mmx::sub<uint8_t>(a, b) == s::sub<uint8_t>(a, b) && mmx::sub<uint16_t>(a, b) == s::sub<uint16_t>(a, b) &&
                     mmx::sub<uint32_t>(a, b) == s::sub<uint32_t>(a, b) && mmx::sub<uint64_t>(a, b) == s::sub<uint64_t>(a, b);
                for (const bool high : {false, true})
                    ok = ok && mmx::unpack<uint8_t>(a, b, high) == s::unpack<uint8_t>(a, b, high) &&
                         mmx::unpack<uint16_t>(a, b, high) == s::unpack<uint16_t>(a, b, high) &&
                         mmx::unpack<uint32_t>(a, b, high) == s::unpack<uint32_t>(a, b, high);
                return ok;
            };
            auto shifts = [&](uint64_t a, uint64_t n) {
                namespace s = mmx::scalar;
                return mmx::shl<uint16_t>(a, n) == s::shl<uint16_t>(a, n) && mmx::shl<uint32_t>(a, n) == s::shl<uint32_t>(a, n) &&
                       mmx::shl<uint64_t>(a, n) == s::shl<uint64_t>(a, n) && mmx::shr<uint16_t>(a, n) == s::shr<uint16_t>(a, n) &&
                       mmx::shr<uint32_t>(a, n) == s::shr<uint32_t>(a, n) && mmx::shr<uint64_t>(a, n) == s::shr<uint64_t>(a, n) &&
                       mmx::sar<int16_t>(a, n) == s::sar<int16_t>(a, n) && mmx::sar<int32_t>(a, n) == s::sar<int32_t>(a, n);
            };
            bool pairs = true, counts = true;
            for (const uint16_t e1 : edges)
                for (const uint16_t e2 : edges) {
                    const uint64_t a = uint64_t(e1) * 0x0001'0001'0001'0001ull, b = uint64_t(e2) | uint64_t(e2) << 16 | uint64_t(e1) << 32 | uint64_t(e2) << 48;
                    pairs = pairs && same(a, b) && same(b, a);
                }
            for (int k = 0; k < 200000; ++k) {
                const uint64_t a = next(), b = next();
                pairs = pairs && same(a, b);
                counts = counts && shifts(a, uint64_t(k) % 70) && shifts(b, k % 1000 == 0 ? next() : uint64_t(k) % 33);
            }
            CHECK(pairs);
            CHECK(counts);
        }
    }

    if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
    else std::printf("all host tests passed\n");
    return failures != 0;
}
