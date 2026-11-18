#include "host.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
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

void GuestHeap::init(uint8_t* ram, uint32_t lo, uint32_t hi) {
    ram_ = ram;
    lo_ = lo;
    hi_ = hi;
    used_.clear();
}
uint32_t GuestHeap::alloc(uint32_t bytes) {
    const uint64_t need = (uint64_t(bytes) + 0xFFF) & ~uint64_t(0xFFF);
    if (need == 0 || need > hi_ - lo_) return 0;
    uint32_t at = lo_;
    size_t i = 0;
    for (; i < used_.size(); ++i) {
        if (used_[i].first - at >= need) break;
        at = uint32_t(used_[i].first + ((uint64_t(used_[i].second) + 0xFFF) & ~uint64_t(0xFFF)));
    }
    if (i == used_.size() && hi_ - at < need) return 0;
    used_.insert(used_.begin() + std::ptrdiff_t(i), {at, bytes});
    std::memset(ram_ + at, 0, size_t(need));
    return at;
}
bool GuestHeap::free(uint32_t addr) {
    const auto it = std::ranges::lower_bound(used_, addr, {}, &std::pair<uint32_t, uint32_t>::first);
    if (it == used_.end() || it->first != addr) return false;
    used_.erase(it);
    return true;
}
uint32_t GuestHeap::size_of(uint32_t addr) const {
    const auto it = std::ranges::lower_bound(used_, addr, {}, &std::pair<uint32_t, uint32_t>::first);
    return it != used_.end() && it->first == addr ? it->second : 0;
}
uint32_t GuestHeap::available() const {
    uint64_t used = 0;
    for (const auto& [a, n] : used_) used += (uint64_t(n) + 0xFFF) & ~uint64_t(0xFFF);
    return uint32_t(total() - used);
}
GuestHeap& heap() {
    static GuestHeap h;
    return h;
}

void set_regions(std::vector<Region> r) { regions() = std::move(r); }
void set_current(rt::Context* c) { g_current = c; }

std::string describe(uint32_t a) {
    if (a >= 0x80000000u && a < 0x80001000u) {
        const KernelExport* e = kernel_export(a & 0xFFFu);
        return e ? std::format("kernel {} (ordinal {})", e->name, e->ordinal) : std::format("kernel ordinal {}", a & 0xFFFu);
    }
    for (const Region& r : regions())
        if (a >= r.lo && a < r.hi) return std::format("{} code at {:08X}", r.name, a);
    return std::format("{:08X}", a);
}

}  // namespace qilin::host

namespace qilin::rt {

void call_unlifted(Context& c, uint32_t target) {
    const auto& r = host::registry();
    if (const auto it = r.find(target); it != r.end()) {
        it->second.fn(c);
        return;
    }
    const Mem m{c.mem, c.mem_size};
    const uint32_t caller = c.esp + 4 <= c.mem_size ? ld32(m, c.esp, 0) : 0;
    static std::string why;
    why = std::format("no replacement for {}, called from {:08X}", host::describe(target), caller);
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
