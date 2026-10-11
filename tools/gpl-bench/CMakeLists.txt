# Standalone build of the D2-spike GPL benchmark (not part of the emulator target).
#   cmake -S tools/gpl-bench -B build/d2-spike/gpl-bench -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake_minimum_required(VERSION 3.18)
project(gpl_bench CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(KYTY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_executable(gpl_bench gpl_bench.cpp)
target_include_directories(gpl_bench PRIVATE
    "${KYTY_ROOT}/3rdparty/Vulkan-Headers/include"
    "${KYTY_ROOT}/3rdparty/nlohmann_json/single_include"
    "${CMAKE_CURRENT_SOURCE_DIR}")
# System Vulkan loader (no -dev symlink dependency): link libvulkan.so.1 by name.
find_library(VULKAN_LOADER NAMES vulkan libvulkan.so.1 REQUIRED)
target_link_libraries(gpl_bench PRIVATE ${VULKAN_LOADER} pthread)
target_compile_options(gpl_bench PRIVATE -O2 -Wall -Wextra -Wno-missing-field-initializers)
