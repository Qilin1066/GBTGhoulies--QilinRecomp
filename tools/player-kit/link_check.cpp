// Stands in for the code qilin lift makes from default.xbe (an empty table
// and the one function the start-up calls by name), so the player kit can be
// linked without it: CI builds this to show that libghoulies_kit.a and its
// system libraries are complete.
#include "guest.hpp"

namespace qilin::gen {
extern const Entry kFunctions[] = {{0u, nullptr}};
extern const size_t kFunctionCount = 0;
}  // namespace qilin::gen
