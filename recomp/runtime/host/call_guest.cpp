// host::call_guest, in its own file: it needs the dispatcher (rt::call), which
// only builds made from lifted code have, so it's linked in only where used.
#include "host.hpp"

namespace qilin::host {

void call_guest(rt::Context& c, uint32_t fn, std::initializer_list<uint32_t> args) {
    const rt::Mem m{c.mem, c.mem_size};
    const uint32_t esp = c.esp;
    for (auto it = std::rbegin(args); it != std::rend(args); ++it) {
        c.esp -= 4;
        rt::st32(m, c.esp, *it, 0);
    }
    c.esp -= 4;
    rt::st32(m, c.esp, kReturnSentinel, 0);
    rt::call(c, fn);
    if (c.ret_to != kReturnSentinel) rt::trap(fn, "a guest function called from the runtime returned somewhere else");
    c.esp = esp;
}

}  // namespace qilin::host
