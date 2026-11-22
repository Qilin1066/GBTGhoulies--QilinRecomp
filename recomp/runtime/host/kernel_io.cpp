// The kernel's I/O manager and object namespace, over host directories.
//
// Devices: \Device\CdRom0 is the extracted game disc (read-only); the hard
// disk's partitions, \Device\Harddisk0\Partition1..6, are directories under
// the runtime's data directory, created when first used. Symbolic links
// (\??\D: and the other drive letters XAPI makes) map names onto those.
// Paths are case-insensitive, as on the Xbox's FATX and the disc's XDVDFS:
// each component is matched exactly first, then case-insensitively.
//
// Every Nt* function here follows the kernel's documented behaviour on the
// guest's own structures (OBJECT_ATTRIBUTES, IO_STATUS_BLOCK, the
// information classes), and I/O completes synchronously. Anything the game
// asks for that isn't modelled yet (an APC routine, an event handle, an
// information class) stops the runtime with its name.
#include <thread>
#include <atomic>
#include <set>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <limits>
#include "file_store.hpp"
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "host.hpp"
#include "sched.hpp"
#include "strf.hpp"

namespace qilin::host {
namespace fs = std::filesystem;
namespace {
std::atomic<uint64_t> g_io_ns{0};  // time spent reading and writing files, for the long-frame log
std::atomic<int> g_game_reads{0};                       // reads of the game's in progress
std::atomic<std::chrono::steady_clock::rep> g_last_game_read{0};  // when its last one ended

// ---------------------------------------------------------------- statuses
constexpr uint32_t kSuccess = 0, kNoMoreFiles = 0x80000006, kBufferOverflow = 0x80000005,
                   kInfoLengthMismatch = 0xC0000004, kInvalidHandle = 0xC0000008, kInvalidParameter = 0xC000000D,
                   kNoSuchFile = 0xC000000F, kInvalidDeviceRequest = 0xC0000010, kBufferTooSmall = 0xC0000023, kEndOfFile = 0xC0000011,
                   kAccessDenied = 0xC0000022, kObjectNameInvalid = 0xC0000033, kNameNotFound = 0xC0000034,
                   kNameCollision = 0xC0000035, kPathNotFound = 0xC000003A, kWriteProtected = 0xC00000A2,
                   kFileIsADirectory = 0xC00000BA, kDirectoryNotEmpty = 0xC0000101, kNotADirectory = 0xC0000103;

rt::Mem mem(const rt::Context& c) { return rt::Mem{c.mem, c.mem_size}; }
uint32_t ld(const rt::Context& c, uint32_t a) { return rt::ld32(mem(c), a, 0); }
void st(const rt::Context& c, uint32_t a, uint32_t v) { rt::st32(mem(c), a, v, 0); }
void st64(const rt::Context& c, uint32_t a, uint64_t v) { rt::st64(mem(c), a, v, 0); }

[[noreturn]] void unmodelled(const char* fn, const std::string& what) {
    static std::string why;
    why = strf("%s: %s isn't modelled yet", fn, what.c_str());
    rt::trap(0, why.c_str());
}

// An ANSI_STRING / OBJECT_STRING: Length, MaximumLength, Buffer.
std::string ansi(const rt::Context& c, uint32_t s) {
    if (!s) return {};
    const uint32_t len = rt::ld16(mem(c), s, 0), buf = ld(c, s + 4);
    std::string out(len, '\0');
    for (uint32_t k = 0; k < len; ++k) out[k] = char(rt::ld8(mem(c), buf + k, 0));
    return out;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& ch : out) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

// ---------------------------------------------------------------- namespace
// The retail hard disk: 8 GB, partitioned by the kernel's fixed table.
// Partition0 is the whole disk; its first 512 KB (sectors 0-0x3FF) are the
// configuration area, which holds the cache partition database (sector 4).
constexpr uint64_t kDiskBytes = 0x1DD156000ull, kConfigArea = 0x80000;
struct PartitionGeometry {
    uint64_t offset, size;
};
constexpr PartitionGeometry kPartitions[7] = {
    {0, kDiskBytes},                 // 0: the whole disk
    {0xABE80000ull, 0x1312D6000ull}, // 1: E:, title and user data
    {0x8CA80000ull, 0x1F400000ull},  // 2: C:, the system
    {0x00080000ull, 0x2EE00000ull},  // 3-5: X:, Y:, Z:, the title caches
    {0x2EE80000ull, 0x2EE00000ull},
    {0x5DC80000ull, 0x2EE00000ull},
    {0x1DD156000ull, 0},             // 6: F:, past the retail disk's end
};

struct Device {
    std::string name;  // lower case, no trailing backslash
    fs::path root;     // a directory; for partition0, the configuration area's file
    bool writable;
    int partition = -1;  // -1: the disc; 0-6: a hard disk partition
    fs::path header;     // partitions 1-6: the FATX volume header the last format wrote
};
std::vector<Device>& devices() {
    static std::vector<Device> d;
    return d;
}
std::map<std::string, std::string>& links() {  // lower-case link name -> target
    static std::map<std::string, std::string> l;
    return l;
}

// The host path for one component under `dir`: the exact name if it exists,
// else a case-insensitive match, else the name as given (for creating).
fs::path child(const fs::path& dir, const std::string& name) {
    fs::path p = dir / name;
    std::error_code ec;
    if (fs::exists(p, ec)) return p;
    const std::string want = lower(name);
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (lower(e.path().filename().string()) == want) return e.path();
    return p;
}

struct Resolved {
    const Device* device = nullptr;
    fs::path host;          // empty for the device itself
    std::string object;     // the full object name, after links
};

// A file the game looks for on a read-only device (the disc) that isn't in
// the game folder: said once, since a folder copied without it shows up
// as the game's disc-error screen.
void note_missing(const Resolved& r) {
    static std::set<std::string> said;
    if (r.device && !r.device->writable && said.insert(r.object).second)
        std::printf("qilin_run: %s isn't in the game folder\n", r.object.c_str());
}

// An object name, relative to `root` (an object name too) if it isn't
// absolute, through symbolic links to a device and a host path.
std::optional<Resolved> resolve(std::string name, const std::string& root) {
    if (!root.empty() && (name.empty() || name[0] != '\\')) name = root + (root.back() == '\\' ? "" : "\\") + name;
    for (int depth = 0; depth < 8; ++depth) {  // a link may point at another link
        const std::string low = lower(name);
        bool changed = false;
        for (const auto& [link, target] : links())
            if (low.starts_with(link) && (low.size() == link.size() || low[link.size()] == '\\')) {
                name = target + name.substr(link.size());
                changed = true;
                break;
            }
        if (!changed) break;
    }
    const std::string low = lower(name);
    for (const Device& d : devices()) {
        if (!low.starts_with(d.name) || (low.size() > d.name.size() && low[d.name.size()] != '\\')) continue;
        Resolved r{&d, {}, name};
        const std::string rest = name.substr(d.name.size());
        // Partitions are directories made on first use.
        std::error_code ec;
        if (d.partition > 0) fs::create_directories(d.root, ec);
        else if (d.partition == 0) fs::create_directories(d.root.parent_path(), ec);
        if (rest.empty()) return r;  // the device object itself: no host path
        fs::path p = d.root;         // "\\": the volume's root directory
        size_t at = 0;
        while (at < rest.size()) {
            if (rest[at] == '\\') {
                ++at;
                continue;
            }
            const size_t end = rest.find('\\', at);
            const std::string comp = rest.substr(at, end == std::string::npos ? std::string::npos : end - at);
            if (comp == "..") return std::nullopt;  // never leave the device's directory
            if (comp != ".") p = child(p, comp);
            at = end == std::string::npos ? rest.size() : end;
        }
        r.host = p;
        return r;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------- handles
struct Handle {
    enum class Kind { Device, File, Directory, Link } kind;
    explicit Handle(Kind k) : kind(k) {}
    const Device* device = nullptr;
    fs::path host;
    std::string object;  // object name, for relative opens
    std::unique_ptr<std::fstream> file;
    uint64_t position = 0;
    bool writable = false, delete_on_close = false;
    bool write_failed = false, history_saved = false;
    std::optional<uint64_t> fixed_size;  // a file that can't change (the disc's): its size, asked once
    bool formatting = false;  // a raw partition handle that has written a FATX volume header
    std::vector<fs::directory_entry> entries;  // directory enumeration, filled on first query
    size_t cursor = 0;
    bool scanned = false;
    std::string mask;
};
std::map<uint32_t, Handle>& handles() {
    static std::map<uint32_t, Handle> h;
    return h;
}
uint32_t new_handle(Handle h) {
    uint32_t v = 4;
    while (handles().contains(v)) v += 4;
    handles().emplace(v, std::move(h));
    return v;
}
Handle* find(uint32_t h) {
    auto it = handles().find(h);
    return it == handles().end() ? nullptr : &it->second;
}

// Keep five pre-write copies outside the guest's partitions. Cache partitions
// are deliberately excluded; title saves live on partition 1.
bool preserve_save(const Device& device, const fs::path& path) {
    if (device.partition != 1) return true;
    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    if (ec) return false;
    if (!exists) return true;
    const fs::path folder = device.root.parent_path() / "save-history" / "partition1" / path.lexically_relative(device.root);
    fs::create_directories(folder, ec);
    if (ec) return false;
    auto copy = [](const fs::path& from, const fs::path& to) {
        std::string bytes;
        return platform::read_file(from, bytes) && platform::atomic_write(to, bytes);
    };
    for (int n = 4; n >= 1; --n) {
        const fs::path previous = folder / (std::to_string(n) + ".bin");
        const bool present = fs::exists(previous, ec);
        if (ec || (present && !copy(previous, folder / (std::to_string(n + 1) + ".bin")))) return false;
    }
    return copy(path, folder / "1.bin");
}
bool preserve_save(Handle& h) {
    if (h.history_saved) return true;
    if (!preserve_save(*h.device, h.host)) return false;
    h.history_saved = true;
    return true;
}

// OBJECT_ATTRIBUTES: RootDirectory, ObjectName, Attributes.
std::optional<Resolved> resolve_attributes(const rt::Context& c, uint32_t oa, uint32_t* status) {
    const uint32_t root = ld(c, oa), name = ld(c, oa + 4);
    std::string base;
    if (root == 0xFFFFFFFDu) {  // ObDosDevicesDirectory(): "D:\\…" names under \\??
        base = "\\??";
    } else if (root) {
        const Handle* h = find(root);
        if (!h) {
            *status = kInvalidHandle;
            return std::nullopt;
        }
        base = h->object;
    }
    const std::string object = ansi(c, name);
    auto r = resolve(object, base);
    if (!r) *status = kPathNotFound;
    // With QILIN_TRACE_HLE set, the trace shows each name and where it went.
    static const bool trace = [] {
        const char* v = std::getenv("QILIN_TRACE_HLE");
        return v && *v && *v != '0';
    }();
    if (trace) std::fprintf(stderr, " [%s%s -> %s]", base.empty() ? "" : (base + " + ").c_str(), object.c_str(),
                            r ? (r->host.empty() ? r->object.c_str() : r->host.string().c_str()) : "unresolved");
    return r;
}

// ---------------------------------------------------------------- times and attributes
uint64_t filetime(const fs::path& p) {  // 100 ns since 1601
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    // file_clock to system_clock (clock_cast isn't in every supported libc++).
    const auto sys = std::chrono::system_clock::now() +
                     std::chrono::duration_cast<std::chrono::system_clock::duration>(t - fs::file_time_type::clock::now());
    const int64_t unix_100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(sys.time_since_epoch()).count() / 100;
    return uint64_t(unix_100ns + 116444736000000000LL);
}
uint32_t attributes(const fs::path& p, bool device_writable) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) return 0x10;  // FILE_ATTRIBUTE_DIRECTORY
    return device_writable ? 0x80 : 0x01;      // NORMAL, or READONLY on the disc
}
uint64_t host_size(const fs::path& p) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) return 0;
    const auto n = fs::file_size(p, ec);
    return ec ? 0 : uint64_t(n);
}
// FILE_NETWORK_OPEN_INFORMATION: Creation, LastAccess, LastWrite, Change
// times, AllocationSize, EndOfFile, FileAttributes (56 bytes).
void network_open_info(const rt::Context& c, uint32_t out, const fs::path& p, bool writable) {
    const uint64_t t = filetime(p), n = host_size(p);
    for (uint32_t k = 0; k < 4; ++k) st64(c, out + 8 * k, t);
    st64(c, out + 32, (n + 0xFFF) & ~uint64_t(0xFFF));
    st64(c, out + 40, n);
    st(c, out + 48, attributes(p, writable));
    st(c, out + 52, 0);
}

