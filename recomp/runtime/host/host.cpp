#include "host.hpp"
#include "strf.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <span>

namespace qilin::host {
namespace {

constexpr KernelExport kExports[] = {
#include "kernel_exports.inc"
};

struct Hle {
    const char* name;
    HleFn fn;
};
std::map<uint32_t, Hle>& registry() {
    static std::map<uint32_t, Hle> r;
    return r;
}
std::vector<Region>& regions() {
    static std::vector<Region> r;
    return r;
}
rt::Context* g_current = nullptr;

}  // namespace

const KernelExport* kernel_export(uint32_t ordinal) {
    const auto it = std::ranges::lower_bound(kExports, ordinal, {}, &KernelExport::ordinal);
    return it != std::end(kExports) && it->ordinal == ordinal ? &*it : nullptr;
}

void register_hle(uint32_t guest_addr, const char* name, HleFn fn) { registry()[guest_addr] = {name, fn}; }
size_t hle_count() { return registry().size(); }

uint32_t arg(const rt::Context& c, int i) {
    const rt::Mem m{c.mem, c.mem_size};
    return rt::ld32(m, c.esp + 4u + 4u * uint32_t(i), 0);
}

void ret(rt::Context& c, uint32_t eax, uint32_t arg_bytes) {
    const rt::Mem m{c.mem, c.mem_size};
    c.ret_to = rt::ld32(m, c.esp, 0);
    c.esp += 4u + arg_bytes;
    c.eax = eax;
}

void VirtualMemory::init(uint8_t* ram, uint32_t lo, uint32_t hi) {
    ram_ = ram;
    lo_ = lo;
    hi_ = hi;
    regions_.clear();
}

uint32_t VirtualMemory::reserve(uint32_t base, uint32_t size, uint32_t protect, bool top_down, uint32_t align, uint32_t lowest,
                               uint32_t highest) {
    const uint64_t bytes = (uint64_t(size) + kPage - 1) & ~uint64_t(kPage - 1);
    if (bytes == 0 || bytes > hi_ - lo_ || align == 0 || (align & (align - 1))) return 0;
    const uint64_t lim_lo = std::max<uint64_t>(lo_, lowest), lim_hi = std::min<uint64_t>(hi_, uint64_t(highest) + 1);
    auto free_at = [&](uint64_t at) {  // [at, at + bytes) inside the range and clear of every reservation
        if (at < lim_lo || at + bytes > lim_hi) return false;
        auto it = regions_.upper_bound(uint32_t(at));
        if (it != regions_.end() && it->first < at + bytes) return false;
        if (it != regions_.begin()) {
            --it;
            if (uint64_t(it->first) + it->second.size > at) return false;
        }
        return true;
    };
    uint64_t at = 0;
    if (base) {
        at = base & ~(kGranularity - 1);
        if (!free_at(at)) return 0;
    } else {
        // Candidate starts: the range's ends and the edges of each reservation.
        std::vector<uint64_t> cands;
        auto up = [&](uint64_t v) { return (v + align - 1) & ~uint64_t(align - 1); };
        auto down = [&](uint64_t v) { return v & ~uint64_t(align - 1); };
        if (!top_down) {
            cands.push_back(up(lim_lo));
            for (const auto& [b, r] : regions_) cands.push_back(up(uint64_t(b) + ((uint64_t(r.size) + kGranularity - 1) & ~uint64_t(kGranularity - 1))));
        } else {
            if (lim_hi >= bytes) cands.push_back(down(lim_hi - bytes));
            for (auto it = regions_.rbegin(); it != regions_.rend(); ++it)
                if (it->first >= bytes) cands.push_back(down(it->first - bytes));
        }
        bool found = false;
        for (uint64_t c : cands)
            if (free_at(c)) {
                at = c;
                found = true;
                break;
            }
        if (!found) return 0;
    }
    regions_[uint32_t(at)] = Region{uint32_t(bytes), protect, std::vector<uint32_t>(size_t(bytes / kPage), 0)};
    return uint32_t(at);
}

const std::pair<const uint32_t, VirtualMemory::Region>* VirtualMemory::find(uint32_t addr) const {
    auto it = regions_.upper_bound(addr);
    if (it == regions_.begin()) return nullptr;
    --it;
    return addr - it->first < it->second.size ? &*it : nullptr;
}

bool VirtualMemory::commit(uint32_t base, uint32_t size, uint32_t protect) {
    const uint32_t first = base & ~(kPage - 1);
    const uint64_t end = (uint64_t(base) + size + kPage - 1) & ~uint64_t(kPage - 1);
    const auto* r = find(first);
    if (!r || end > uint64_t(r->first) + r->second.size || end == first) return false;
    auto& pages = const_cast<Region&>(r->second).page_protect;
    for (uint64_t a = first; a < end; a += kPage) {
        uint32_t& p = pages[size_t((a - r->first) / kPage)];
        if (!p) std::memset(ram_ + a, 0, kPage);
        p = protect ? protect : 4;
    }
    return true;
}

bool VirtualMemory::decommit(uint32_t base, uint32_t size) {
    const uint32_t first = base & ~(kPage - 1);
    const auto* r = find(first);
    if (!r) return false;
    const uint64_t end = size ? (uint64_t(base) + size + kPage - 1) & ~uint64_t(kPage - 1) : uint64_t(r->first) + r->second.size;
    if (end > uint64_t(r->first) + r->second.size) return false;
    auto& pages = const_cast<Region&>(r->second).page_protect;
    for (uint64_t a = first; a < end; a += kPage) pages[size_t((a - r->first) / kPage)] = 0;
    return true;
}

bool VirtualMemory::release(uint32_t base) { return regions_.erase(base) == 1; }

uint32_t VirtualMemory::region_size(uint32_t base) const {
    const auto it = regions_.find(base);
    return it == regions_.end() ? 0 : it->second.size;
}

std::optional<VirtualMemory::Info> VirtualMemory::query(uint32_t addr) const {
    if (addr < lo_ || addr >= hi_) return std::nullopt;
    const uint32_t page = addr & ~(kPage - 1);
    if (const auto* r = find(page)) {
        const auto& pages = r->second.page_protect;
        size_t k = (page - r->first) / kPage;
        const uint32_t p = pages[k];
        size_t e = k;
        while (e < pages.size() && (pages[e] != 0) == (p != 0) && (!p || pages[e] == p)) ++e;
        return Info{page, r->first, r->second.protect, uint32_t((e - k) * kPage), p ? kMemCommit : kMemReserve, p, kMemPrivate};
    }
    // Free: up to the next reservation or the end of the range.
    const auto next = regions_.upper_bound(page);
    const uint32_t end = next == regions_.end() ? hi_ : next->first;
    return Info{page, 0, 0, end - page, kMemFree, 1, 0};  // PAGE_NOACCESS
}

uint64_t VirtualMemory::committed_bytes() const {
    uint64_t n = 0;
    for (const auto& [b, r] : regions_)
        for (uint32_t p : r.page_protect) n += p ? kPage : 0;
    return n;
}
uint64_t VirtualMemory::reserved_bytes() const {
    uint64_t n = 0;
    for (const auto& [b, r] : regions_) n += r.size;
    return n;
}
VirtualMemory& vm() {
    static VirtualMemory v;
    return v;
}

namespace {
MemoryMap g_map{kRetailRamMb << 20};
}
const MemoryMap& memory_map() { return g_map; }
void set_memory_map(MemoryMap m) { g_map = m; }

void set_regions(std::vector<Region> r) { regions() = std::move(r); }
void set_current(rt::Context* c) { g_current = c; }

std::string describe(uint32_t a) {
    if (a >= 0x80000000u && a < 0x80001000u) {
        const KernelExport* e = kernel_export(a & 0xFFFu);
        return e ? strf("kernel %s (ordinal %u)", e->name, unsigned(e->ordinal)) : strf("kernel ordinal %u", a & 0xFFFu);
    }
    for (const Region& r : regions())
        if (a >= r.lo && a < r.hi) return strf("%s code at %08X", r.name.c_str(), a);
    return strf("%08X", a);
}

}  // namespace qilin::host

