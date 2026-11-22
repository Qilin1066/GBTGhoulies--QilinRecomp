#pragma once
#include <filesystem>
#include <string_view>
#include <string>

namespace qilin::platform {
// Unique sibling temporary, checked write/flush/close, then atomic replacement.
// Failure before replacement preserves the destination, including on Windows.
bool read_file(const std::filesystem::path& path, std::string& bytes);
// Stream a regular file through the same checked replacement path.
bool atomic_copy(const std::filesystem::path& source, const std::filesystem::path& destination);
bool atomic_write(const std::filesystem::path& destination, std::string_view text);
}