void io_status(const rt::Context& c, uint32_t iosb, uint32_t status, uint32_t information) {
    if (!iosb) return;
    st(c, iosb, status);
    st(c, iosb + 4, information);
}

// ---------------------------------------------------------------- NtCreateFile / NtOpenFile
constexpr uint32_t kGenericWrite = 0x40000000, kGenericAll = 0x10000000, kFileWriteData = 0x2, kFileAppendData = 0x4,
                   kDelete = 0x10000;
constexpr uint32_t kSupersede = 0, kOpen = 1, kCreate = 2, kOpenIf = 3, kOverwrite = 4, kOverwriteIf = 5;
constexpr uint32_t kDirectoryFile = 0x1, kNonDirectoryFile = 0x40, kDeleteOnClose = 0x1000;

uint32_t create_file(const rt::Context& c, uint32_t phandle, uint32_t access, uint32_t oa, uint32_t iosb,
                     uint32_t disposition, uint32_t options) {
    uint32_t status = kSuccess;
    const auto r = resolve_attributes(c, oa, &status);
    if (!r) return status;
    const bool want_write = (access & (kGenericWrite | kGenericAll | kFileWriteData | kFileAppendData | kDelete)) != 0 ||
                            disposition != kOpen;
    if (r->host.empty()) {  // the device itself (\Device\CdRom0): raw volume access
        if (want_write && !r->device->writable) return kWriteProtected;
        Handle h{Handle::Kind::Device};
        h.device = r->device;
        h.object = r->object;
        h.writable = want_write;
        st(c, phandle, new_handle(std::move(h)));
        io_status(c, iosb, kSuccess, 1);  // FILE_OPENED
        return kSuccess;
    }
    std::error_code ec;
    const bool exists = fs::exists(r->host, ec), is_dir = exists && fs::is_directory(r->host, ec);
    if (!fs::exists(r->host.parent_path(), ec)) {
        note_missing(*r);
        return kPathNotFound;
    }
    if (want_write && !r->device->writable) return kWriteProtected;
    uint32_t info = 1;  // FILE_OPENED
    if (options & kDirectoryFile) {
        if (exists && !is_dir) return kNotADirectory;
        if (!exists) {
            if (disposition != kCreate && disposition != kOpenIf) return kNameNotFound;
            if (!fs::create_directory(r->host, ec)) return kAccessDenied;
            info = 2;  // FILE_CREATED
        } else if (disposition == kCreate) {
            return kNameCollision;
        }
        Handle h{Handle::Kind::Directory};
        h.device = r->device;
        h.host = r->host;
        h.object = r->object;
        st(c, phandle, new_handle(std::move(h)));
        io_status(c, iosb, kSuccess, info);
        return kSuccess;
    }
    if (is_dir) {
        if (options & kNonDirectoryFile) return kFileIsADirectory;
        if (disposition != kOpen && disposition != kOpenIf) return kFileIsADirectory;
        Handle h{Handle::Kind::Directory};
        h.device = r->device;
        h.host = r->host;
        h.object = r->object;
        st(c, phandle, new_handle(std::move(h)));
        io_status(c, iosb, kSuccess, 1);
        return kSuccess;
    }
    bool truncate = false;
    switch (disposition) {
        case kOpen: if (!exists) { note_missing(*r); return kNameNotFound; } break;
        case kCreate: if (exists) return kNameCollision; info = 2; truncate = true; break;
        case kOpenIf: if (!exists) { info = 2; truncate = true; } break;
        case kOverwrite: if (!exists) return kNameNotFound; info = 3; truncate = true; break;
        case kOverwriteIf: info = exists ? 3 : 2; truncate = true; break;
        case kSupersede: info = exists ? 0 : 2; truncate = true; break;
        default: return kInvalidParameter;
    }
    if (truncate) {
        if (!preserve_save(*r->device, r->host)) return kAccessDenied;
        std::ofstream make(r->host, std::ios::binary | std::ios::trunc);
        if (!make) return kAccessDenied;
        make.close();
        if (!make) return kAccessDenied;
    }
    auto f = std::make_unique<std::fstream>(r->host, std::ios::binary | std::ios::in | (r->device->writable ? std::ios::out : std::ios::in));
    if (!*f) return kAccessDenied;
    Handle h{Handle::Kind::File};
    h.device = r->device;
    h.host = r->host;
    h.object = r->object;
    h.file = std::move(f);
    h.writable = want_write;
    h.history_saved = truncate;
    h.delete_on_close = (options & kDeleteOnClose) != 0;
    st(c, phandle, new_handle(std::move(h)));
    io_status(c, iosb, kSuccess, info);
    return kSuccess;
}

