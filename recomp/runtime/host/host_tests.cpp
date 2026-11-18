// Tests for the runtime's host side; no game files needed.
#include <cstring>
#include <memory>
#include <print>
#include <string>

#include "host.hpp"

using namespace qilin;

namespace {
int failures = 0;
#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::println(stderr, "{}:{}: CHECK failed: {}", __FILE__, __LINE__, #cond); \
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

    // The guest heap: page-granular first fit, zeroed, with sizes and frees.
    host::GuestHeap h;
    auto heap_ram = std::make_unique<uint8_t[]>(0x10000);
    std::memset(heap_ram.get(), 0xAB, 0x10000);
    h.init(heap_ram.get(), 0x1000, 0x5000);  // four pages
    const uint32_t a = h.alloc(10), b = h.alloc(0x1001), d = h.alloc(1);
    CHECK(a == 0x1000 && b == 0x2000 && d == 0x4000 && h.alloc(1) == 0);
    CHECK(heap_ram[a] == 0 && heap_ram[b + 0x1FFF] == 0);  // zeroed, whole pages
    CHECK(h.size_of(b) == 0x1001 && h.size_of(b + 4) == 0 && h.available() == 0);
    CHECK(h.free(b) && !h.free(b) && h.available() == 0x2000);
    CHECK(h.alloc(0x2000) == 0x2000 && h.alloc(0) == 0);

    if (failures) std::println(stderr, "{} check(s) failed", failures);
    else std::println("all host tests passed");
    return failures != 0;
}
