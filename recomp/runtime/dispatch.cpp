// Indirect-call dispatch for lifted code: rt::call looks the target up in the
// generated function table (binary search over kFunctions) and falls back to
// rt::call_unlifted, which the host provides (HLE in the runtime, Unicorn in
// the differential test). Compiled together with the generated code.
#include <algorithm>
#include <span>

#include "guest.hpp"

namespace qilin::rt {

void call(Context& c, uint32_t target) {
    const std::span<const gen::Entry> fns(gen::kFunctions, gen::kFunctionCount);
    const auto it = std::ranges::lower_bound(fns, target, {}, &gen::Entry::addr);
    if (it != fns.end() && it->addr == target) {
        it->fn(c);
        return;
    }
    call_unlifted(c, target);
}

}  // namespace qilin::rt