namespace qilin::rt {

void call_unlifted(Context& c, uint32_t target) {
    const auto& r = host::registry();
    if (const auto it = r.find(target); it != r.end()) {
        // QILIN_TRACE_HLE=1 logs every replacement call: caller, arguments, result.
        static const bool trace = [] {
            const char* v = std::getenv("QILIN_TRACE_HLE");
            return v && *v && *v != '0';
        }();
        if (trace) {
            const Mem m{c.mem, c.mem_size};
            std::fprintf(stderr, "hle %08X %s(%08X %08X %08X %08X %08X %08X)", ld32(m, c.esp, 0), it->second.name,
                         host::arg(c, 0), host::arg(c, 1), host::arg(c, 2), host::arg(c, 3), host::arg(c, 4), host::arg(c, 5));
            it->second.fn(c);
            std::fprintf(stderr, " = %08X\n", c.eax);
            return;
        }
        it->second.fn(c);
        return;
    }
    const Mem m{c.mem, c.mem_size};
    const uint32_t caller = c.esp + 4 <= c.mem_size ? ld32(m, c.esp, 0) : 0;
    static std::string why;
    why = strf("no replacement for %s, called from %08X", host::describe(target).c_str(), caller);
    trap(target, why.c_str());
}

[[noreturn]] void trap(uint32_t guest_addr, const char* why) {
    std::fflush(stdout);
    std::fprintf(stderr, "qilin: stopped at %08X: %s\n", guest_addr, why);
    if (const Context* c = host::g_current)
        std::fprintf(stderr, "  eax %08X ecx %08X edx %08X ebx %08X esp %08X ebp %08X esi %08X edi %08X\n", c->eax,
                     c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
    std::exit(3);
}

}  // namespace qilin::rt
