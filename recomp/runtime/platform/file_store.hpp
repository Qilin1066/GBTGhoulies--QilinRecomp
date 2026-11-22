#pragma once
#include <filesystem>
#include <string_view>
#include <string>

namespace qilin::platform {
// Unique sibling temporary, checked write/flush/close, then atomic replacement.
// Failure before replacement preserves the destination, including on Windows.
bool read_file(const std::filesystem::path& path, std::string& bytes);
bool atomic_write(const std::filesystem::path& destination, std::string_view text);
}