// NTSTATUS NtCreateFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
//     PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
//     ULONG CreateDisposition, ULONG CreateOptions) (190)
void nt_create_file(rt::Context& c) {
    const uint32_t status = create_file(c, arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 7), arg(c, 8));
    if (status) io_status(c, arg(c, 3), status, 0);
    ret(c, status, 36);
}
// NTSTATUS NtOpenFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
//     ULONG ShareAccess, ULONG OpenOptions) (202)
void nt_open_file(rt::Context& c) {
    const uint32_t status = create_file(c, arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), kOpen, arg(c, 5));
    if (status) io_status(c, arg(c, 3), status, 0);
    ret(c, status, 24);
}

// NTSTATUS NtClose(HANDLE) (187)
void nt_close(rt::Context& c) {
    if (sched::close_handle(arg(c, 0))) return ret(c, kSuccess, 4);  // a kernel object's (sched.cpp)
    auto it = handles().find(arg(c, 0));
    if (it == handles().end()) {
        ret(c, kInvalidHandle, 4);
        return;
    }
    Handle& h = it->second;
    uint32_t status = h.write_failed ? kAccessDenied : kSuccess;
    if (h.file) {
        if (!h.write_failed && !h.file->bad()) h.file->clear(); // EOF is not a close failure
        h.file->close();
        if (h.file->fail()) status = kAccessDenied;
    }
    if (h.delete_on_close && status == kSuccess) {
        if (h.kind == Handle::Kind::File && !preserve_save(h)) status = kAccessDenied;
        else {
            std::error_code ec;
            fs::remove(h.host, ec);
            if (ec) status = kAccessDenied;
        }
    }
    handles().erase(it);
    ret(c, status, 4);
}

