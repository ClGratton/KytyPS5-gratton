# spirv-compile-bench: offline driver-compile timing tool (see docs/spirv-compile-bench.md).
#
# Built either from the top-level project (-DKYTY_BUILD_TOOLS=ON) or standalone:
#   cmake -S tools/spirv-compile-bench -B build/spirv-compile-bench -G Ninja \
#         -DKYTY_THIRD_PARTY_DIR=<path to 3rdparty>
# Needs only the Vulkan headers (3rdparty/Vulkan-Headers) and the system Vulkan loader.
cmake_minimum_required(VERSION 3.19)

if(NOT PROJECT_NAME)
	project(spirv-compile-bench CXX)
	set(CMAKE_CXX_STANDARD 20)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)
	if(NOT CMAKE_BUILD_TYPE)
		set(CMAKE_BUILD_TYPE Release)
	endif()
endif()

if(NOT KYTY_THIRD_PARTY_DIR)
	set(KYTY_THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty")
endif()

find_library(KYTY_VULKAN_LOADER NAMES vulkan libvulkan.so.1 REQUIRED)

add_executable(spirv-compile-bench main.cpp)
target_include_directories(spirv-compile-bench SYSTEM PRIVATE "${KYTY_THIRD_PARTY_DIR}/Vulkan-Headers/include")
target_link_libraries(spirv-compile-bench PRIVATE "${KYTY_VULKAN_LOADER}")
find_package(Threads REQUIRED)
target_link_libraries(spirv-compile-bench PRIVATE Threads::Threads)
if(NOT MSVC)
	target_compile_options(spirv-compile-bench PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()

# Pure python checks (no GPU): the SPIR-V rewrites validate with spirv-val and keep the module sane.
find_program(PYTHON3 python3)
if(PYTHON3 AND BUILD_TESTING)
	add_test(NAME spirv_compile_bench_variants
		COMMAND "${PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/test_variants.py")
	set_tests_properties(spirv_compile_bench_variants PROPERTIES
		ENVIRONMENT "KYTY_SPIRV_GRAMMAR=${KYTY_THIRD_PARTY_DIR}/SPIRV-Headers/include/spirv/unified1/spirv.core.grammar.json")
endif()
