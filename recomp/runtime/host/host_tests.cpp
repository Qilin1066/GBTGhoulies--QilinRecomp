// Tests for the runtime's host side; no game files needed.
#include <cstring>
#include <memory>
#include <cstdio>
#include <string>

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

    if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
    else std::printf("all host tests passed\n");
    return failures != 0;
}
