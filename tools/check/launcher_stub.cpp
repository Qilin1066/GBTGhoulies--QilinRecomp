// Native child processes for launcher regression; no game code or disc data.
#include "command_line.hpp"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
int main(int argc, char** argv) {
    const qilin::CommandLine command_line(argc, argv);
    argc = command_line.count(); argv = command_line.values();
    if (argc == 3 && std::strcmp(argv[1], "info") == 0) {
        std::puts("{\"certificate\":{\"title_id\":\"4D530053\"},\"timestamp\":1064426049}"); return 0;
    }
    if (argc == 5 && std::strcmp(argv[1], "lift") == 0) {
        const auto dir = qilin::native_path(argv[4]);
        std::filesystem::create_directories(dir);
        std::ofstream(dir / "fixture.cpp") << "// fixture\n"; return 0;
    }
    if (argc == 5 && std::strcmp(argv[1], "-C") == 0) {
        std::filesystem::copy_file(qilin::native_path(argv[0]), qilin::native_path(argv[2]) / "Ghoulies.exe",
                                  std::filesystem::copy_options::overwrite_existing); return 0;
    }
    if (argc == 4 && std::strcmp(argv[1], "--game-dir") == 0 &&
        std::filesystem::exists(qilin::native_path(argv[2]) / "default.xbe") &&
        qilin::native_path(argv[3]) == qilin::native_path(argv[2]) / "default.xbe") {
        std::printf("launched: %s\n", argv[2]); return 0;
    }
    return 2;
}