// ---------------------------------------------------------------- read and write
// Raw sector access to a device handle. Partition0's configuration area is a
// host file, sparse: what was never written reads as zero. The other
// partitions are directories, so their sectors aren't there to read; the one
// raw write modelled is XAPI formatting a cache partition, which writes the
// FATX volume header to sector 0 and then the FAT and the root directory:
// the header empties the directory, as a format empties the volume, and the
// writes after it carry nothing a directory keeps.
void raw_transfer(rt::Context& c, Handle& h, bool write, const char* fn) {
    const uint32_t iosb = arg(c, 4), buf = arg(c, 5), len = arg(c, 6), poff = arg(c, 7);
    const Device& d = *h.device;
    if (!poff) unmodelled(fn, "raw device access without a byte offset");
    const uint64_t off = rt::ld64(mem(c), poff, 0);
    if (uint64_t(buf) + len > c.mem_size) {
        io_status(c, iosb, 0xC0000005, 0);
        ret(c, 0xC0000005, 32);
        return;
    }
    if (off > uint64_t(std::numeric_limits<std::streamoff>::max()) - len) {
        io_status(c, iosb, kInvalidParameter, 0); ret(c, kInvalidParameter, 32); return;
    }
    if ((off | len) & 511) {  // sector-aligned, as the disk driver requires
        io_status(c, iosb, kInvalidParameter, 0);
        ret(c, kInvalidParameter, 32);
        return;
    }
    if (write && !h.writable) {
        io_status(c, iosb, kAccessDenied, 0);
        ret(c, kAccessDenied, 32);
        return;
    }
    uint8_t* p = c.mem + buf;
    uint32_t status = kSuccess;
    if (d.partition == 0) {
        if (off > kConfigArea || len > kConfigArea - off) unmodelled(fn, strf("raw access to the whole disk past its configuration area (offset %#llx)", (unsigned long long)off));
        std::error_code ec;
        if (write) {
            if (!fs::exists(d.root, ec)) std::ofstream(d.root, std::ios::binary);
            std::fstream f(d.root, std::ios::binary | std::ios::in | std::ios::out);
            f.seekp(std::streamoff(off));
            f.write(reinterpret_cast<const char*>(p), len);
            f.flush();
            if (!f) status = kAccessDenied;
            f.close();
            if (f.fail()) status = kAccessDenied;
        } else {
            std::memset(p, 0, len);
            std::ifstream f(d.root, std::ios::binary);
            if (f) {
                f.seekg(std::streamoff(off));
                f.read(reinterpret_cast<char*>(p), len);
                if (f.bad() || (f.fail() && !f.eof())) status = kAccessDenied;
            } else if (fs::exists(d.root, ec) || ec) status = kAccessDenied;
        }
    } else if (d.partition > 0 && write) {
        if (off == 0 && len >= 4 && std::memcmp(p, "FATX", 4) == 0) {
            std::error_code ec;
            for (fs::directory_iterator it(d.root, ec), end; !ec && it != end; it.increment(ec))
                fs::remove_all(it->path(), ec);
            // Keep the header only after a successful format.
            if (ec || !platform::atomic_write(d.header, {reinterpret_cast<const char*>(p), std::min<uint32_t>(len, 4096)}))
                status = kAccessDenied;
            else h.formatting = true;
        } else if (!h.formatting) {
            unmodelled(fn, strf("a raw write to %s that isn't part of a format", d.name.c_str()));
        }
    } else {
        unmodelled(fn, strf("raw reads from %s", d.name.c_str()));
    }
    io_status(c, iosb, status, status == kSuccess ? len : 0);
    ret(c, status, 32);
}

// NtReadFile / NtWriteFile(HANDLE, HANDLE Event, PIO_APC_ROUTINE, PVOID ApcContext,
//     PIO_STATUS_BLOCK, PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset) (219, 236)
void transfer(rt::Context& c, bool write, const char* fn) {
    Handle* h = find(arg(c, 0));
    const uint32_t event = arg(c, 1), apc = arg(c, 2), iosb = arg(c, 4), buf = arg(c, 5), len = arg(c, 6), poff = arg(c, 7);
    if (event) unmodelled(fn, "an event handle");
    if (apc) unmodelled(fn, "an APC routine");
    if (h && h->kind == Handle::Kind::Device) {
        raw_transfer(c, *h, write, fn);
        return;
    }
    if (!h || h->kind != Handle::Kind::File) {
        io_status(c, iosb, h ? kInvalidDeviceRequest : kInvalidHandle, 0);
        ret(c, h ? kInvalidDeviceRequest : kInvalidHandle, 32);
        return;
    }
    if (write && !h->writable) {
        io_status(c, iosb, kAccessDenied, 0);
        ret(c, kAccessDenied, 32);
        return;
    }
    uint64_t off = h->position;
    if (poff) {
        const uint64_t o = rt::ld64(mem(c), poff, 0);
        if (write && o == 0xFFFFFFFFFFFFFFFFull) {
            std::error_code ec;
            off = fs::file_size(h->host, ec);
            if (ec) { io_status(c, iosb, kAccessDenied, 0); ret(c, kAccessDenied, 32); return; }
        } else if (o != 0xFFFFFFFFFFFFFFFEull) off = o;  // FILE_USE_FILE_POINTER_POSITION
    }
    if (off > uint64_t(std::numeric_limits<std::streamoff>::max()) - len) {
        io_status(c, iosb, kInvalidParameter, 0);
        ret(c, kInvalidParameter, 32);
        return;
    }
    if (uint64_t(buf) + len > c.mem_size) {  // the host side is never unchecked
        io_status(c, iosb, 0xC0000005, 0);     // STATUS_ACCESS_VIOLATION
        ret(c, 0xC0000005, 32);
        return;
    }
    uint32_t done = 0, status = kSuccess;
    if (write) {
        if (h->write_failed || !preserve_save(*h)) {
            io_status(c, iosb, kAccessDenied, 0); ret(c, kAccessDenied, 32); return;
        }
        const auto io0 = std::chrono::steady_clock::now();
        h->file->clear();
        h->file->seekp(std::streamoff(off));
        h->file->write(reinterpret_cast<const char*>(c.mem + buf), len);
        h->file->flush();
        g_io_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - io0).count());
        done = *h->file ? len : 0;
        if (!*h->file) { status = kAccessDenied; h->write_failed = true; }
    } else {
        // The disc's files don't change: their size is asked of the host once
        // (it was two calls to the file system at every read).
        if (!h->device->writable && !h->fixed_size) h->fixed_size = host_size(h->host);
        const uint64_t size = h->fixed_size ? *h->fixed_size : host_size(h->host);
        if (off >= size && len) {
            status = kEndOfFile;
        } else {
            const auto io0 = std::chrono::steady_clock::now();
            g_game_reads.fetch_add(1, std::memory_order_relaxed);  // the warm-up stands aside
            h->file->clear();
            h->file->seekg(std::streamoff(off));
            h->file->read(reinterpret_cast<char*>(c.mem + buf), len);
            const auto io1 = std::chrono::steady_clock::now();
            g_last_game_read.store(io1.time_since_epoch().count(), std::memory_order_relaxed);
            g_game_reads.fetch_sub(1, std::memory_order_relaxed);
            const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(io1 - io0).count());
            g_io_ns += ns;
            // Reads over 3 ms are logged (the first 300): a stutter while a scene loads is often the disk.
            static uint32_t slow = 0;
            if (ns > 3'000'000 && slow < 300)
                ++slow, std::fprintf(stderr, "qilin: reading %u bytes at %llu of %s took %.1f ms\n", len, (unsigned long long)off,
                                     h->host.filename().string().c_str(), double(ns) / 1e6);
            done = uint32_t(h->file->gcount());
            if (h->file->bad() || (h->file->fail() && !h->file->eof())) status = kAccessDenied;
            else if (!done && len) status = kEndOfFile;
        }
    }
    if (status == kSuccess) h->position = off + done;
    io_status(c, iosb, status, done);
    ret(c, status, 32);
}
void nt_read_file(rt::Context& c) { transfer(c, false, "NtReadFile"); }
void nt_write_file(rt::Context& c) { transfer(c, true, "NtWriteFile"); }

