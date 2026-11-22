#include "file_store.hpp"
#include <span>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace qilin::platform {
bool read_file(const std::filesystem::path& path, std::string& bytes) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return false;
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::string candidate;
    char block[65536];
    for (;;) {
        input.read(block, sizeof(block));
        candidate.append(block, static_cast<size_t>(input.gcount()));
        if (input.bad()) return false;
        if (input.eof()) break;
        if (input.fail()) return false;
    }
    bytes = std::move(candidate);
    return true;
}

bool atomic_write(const std::filesystem::path& destination, std::string_view text) {
    const auto bytes = std::span(text.data(), text.size());
    static std::atomic<unsigned> serial{0};
#ifdef _WIN32
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    std::filesystem::path temporary;
    std::FILE* file = nullptr;
    for (int attempt = 0; attempt < 32 && !file; ++attempt) {
        temporary = destination;
        temporary += ".tmp." + std::to_string(pid) + "." + std::to_string(serial.fetch_add(1));
#ifdef _WIN32
        file = _wfopen(temporary.c_str(), L"wbx");
#else
        file = std::fopen(temporary.c_str(), "wbx");
#endif
        if (!file && errno != EEXIST) break;
    }
    if (!file) return false;
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size() && std::fflush(file) == 0;
#ifdef _WIN32
    if (ok) ok = _commit(_fileno(file)) == 0;
#else
    if (ok) ok = fsync(fileno(file)) == 0;
#endif
    if (std::fclose(file) != 0) ok = false;
    if (ok) {
#ifdef _WIN32
        ok = MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        std::error_code ec;
        std::filesystem::rename(temporary, destination, ec);
        ok = !ec;
        if (ok) {
            const auto parent = destination.has_parent_path() ? destination.parent_path() : std::filesystem::path(".");
            const int directory = open(parent.c_str(), O_RDONLY | O_DIRECTORY);
            if (directory >= 0) { (void)fsync(directory); close(directory); }
        }
#endif
    }
    if (!ok) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
    }
    return ok;
}
} // namespace qilin::platform
