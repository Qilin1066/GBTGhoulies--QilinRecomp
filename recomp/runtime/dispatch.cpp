// Indirect-call dispatch for lifted code: rt::call looks the target up in the
// generated function table (binary search over kFunctions) and falls back to
// rt::call_unlifted, which the host provides (HLE in the runtime, Unicorn in
// the differential test). Compiled together with the generated code.
#include <algorithm>
#include <span>

#include "guest.hpp"

namespace qilin::rt {

namespace {
const gen::Entry* find(uint32_t target) {
    const std::span<const gen::Entry> fns(gen::kFunctions, gen::kFunctionCount);
    const auto it = std::ranges::lower_bound(fns, target, {}, &gen::Entry::addr);
    return it != fns.end() && it->addr == target ? &*it : nullptr;
}
}  // namespace

void call(Context& c, uint32_t target) {
    if (const gen::Entry* e = find(target)) {
        e->fn(c);
        return;
    }
    call_unlifted(c, target);
}

bool is_lifted(uint32_t target) { return find(target) != nullptr; }

}  // namespace qilin::rt