// NTSTATUS NtFlushBuffersFile(HANDLE, PIO_STATUS_BLOCK) (198)
void nt_flush_buffers_file(rt::Context& c) {
    Handle* h = find(arg(c, 0));
    uint32_t status = !h ? kInvalidHandle : !h->file ? kInvalidDeviceRequest : kSuccess;
    if (h && h->file) {
        if (!h->write_failed && !h->file->bad()) h->file->clear();
        h->file->flush();
        if (h->write_failed || !*h->file) status = kAccessDenied;
    }
    io_status(c, arg(c, 1), status, 0);
    ret(c, status, 8);
}

// ---------------------------------------------------------------- information
// NTSTATUS NtQueryInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG Length,
//     FILE_INFORMATION_CLASS) (211)
void nt_query_information_file(rt::Context& c) {
    Handle* h = find(arg(c, 0));
    const uint32_t iosb = arg(c, 1), out = arg(c, 2), len = arg(c, 3), cls = arg(c, 4);
    if (!h || h->kind == Handle::Kind::Device || h->kind == Handle::Kind::Link) {
        ret(c, h ? kInvalidParameter : kInvalidHandle, 20);
        return;
    }
    uint32_t need = 0;
    switch (cls) {
        case 4: need = 40; break;   // FileBasicInformation
        case 5: need = 24; break;   // FileStandardInformation
        case 14: need = 8; break;   // FilePositionInformation
        case 34: need = 56; break;  // FileNetworkOpenInformation
        default: unmodelled("NtQueryInformationFile", strf("information class %u", cls));
    }
    if (len < need) {
        ret(c, kInfoLengthMismatch, 20);
        return;
    }
    const bool dir = h->kind == Handle::Kind::Directory;
    switch (cls) {
        case 4: {  // Creation, LastAccess, LastWrite, Change times; FileAttributes
            const uint64_t t = filetime(h->host);
            for (uint32_t k = 0; k < 4; ++k) st64(c, out + 8 * k, t);
            st(c, out + 32, attributes(h->host, h->device->writable));
            st(c, out + 36, 0);
            break;
        }
        case 5: {  // AllocationSize, EndOfFile, NumberOfLinks, DeletePending, Directory
            const uint64_t n = host_size(h->host);
            st64(c, out, (n + 0xFFF) & ~uint64_t(0xFFF));
            st64(c, out + 8, n);
            st(c, out + 16, 1);
            rt::st8(mem(c), out + 20, h->delete_on_close ? 1 : 0, 0);
            rt::st8(mem(c), out + 21, dir ? 1 : 0, 0);
            rt::st16(mem(c), out + 22, 0, 0);
            break;
        }
        case 14: st64(c, out, h->position); break;
        case 34: network_open_info(c, out, h->host, h->device->writable); break;
    }
    io_status(c, iosb, kSuccess, need);
    ret(c, kSuccess, 20);
}

// NTSTATUS NtSetInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG Length,
//     FILE_INFORMATION_CLASS) (226)
void nt_set_information_file(rt::Context& c) {
    Handle* h = find(arg(c, 0));
    const uint32_t iosb = arg(c, 1), in = arg(c, 2), cls = arg(c, 4);
    if (!h || h->kind == Handle::Kind::Device || h->kind == Handle::Kind::Link) {
        ret(c, h ? kInvalidParameter : kInvalidHandle, 20);
        return;
    }
    const uint32_t length = arg(c, 3);
    const uint32_t needed = cls == 4 ? 40 : cls == 13 ? 1 : (cls == 14 || cls == 19 || cls == 20) ? 8 : 0;
    if (length < needed) {
        io_status(c, iosb, kInfoLengthMismatch, 0); ret(c, kInfoLengthMismatch, 20); return;
    }
    if ((cls == 14 || cls == 19 || cls == 20) && rt::ld64(mem(c), in, 0) > uint64_t(std::numeric_limits<std::streamoff>::max())) {
        io_status(c, iosb, kInvalidParameter, 0); ret(c, kInvalidParameter, 20); return;
    }
    uint32_t status = kSuccess;
    std::error_code ec;
    switch (cls) {
        case 14:  // FilePositionInformation
            h->position = rt::ld64(mem(c), in, 0);
            break;
        case 20: case 19: {  // FileEndOfFileInformation, FileAllocationInformation
            if (!h->writable) { status = kAccessDenied; break; }
            const uint64_t n = rt::ld64(mem(c), in, 0);
            if (cls == 20 || n < host_size(h->host)) {
                if (h->write_failed || !preserve_save(*h)) { status = kAccessDenied; break; }
                h->file->clear();
                h->file->flush();
                if (!*h->file) { h->write_failed = true; status = kAccessDenied; break; }
                fs::resize_file(h->host, n, ec);
                if (ec) status = kAccessDenied;
            }
            break;
        }
        case 13:  // FileDispositionInformation: DeleteFile
            if (!h->device->writable) { status = kWriteProtected; break; }
            if (h->kind == Handle::Kind::Directory && rt::ld8(mem(c), in, 0) && !fs::is_empty(h->host, ec)) {
                status = kDirectoryNotEmpty;
                break;
            }
            h->delete_on_close = rt::ld8(mem(c), in, 0) != 0;
            break;
        case 4:  // FileBasicInformation: times and attributes; the host keeps its own
            break;
        default:
            unmodelled("NtSetInformationFile", strf("information class %u", cls));
    }
    io_status(c, iosb, status, 0);
    ret(c, status, 20);
}

