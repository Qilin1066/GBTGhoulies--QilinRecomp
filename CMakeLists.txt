cmake_minimum_required(VERSION 3.20)
project(GBTGQilincomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---- Zydis (vendored, MIT) ----
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_MAN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_ENCODER OFF CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_AVX512 OFF CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_KNC OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/zydis EXCLUDE_FROM_ALL)

# ---- Warnings ----
if(MSVC)
    set(QILIN_WARNINGS /W4 /permissive-)
else()
    set(QILIN_WARNINGS -Wall -Wextra -Wpedantic)
endif()

# ---- Recompiler core ----
add_library(qilin_core STATIC
    recomp/src/xbe.cpp
    recomp/src/decoder.cpp
    recomp/src/cfg.cpp
)
target_include_directories(qilin_core PUBLIC recomp/src)
target_link_libraries(qilin_core PUBLIC Zydis)
target_compile_options(qilin_core PRIVATE ${QILIN_WARNINGS})

add_executable(qilin recomp/src/main.cpp)
target_link_libraries(qilin PRIVATE qilin_core)
target_compile_options(qilin PRIVATE ${QILIN_WARNINGS})

# ---- Tests (no game files needed) ----
enable_testing()
add_executable(qilin_tests recomp/tests/tests.cpp)
target_link_libraries(qilin_tests PRIVATE qilin_core)
target_compile_options(qilin_tests PRIVATE ${QILIN_WARNINGS})
add_test(NAME qilin_tests COMMAND qilin_tests)