// NTSTATUS NtQueryFullAttributesFile(POBJECT_ATTRIBUTES, PFILE_NETWORK_OPEN_INFORMATION) (210)
void nt_query_full_attributes_file(rt::Context& c) {
    uint32_t status = kSuccess;
    const auto r = resolve_attributes(c, arg(c, 0), &status);
    std::error_code ec;
    if (r && (r->host.empty() || !fs::exists(r->host, ec))) {
        status = r->host.empty() ? kInvalidParameter : kNameNotFound;
        if (!r->host.empty()) note_missing(*r);
    }
    if (r && !status) network_open_info(c, arg(c, 1), r->host, r->device->writable);
    ret(c, status, 8);
}

// A volume's cluster size: the disc's 2 KB sectors; on the hard disk, what
// the FATX header the last format wrote says (SectorsPerCluster, +8), or the
// retail format's 16 KB.
uint64_t cluster_bytes(const Device& d) {
    if (d.partition < 0) return 2048;
    uint32_t spc = 32;
    if (std::ifstream f{d.header, std::ios::binary}) {
        char hdr[12] = {};
        if (f.read(hdr, sizeof hdr) && std::memcmp(hdr, "FATX", 4) == 0) {
            uint32_t v;
            std::memcpy(&v, hdr + 8, 4);  // little-endian on every supported host
            if (v && (v & (v - 1)) == 0 && v <= 1024) spc = v;
        }
    }
    return uint64_t(spc) * 512;
}
// The bytes a directory tree takes, each file rounded up to whole clusters
// and each directory taking one, as FATX allocates them.
uint64_t tree_bytes(const fs::path& root, uint64_t cluster) {
    uint64_t n = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec)) n += cluster;
        else n += (host_size(it->path()) + cluster - 1) / cluster * cluster;
    }
    return n;
}

// NTSTATUS NtQueryVolumeInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG Length,
//     FS_INFORMATION_CLASS) (218)
void nt_query_volume_information_file(rt::Context& c) {
    Handle* h = find(arg(c, 0));
    const uint32_t iosb = arg(c, 1), out = arg(c, 2), len = arg(c, 3), cls = arg(c, 4);
    if (!h) {
        ret(c, kInvalidHandle, 20);
        return;
    }
    const Device& d = *h->device;
    const uint64_t cluster = cluster_bytes(d), sector = d.partition < 0 ? 2048 : 512;
    uint64_t total, avail = 0;
    if (d.partition < 0) {  // the disc: its files, in 2 KB sectors
        total = (tree_bytes(d.root, cluster) + cluster - 1) / cluster;
    } else {  // a partition: its size, less what its directory holds, within what the host has free
        total = kPartitions[d.partition].size / cluster;
        const uint64_t used = d.partition == 0 ? total : (tree_bytes(d.root, cluster) + cluster - 1) / cluster;
        std::error_code ec;
        const auto space = fs::space(d.partition == 0 ? d.root.parent_path() : d.root, ec);
        avail = std::min<uint64_t>(total - std::min(used, total), ec ? 0 : space.available / cluster);
    }
    uint32_t need;
    switch (cls) {
        case 3:  // FileFsSizeInformation: TotalAllocationUnits, AvailableAllocationUnits, SectorsPerAllocationUnit, BytesPerSector
            need = 24;
            if (len < need) break;
            st64(c, out, total);
            st64(c, out + 8, avail);
            st(c, out + 16, uint32_t(cluster / sector));
            st(c, out + 20, uint32_t(sector));
            break;
        case 7:  // FileFsFullSizeInformation: Total, CallerAvailable, ActualAvailable, SectorsPerAllocationUnit, BytesPerSector
            need = 32;
            if (len < need) break;
            st64(c, out, total);
            st64(c, out + 8, avail);
            st64(c, out + 16, avail);
            st(c, out + 24, uint32_t(cluster / sector));
            st(c, out + 28, uint32_t(sector));
            break;
        default:
            unmodelled("NtQueryVolumeInformationFile", strf("information class %u", cls));
    }
    if (len < need) {
        ret(c, kInfoLengthMismatch, 20);
        return;
    }
    io_status(c, iosb, kSuccess, need);
    ret(c, kSuccess, 20);
}

// ---------------------------------------------------------------- directories
bool wildcard(std::string_view mask, std::string_view name) {  // * and ?, case-insensitive
    if (mask.empty()) return true;
    size_t m = 0, n = 0, star = std::string_view::npos, back = 0;
    while (n < name.size()) {
        if (m < mask.size() && (mask[m] == '?' || std::tolower(static_cast<unsigned char>(mask[m])) ==
                                                      std::tolower(static_cast<unsigned char>(name[n])))) {
            ++m;
            ++n;
        } else if (m < mask.size() && mask[m] == '*') {
            star = m++;
            back = n;
        } else if (star != std::string_view::npos) {
            m = star + 1;
            n = ++back;
        } else {
            return false;
        }
    }
    while (m < mask.size() && mask[m] == '*') ++m;
    return m == mask.size();
}

// NTSTATUS NtQueryDirectoryFile(HANDLE, HANDLE Event, PIO_APC_ROUTINE, PVOID ApcContext,
//     PIO_STATUS_BLOCK, PVOID FileInformation, ULONG Length, FILE_INFORMATION_CLASS,
//     POBJECT_STRING FileMask, BOOLEAN RestartScan) (207)
// FILE_DIRECTORY_INFORMATION (class 1): NextEntryOffset, FileIndex, four
// times, EndOfFile, AllocationSize, FileAttributes, FileNameLength, then the
// name in ANSI (the Xbox's form). One entry per call, as XAPI asks.
void nt_query_directory_file(rt::Context& c) {
    Handle* h = find(arg(c, 0));
    const uint32_t event = arg(c, 1), apc = arg(c, 2), iosb = arg(c, 4), out = arg(c, 5), len = arg(c, 6), cls = arg(c, 7),
                   mask = arg(c, 8);
    const bool restart = (arg(c, 9) & 0xFF) != 0;
    if (event) unmodelled("NtQueryDirectoryFile", "an event handle");
    if (apc) unmodelled("NtQueryDirectoryFile", "an APC routine");
    if (cls != 1) unmodelled("NtQueryDirectoryFile", strf("information class %u", cls));
    if (!h || h->kind != Handle::Kind::Directory) {
        ret(c, h ? kInvalidParameter : kInvalidHandle, 40);
        return;
    }
    if (!h->scanned || restart) {
        h->entries.clear();
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(h->host, ec)) h->entries.push_back(e);
        std::ranges::sort(h->entries, {}, [](const fs::directory_entry& e) { return lower(e.path().filename().string()); });
        h->cursor = 0;
        if (mask || !h->scanned) h->mask = ansi(c, mask);
        h->scanned = true;
    }
    while (h->cursor < h->entries.size() && !wildcard(h->mask, h->entries[h->cursor].path().filename().string())) ++h->cursor;
    if (h->cursor >= h->entries.size()) {
        const uint32_t status = h->entries.empty() || h->cursor == 0 ? kNoSuchFile : kNoMoreFiles;
        io_status(c, iosb, status, 0);
        ret(c, status, 40);
        return;
    }
    const fs::path p = h->entries[h->cursor].path();
    const std::string name = p.filename().string();
    const uint32_t need = 64 + uint32_t(name.size());
    if (len < 64) {
        ret(c, kInfoLengthMismatch, 40);
        return;
    }
    const uint64_t t = filetime(p), n = host_size(p);
    st(c, out, 0);
    st(c, out + 4, uint32_t(h->cursor));
    for (uint32_t k = 0; k < 4; ++k) st64(c, out + 8 + 8 * k, t);
    st64(c, out + 40, n);
    st64(c, out + 48, (n + 0xFFF) & ~uint64_t(0xFFF));
    st(c, out + 56, attributes(p, h->device->writable));
    st(c, out + 60, uint32_t(name.size()));
    const uint32_t fit = std::min<uint32_t>(uint32_t(name.size()), len - 64);
    for (uint32_t k = 0; k < fit; ++k) rt::st8(mem(c), out + 64 + k, uint8_t(name[k]), 0);
    ++h->cursor;
    const uint32_t status = fit < name.size() ? kBufferOverflow : kSuccess;
    io_status(c, iosb, status, std::min(need, len));
    ret(c, status, 40);
}

// ---------------------------------------------------------------- device control
// NTSTATUS NtDeviceIoControlFile / NtFsControlFile(HANDLE, HANDLE Event,
//     PIO_APC_ROUTINE, PVOID ApcContext, PIO_STATUS_BLOCK, ULONG IoControlCode,
//     PVOID InputBuffer, ULONG InputBufferLength, PVOID OutputBuffer,
//     ULONG OutputBufferLength) (196, 200)
// There's no drive behind the disc: SCSI pass-through (XAPI's DVD
// authentication check at start-up, whose result it ignores) and other
// device requests fail as an empty drive would.
// The hard disk's partitions answer the two disk queries XAPI's formatter
// makes, from the retail partition table.
void nt_device_io_control_file(rt::Context& c) {
    Handle* h = find(arg(c, 0));
    const uint32_t iosb = arg(c, 4), code = arg(c, 5), out = arg(c, 8), out_len = arg(c, 9);
    if (arg(c, 1)) unmodelled("NtDeviceIoControlFile", "an event handle");
    if (arg(c, 2)) unmodelled("NtDeviceIoControlFile", "an APC routine");
    if (!h) {
        ret(c, kInvalidHandle, 40);
        return;
    }
    uint32_t status = kInvalidDeviceRequest, info = 0;
    const int part = h->kind == Handle::Kind::Device ? h->device->partition : -1;
    if (part >= 0 && code == 0x70000) {  // IOCTL_DISK_GET_DRIVE_GEOMETRY: the whole disk, one sector per "cylinder"
        if (out_len < 24) {
            status = kBufferTooSmall;
        } else {
            st64(c, out, kDiskBytes / 512);  // Cylinders
            st(c, out + 8, 12);              // MediaType: FixedMedia
            st(c, out + 12, 1);              // TracksPerCylinder
            st(c, out + 16, 1);              // SectorsPerTrack
            st(c, out + 20, 512);            // BytesPerSector
            status = kSuccess;
            info = 24;
        }
    } else if (part >= 0 && code == 0x74004) {  // IOCTL_DISK_GET_PARTITION_INFO
        if (out_len < 32) {
            status = kBufferTooSmall;
        } else {
            const PartitionGeometry g = kPartitions[part];
            st64(c, out, g.offset);                   // StartingOffset
            st64(c, out + 8, g.size);                 // PartitionLength
            st(c, out + 16, uint32_t(g.offset / 512)); // HiddenSectors
            st(c, out + 20, uint32_t(part));          // PartitionNumber
            rt::st8(mem(c), out + 24, 0, 0);          // PartitionType
            rt::st8(mem(c), out + 25, 0, 0);          // BootIndicator
            rt::st8(mem(c), out + 26, 1, 0);          // RecognizedPartition
            rt::st8(mem(c), out + 27, 0, 0);          // RewritePartition
            status = kSuccess;
            info = 32;
        }
    } else if (part >= 0) {
        unmodelled("NtDeviceIoControlFile", strf("control code %#x on %s", code, h->device->name.c_str()));
    }
    // The disc: no drive behind it, so SCSI pass-through (XAPI's DVD
    // authentication check, whose result it ignores) fails as on an empty drive.
    io_status(c, iosb, status, info);
    ret(c, status, 40);
}

// FSCTL_DISMOUNT_VOLUME, which XAPI's formatter sends when it's done: there's
// no cached volume state to drop.
void nt_fs_control_file(rt::Context& c) {
    Handle* h = find(arg(c, 0));
    const uint32_t code = arg(c, 5);
    if (!h) {
        ret(c, kInvalidHandle, 40);
        return;
    }
    if (code != 0x90020) unmodelled("NtFsControlFile", strf("control code %#x", code));
    io_status(c, arg(c, 4), kSuccess, 0);
    ret(c, kSuccess, 40);
}

// ---------------------------------------------------------------- symbolic links
// NTSTATUS IoCreateSymbolicLink(POBJECT_STRING SymbolicLinkName, POBJECT_STRING DeviceName) (67)
void io_create_symbolic_link(rt::Context& c) {
    const std::string link = lower(ansi(c, arg(c, 0))), target = ansi(c, arg(c, 1));
    uint32_t status = kSuccess;
    if (link.empty()) status = kObjectNameInvalid;
    else if (links().contains(link)) status = kNameCollision;
    else links()[link] = target.ends_with('\\') ? target.substr(0, target.size() - 1) : target;
    ret(c, status, 8);
}
// NTSTATUS IoDeleteSymbolicLink(POBJECT_STRING) (69)
void io_delete_symbolic_link(rt::Context& c) {
    ret(c, links().erase(lower(ansi(c, arg(c, 0)))) ? kSuccess : kNameNotFound, 4);
}
// NTSTATUS NtOpenSymbolicLinkObject(PHANDLE, POBJECT_ATTRIBUTES) (203)
void nt_open_symbolic_link_object(rt::Context& c) {
    const std::string name = lower(ansi(c, ld(c, arg(c, 1) + 4)));
    const auto it = links().find(name);
    if (it == links().end()) {
        ret(c, kNameNotFound, 8);
        return;
    }
    Handle h{Handle::Kind::Link};
    h.object = it->second;
    st(c, arg(c, 0), new_handle(std::move(h)));
    ret(c, kSuccess, 8);
}
// NTSTATUS NtQuerySymbolicLinkObject(HANDLE, POBJECT_STRING LinkTarget, PULONG ReturnedLength) (215)
void nt_query_symbolic_link_object(rt::Context& c) {
    Handle* h = find(arg(c, 0));
    const uint32_t target = arg(c, 1), plen = arg(c, 2);
    if (!h || h->kind != Handle::Kind::Link) {
        ret(c, kInvalidHandle, 12);
        return;
    }
    const uint32_t max = rt::ld16(mem(c), target + 2, 0), buf = ld(c, target + 4);
    const uint32_t n = uint32_t(h->object.size());
    if (plen) st(c, plen, n);
    if (n > max) {
        ret(c, kBufferOverflow, 12);
        return;
    }
    for (uint32_t k = 0; k < n; ++k) rt::st8(mem(c), buf + k, uint8_t(h->object[k]), 0);
    rt::st16(mem(c), target, uint16_t(n), 0);
    ret(c, kSuccess, 12);
}

// NTSTATUS FscSetCacheSize(ULONG NumberOfCachePages) (37): the size of the
// kernel's file cache, which the title takes out of its 64 MB. The host
// caches files itself, so the pages aren't taken from guest RAM.
void fsc_set_cache_size(rt::Context& c) { ret(c, kSuccess, 4); }

struct IoFn {
    const char* name;
    uint16_t arg_bytes;
    HleFn fn;
};
constexpr IoFn kIo[] = {
    {"FscSetCacheSize", 4, fsc_set_cache_size},
    {"NtCreateFile", 36, nt_create_file},
    {"NtOpenFile", 24, nt_open_file},
    {"NtClose", 4, nt_close},
    {"NtReadFile", 32, nt_read_file},
    {"NtWriteFile", 32, nt_write_file},
    {"NtFlushBuffersFile", 8, nt_flush_buffers_file},
    {"NtQueryInformationFile", 20, nt_query_information_file},
    {"NtSetInformationFile", 20, nt_set_information_file},
    {"NtQueryFullAttributesFile", 8, nt_query_full_attributes_file},
    {"NtQueryVolumeInformationFile", 20, nt_query_volume_information_file},
    {"NtQueryDirectoryFile", 40, nt_query_directory_file},
    {"NtDeviceIoControlFile", 40, nt_device_io_control_file},
    {"NtFsControlFile", 40, nt_fs_control_file},
    {"IoCreateSymbolicLink", 8, io_create_symbolic_link},
    {"IoDeleteSymbolicLink", 4, io_delete_symbolic_link},
    {"NtOpenSymbolicLinkObject", 8, nt_open_symbolic_link_object},
    {"NtQuerySymbolicLinkObject", 12, nt_query_symbolic_link_object},
};

}  // namespace

// The disc's files read through once, in the background, as the game
// starts: the system then has them in memory, and the game's own reads
// (which it makes on the thread that draws) don't wait for the disk. On a
// hard disk that wait is 15-20 ms a read, three dropped frames at 144 Hz.
void warm_disc(fs::path disc) {
    std::thread([disc = std::move(disc)] {
        const auto t0 = std::chrono::steady_clock::now();
        // A quarter megabyte at a time, and none while the game is reading
        // or has read in the last 50 ms: on a hard disk the two would
        // otherwise take turns seeking, and the game's reads (bunched as a
        // scene loads) waited 60-150 ms behind this one's.
        std::vector<char> block(1 << 18);
        auto stand_aside = [] {
            using Clock = std::chrono::steady_clock;
            while (g_game_reads.load(std::memory_order_relaxed) > 0 ||
                   Clock::now().time_since_epoch().count() - g_last_game_read.load(std::memory_order_relaxed) <
                       std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(50)).count())
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        };
        uint64_t total = 0;
        std::error_code ec;
        std::vector<fs::path> files;
        for (auto it = fs::recursive_directory_iterator(disc, fs::directory_options::skip_permission_denied, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec))
            if (it->is_regular_file(ec)) files.push_back(it->path());
        std::ranges::sort(files);
        for (const fs::path& f : files) {
            std::ifstream in(f, std::ios::binary);
            for (;;) {
                stand_aside();
                if (!in.read(block.data(), std::streamsize(block.size())) && in.gcount() <= 0) break;
                total += uint64_t(in.gcount());
            }
        }
        std::fprintf(stderr, "qilin: the disc's %zu files (%.0f MB) read into the system's memory in %.1f s\n", files.size(), double(total) / 1e6,
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }).detach();
}

void register_kernel_io(const IoConfig& cfg) {
    if (cfg.warm_disc) warm_disc(cfg.disc);
    devices().clear();
    links().clear();
    handles().clear();
    devices().push_back({"\\device\\cdrom0", cfg.disc, false, -1, {}});
    devices().push_back({"\\device\\harddisk0\\partition0", cfg.data / "partition0.bin", true, 0, {}});
    for (int p = 1; p <= 6; ++p)
        devices().push_back({strf("\\device\\harddisk0\\partition%d", p), cfg.data / strf("partition%d", p), true, p,
                             cfg.data / strf("partition%d.fatx", p)});
    for (const IoFn& k : kIo) {
        const KernelExport* e = kernel_export(k.name);
        if (!e || e->data || e->arg_bytes != k.arg_bytes) {
            std::fprintf(stderr, "qilin: kernel replacement %s doesn't match the export table\n", k.name);
            std::abort();
        }
        register_hle(kernel_addr(e->ordinal), e->name, k.fn);
    }
}

uint64_t take_file_read_ns() { return g_io_ns.exchange(0); }

}  // namespace qilin::host
