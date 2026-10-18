cmake_minimum_required(VERSION 3.19...4.3)

if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0)
	set(CMAKE_POLICY_VERSION_MINIMUM 3.10 CACHE STRING "Minimum policy version for bundled dependencies")
endif()

get_property(KYTY_GENERATOR_IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT KYTY_GENERATOR_IS_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build" FORCE)
	set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

project(Kyty)

find_program(SCCACHE_PROGRAM sccache)
find_program(CCACHE_PROGRAM ccache)

foreach(lang IN ITEMS C CXX)
    # An explicitly empty launcher disables caching for this language.
    if(NOT DEFINED CMAKE_${lang}_COMPILER_LAUNCHER)
        if(SCCACHE_PROGRAM)
            set(CMAKE_${lang}_COMPILER_LAUNCHER "${SCCACHE_PROGRAM}")
            message(STATUS "sccache enabled for ${lang}: ${SCCACHE_PROGRAM}")
        elseif(CCACHE_PROGRAM)
            set(CMAKE_${lang}_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
            message(STATUS "ccache enabled for ${lang}: ${CCACHE_PROGRAM}")
        endif()
    endif()
endforeach()

set(KYTY_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(KYTY_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests")
set(KYTY_THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty")

if(CMAKE_SYSTEM_NAME MATCHES ".*Linux")
	set(LINUX TRUE)
endif()

if (NOT (WIN32 OR LINUX OR APPLE))
    message(FATAL_ERROR "only Windows, Linux, and macOS builds are supported")
endif()

if(WIN32)
	# Prevent Windows headers from defining min/max macros in all targets.
	add_compile_definitions(NOMINMAX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

include("${KYTY_SOURCE_DIR}/utils.cmake")
include(CTest)

option(KYTY_ENABLE_CLANG_TIDY "Run clang-tidy checks during builds" OFF)

set(KYTY_BUILD_ORIGIN "Source" CACHE STRING "Build provenance shown in diagnostics")
set_property(CACHE KYTY_BUILD_ORIGIN PROPERTY STRINGS Source Official Fork)
if(NOT KYTY_BUILD_ORIGIN STREQUAL "Source" AND NOT KYTY_BUILD_ORIGIN STREQUAL "Official" AND
   NOT KYTY_BUILD_ORIGIN STREQUAL "Fork")
	message(FATAL_ERROR "KYTY_BUILD_ORIGIN must be Source, Official, or Fork")
endif()

set(KYTY_RELEASE_TAG "" CACHE STRING "Release tag embedded in official builds")
set(KYTY_BUILD_REPOSITORY "" CACHE STRING "GitHub repository embedded in fork builds")
set(KYTY_OFFICIAL_BUILD OFF)
set(KYTY_FORK_BUILD OFF)
if(KYTY_BUILD_ORIGIN STREQUAL "Official")
	if(KYTY_RELEASE_TAG STREQUAL "")
		message(FATAL_ERROR "Official builds require KYTY_RELEASE_TAG")
	endif()
	set(KYTY_OFFICIAL_BUILD ON)
elseif(KYTY_BUILD_ORIGIN STREQUAL "Fork")
	if(KYTY_BUILD_REPOSITORY STREQUAL "")
		message(FATAL_ERROR "Fork builds require KYTY_BUILD_REPOSITORY")
	endif()
	set(KYTY_FORK_BUILD ON)
endif()

set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

include(TestBigEndian)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "only 64-bit builds are supported")
endif()

test_big_endian(BIG_ENDIAN)
if(BIG_ENDIAN)
	set(KYTY_ENDIAN KYTY_ENDIAN_BIG)
else()
	set(KYTY_ENDIAN KYTY_ENDIAN_LITTLE)
endif()

if(CMAKE_BUILD_TYPE MATCHES Debug)
	set(KYTY_BUILD KYTY_BUILD_DEBUG)
else()
	set(KYTY_BUILD KYTY_BUILD_RELEASE)
endif()

if(LINUX OR APPLE)
	# macOS rides the POSIX/Linux code paths until it gets a dedicated platform
	set(KYTY_PLATFORM KYTY_PLATFORM_LINUX)
else()
	set(KYTY_PLATFORM KYTY_PLATFORM_WINDOWS)
endif()

set(CLANG FALSE)
set(GCC FALSE)
set(KYTY_CLANG_CL FALSE)
get_filename_component(KYTY_CXX_COMPILER_NAME "${CMAKE_CXX_COMPILER}" NAME_WE)
string(TOLOWER "${KYTY_CXX_COMPILER_NAME}" KYTY_CXX_COMPILER_NAME)

if(CMAKE_CXX_COMPILER_ID MATCHES "^(Apple)?Clang$")
	set(CLANG TRUE)
	set(KYTY_COMPILER CLANG)
	if(WIN32 AND KYTY_CXX_COMPILER_NAME STREQUAL "clang-cl")
		set(KYTY_CLANG_CL TRUE)
		set(KYTY_LINKER LLD_LINK)
	elseif(APPLE)
		set(KYTY_LINKER LD64) # Apple's default linker; lld flags don't apply
	else()
		set(KYTY_LINKER LLD)
	endif()
	if ((CMAKE_AR MATCHES "llvm") AND (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12.0.0) AND (CMAKE_CXX_COMPILER MATCHES "winlibs"))
		string(REPLACE "llvm-ar.exe" "ar.exe" CMAKE_AR ${CMAKE_AR}) # llvm-ar.exe 11.0.0 from winlibs is broken
	endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	set(GCC TRUE)
	set(KYTY_COMPILER GCC)
	set(KYTY_LINKER LD)
else()
	message(FATAL_ERROR "only Clang and GCC are supported; found ${CMAKE_CXX_COMPILER_ID}")
endif()

set(KYTY_PROJECT_NAME "Emulator" CACHE STRING "Project name suffix")

string(TOLOWER ${KYTY_COMPILER} KYTY_COMPILER_ID)
string(TOLOWER ${KYTY_LINKER} KYTY_LINKER_ID)

if (CLANG AND (KYTY_LINKER STREQUAL LLD))
	set(KYTY_LD_OPTIONS "-fuse-ld=lld")
endif()

if (KYTY_LINKER STREQUAL LD AND NOT LINUX)
	set(KYTY_LD_OPTIONS "-Wl,--image-base=0x100000000000")
endif()

project(Kyty${KYTY_PROJECT_NAME}${CMAKE_BUILD_TYPE}${KYTY_COMPILER} VERSION 0.3.0)

include_directories(
	${KYTY_THIRD_PARTY_DIR}/gtest/include
	${KYTY_THIRD_PARTY_DIR}/gtest
	${KYTY_THIRD_PARTY_DIR}/fmt/include
	${KYTY_THIRD_PARTY_DIR}/magic_enum/include/magic_enum
	${PROJECT_BINARY_DIR}
	${KYTY_SOURCE_DIR}
)

set(KYTY_VERSION "${PROJECT_VERSION}")

configure_file(
  ${KYTY_SOURCE_DIR}/cmake_config.h.in
  ${PROJECT_BINARY_DIR}/cmake_config.h
  )

find_package(Git)

add_custom_target(KytyGitVersion
	COMMAND ${CMAKE_COMMAND}
		-D INPUT_FILE=${KYTY_SOURCE_DIR}/kytyGitVersion.h.in
		-D OUTPUT_FILE=${PROJECT_BINARY_DIR}/kytyGitVersion.h
		-D GIT_EXECUTABLE=${GIT_EXECUTABLE}
		-D GIT_WORKING_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}
		-P ${KYTY_SOURCE_DIR}/generate_version.cmake
	BYPRODUCTS ${PROJECT_BINARY_DIR}/kytyGitVersion.h
	COMMENT "Generate kytyGitVersion.h"
)

add_library(kyty_git_version INTERFACE)
add_dependencies(kyty_git_version KytyGitVersion)

if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 12.0.0)
	list(APPEND KYTY_IWYU
		kyty_emulator
		common
		#launcher
	)
	list(APPEND KYTY_CLANG_TIDY
		kyty_emulator
		#common
		launcher
	)
endif()

option(KYTY_BUILD_LAUNCHER "Build Qt launcher" ON)

config_compiler_and_linker()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/KytyOptimization.cmake")

add_subdirectory("${KYTY_THIRD_PARTY_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/3rdparty")
add_subdirectory("${KYTY_SOURCE_DIR}/common" "${CMAKE_CURRENT_BINARY_DIR}/common")

file(GLOB kyty_emulator_src CONFIGURE_DEPENDS
	"${KYTY_SOURCE_DIR}/libs/*.cpp"
	"${KYTY_SOURCE_DIR}/libs/*.h"
	"${KYTY_SOURCE_DIR}/graphics/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/*.h"
	"${KYTY_SOURCE_DIR}/graphics/guest_gpu/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/guest_gpu/*.h"
	"${KYTY_SOURCE_DIR}/graphics/guest_gpu/command_processor/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/guest_gpu/command_processor/*.h"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/*.h"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/renderer/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/renderer/*.h"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/renderer/cache/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/renderer/cache/*.h"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/renderer/image/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/renderer/image/*.h"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/renderer/pipeline/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/renderer/pipeline/*.h"
	"${KYTY_SOURCE_DIR}/graphics/shader/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/*.h"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/*.h"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/frontend/cfg/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/frontend/cfg/*.h"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/frontend/decode/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/frontend/decode/*.h"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/frontend/translate/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/frontend/translate/*.h"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/backend/spirv/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/backend/spirv/*.h"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/*.h"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/opcodes/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/opcodes/*.h"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/opcodes/*.inc"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/*.h"
	"${KYTY_SOURCE_DIR}/graphics/presentation/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/presentation/*.h"
	"${KYTY_SOURCE_DIR}/graphics/presentation/window/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/presentation/window/*.h"
	"${KYTY_SOURCE_DIR}/kernel/*.cpp"
	"${KYTY_SOURCE_DIR}/kernel/*.h"
	"${KYTY_SOURCE_DIR}/loader/*.cpp"
	"${KYTY_SOURCE_DIR}/loader/*.h"
)

find_program(KYTY_GLSLANG_VALIDATOR NAMES glslangValidator glslang REQUIRED)
set(gpu_tiler_shader_dir "${KYTY_SOURCE_DIR}/graphics/host_gpu/shaders")
set(gpu_tiler_generated_dir "${PROJECT_BINARY_DIR}/gpu_tiler_shaders")
set(gpu_tiler_shader_names
	standard256
	standard4
	standard4_3d
	standard64
	standard64_3d
	prt
	prt_3d
	render_target
	depth
	promote_d16
	demote_d16
	color_transform
	copy_up4
	copy_up8
	copy_up16
	copy_down4
	copy_down8
	copy_down16
)
file(GLOB gpu_tiler_shader_includes CONFIGURE_DEPENDS "${gpu_tiler_shader_dir}/gpu_tiler_*.inc")
foreach(shader_name IN LISTS gpu_tiler_shader_names)
	set(shader_source "${gpu_tiler_shader_dir}/gpu_tiler_${shader_name}.comp")
	set(shader_spv "${gpu_tiler_generated_dir}/gpu_tiler_${shader_name}.spv")
	set(shader_header "${gpu_tiler_generated_dir}/gpu_tiler_${shader_name}_spv.h")
	string(TOUPPER "GPU_TILER_${shader_name}_SPV" shader_symbol)
	add_custom_command(
		OUTPUT "${shader_header}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${gpu_tiler_generated_dir}"
		COMMAND "${KYTY_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 -Os
		        "-I${gpu_tiler_shader_dir}" -o "${shader_spv}" "${shader_source}"
		COMMAND ${CMAKE_COMMAND} -DINPUT=${shader_spv} -DOUTPUT=${shader_header}
		        -DSYMBOL=${shader_symbol} -P "${KYTY_SOURCE_DIR}/embed_spirv.cmake"
		DEPENDS "${shader_source}" ${gpu_tiler_shader_includes}
		VERBATIM
	)
	list(APPEND gpu_tiler_shader_headers "${shader_header}")
endforeach()

set(fault_buffer_shader_source "${gpu_tiler_shader_dir}/fault_buffer_process.comp")
set(fault_buffer_shader_spv "${gpu_tiler_generated_dir}/fault_buffer_process.spv")
set(fault_buffer_shader_header "${gpu_tiler_generated_dir}/fault_buffer_process_spv.h")
add_custom_command(
	OUTPUT "${fault_buffer_shader_header}"
	COMMAND ${CMAKE_COMMAND} -E make_directory "${gpu_tiler_generated_dir}"
	COMMAND "${KYTY_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 -Os
	        -DCACHING_PAGEBITS=14 -DMAX_PAGE_FAULTS=1024
	        -o "${fault_buffer_shader_spv}" "${fault_buffer_shader_source}"
	COMMAND ${CMAKE_COMMAND} -DINPUT=${fault_buffer_shader_spv} -DOUTPUT=${fault_buffer_shader_header}
	        -DSYMBOL=FAULT_BUFFER_PROCESS_SPV -P "${KYTY_SOURCE_DIR}/embed_spirv.cmake"
	DEPENDS "${fault_buffer_shader_source}"
	VERBATIM
)
list(APPEND kyty_emulator_src "${fault_buffer_shader_header}")
list(APPEND kyty_emulator_src ${gpu_tiler_shader_headers})

set(mesh_indirect_shader_source "${gpu_tiler_shader_dir}/mesh_indirect_args.comp")
set(mesh_indirect_shader_spv "${gpu_tiler_generated_dir}/mesh_indirect_args.spv")
set(mesh_indirect_shader_header "${gpu_tiler_generated_dir}/mesh_indirect_args_spv.h")
add_custom_command(
	OUTPUT "${mesh_indirect_shader_header}"
	COMMAND ${CMAKE_COMMAND} -E make_directory "${gpu_tiler_generated_dir}"
	COMMAND "${KYTY_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 -Os
	        -o "${mesh_indirect_shader_spv}" "${mesh_indirect_shader_source}"
	COMMAND ${CMAKE_COMMAND} -DINPUT=${mesh_indirect_shader_spv} -DOUTPUT=${mesh_indirect_shader_header}
	        -DSYMBOL=MESH_INDIRECT_ARGS_SPV -P "${KYTY_SOURCE_DIR}/embed_spirv.cmake"
	DEPENDS "${mesh_indirect_shader_source}"
	VERBATIM
)
list(APPEND kyty_emulator_src "${mesh_indirect_shader_header}")

set(gpu_blit_generated_dir "${PROJECT_BINARY_DIR}/gpu_blit_shaders")
set(gpu_blit_shader_sources
	"${gpu_tiler_shader_dir}/gpu_blit_fs_triangle.vert"
	"${gpu_tiler_shader_dir}/gpu_blit_color_to_ms_depth.frag"
	"${gpu_tiler_shader_dir}/gpu_video_out_overlay.frag"
)
foreach(shader_source IN LISTS gpu_blit_shader_sources)
	get_filename_component(shader_name "${shader_source}" NAME_WE)
	set(shader_spv "${gpu_blit_generated_dir}/${shader_name}.spv")
	set(shader_header "${gpu_blit_generated_dir}/${shader_name}_spv.h")
	string(TOUPPER "${shader_name}_SPV" shader_symbol)
	add_custom_command(
		OUTPUT "${shader_header}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${gpu_blit_generated_dir}"
		COMMAND "${KYTY_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 -Os
		        "-I${gpu_tiler_shader_dir}" -o "${shader_spv}" "${shader_source}"
		COMMAND ${CMAKE_COMMAND} -DINPUT=${shader_spv} -DOUTPUT=${shader_header}
		        -DSYMBOL=${shader_symbol} -P "${KYTY_SOURCE_DIR}/embed_spirv.cmake"
		DEPENDS "${shader_source}"
		VERBATIM
	)
	list(APPEND gpu_blit_shader_headers "${shader_header}")
endforeach()
list(APPEND kyty_emulator_src ${gpu_blit_shader_headers})

list(APPEND kyty_emulator_src
	"${KYTY_SOURCE_DIR}/emulator.h"
	"${KYTY_SOURCE_DIR}/emulator.cpp"
)
list(REMOVE_DUPLICATES kyty_emulator_src)

if (KYTY_CLANG_CL)
	set_source_files_properties(${kyty_emulator_src} PROPERTIES COMPILE_FLAGS "-Wno-pragma-pack -Wno-deprecated-declarations -D_TIMESPEC_DEFINED")
endif()

set(kyty_emulator_link_libraries common Vulkan::Headers spirv-tools-opt spirv-tools SDL3::SDL3-static imgui xxhash FFmpeg::ffmpeg fmt::fmt nlohmann_json::nlohmann_json LibAtrac9 kyty_git_version Zydis::Zydis xbyak::xbyak opus)

# Linux system libraries.
if(LINUX)
	find_package(Threads REQUIRED)
	list(APPEND kyty_emulator_link_libraries m ${CMAKE_DL_LIBS} Threads::Threads)
endif()

set(inc_headers
	${KYTY_SOURCE_DIR}
	${KYTY_THIRD_PARTY_DIR}/VulkanMemoryAllocator/include
	${KYTY_THIRD_PARTY_DIR}/SPIRV-Tools/include
	${KYTY_THIRD_PARTY_DIR}/SPIRV-Headers/include
	${KYTY_THIRD_PARTY_DIR}/fmt/include
	${KYTY_THIRD_PARTY_DIR}/xxhash
	${KYTY_THIRD_PARTY_DIR}/cpuinfo/include
	${KYTY_THIRD_PARTY_DIR}/renderdoc
	${KYTY_THIRD_PARTY_DIR}/stb
)

if (KYTY_CLANG_CL)
	list(APPEND kyty_emulator_link_libraries winpthread)
	list(APPEND inc_headers
		${KYTY_THIRD_PARTY_DIR}/winpthread/include
	)
endif()

list(APPEND check_headers
	${KYTY_SOURCE_DIR}
)

function(add_kyty_full_emulator_test target source)
	add_executable(${target} EXCLUDE_FROM_ALL ${source} ${kyty_emulator_src})
	target_link_libraries(${target} ${kyty_emulator_link_libraries})
	target_include_directories(${target} PRIVATE ${inc_headers})
	if (WIN32)
		target_link_libraries(${target} iphlpapi)
	endif()
	if (KYTY_CLANG_CL)
		target_link_libraries(${target} onecore)
		add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KYTY_THIRD_PARTY_DIR}/winpthread/bin/libwinpthread-1.dll" $<TARGET_FILE_DIR:${target}>/libwinpthread-1.dll)
	endif()
	# The macOS x86_64 guest address space needs its .zerofill segments anchored
	# by linker flags, or the kernel kills the binary on load (posix_spawn EIO).
	configure_macos_guest_address_space(${target})
endfunction()

function(configure_macos_guest_address_space target)
	if(APPLE AND (CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64" OR
			(NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")))
		target_sources(${target} PRIVATE "${KYTY_SOURCE_DIR}/kernel/macosGuestAddressSpace.cpp")
		target_compile_definitions(${target} PRIVATE KYTY_LINKED_GUEST_ADDRESS_SPACE=1)
		target_link_options(${target} PRIVATE
			-Wl,-ld_classic,-no_pie,-no_fixup_chains,-no_huge,-pagezero_size,0x40000,-segaddr,SYSTEM_MANAGED,0x40000,-segaddr,SYSTEM_RESERVED,0x7ffffc000,-segaddr,USER_AREA,0x7000000000,-image_base,0x700000000000)
	endif()
endfunction()

add_kyty_full_emulator_test(shader_cfg_tests "${KYTY_TESTS_DIR}/shaderCfgTests.cpp")
add_kyty_full_emulator_test(dynamic_state_tests "${KYTY_TESTS_DIR}/DynamicStateTests.cpp")
add_kyty_full_emulator_test(slot_vector_tests "${KYTY_TESTS_DIR}/SlotVectorTests.cpp")

add_executable(scalar_provenance_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ScalarProvenanceTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/guest_gpu/gpu_format.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/hostMemory.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/ReadLaneElimination.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/ResourceMaterialization.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/SrtWalker.cpp"
)
target_link_libraries(scalar_provenance_tests fmt::fmt xbyak::xbyak)
target_include_directories(scalar_provenance_tests PRIVATE ${inc_headers})

add_executable(page_manager_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/PageManagerTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/pageManager.cpp"
)
target_link_libraries(page_manager_tests common)
target_include_directories(page_manager_tests PRIVATE ${inc_headers})

add_executable(mem_residency_ranges_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/MemResidencyRangesTests.cpp"
)
target_include_directories(mem_residency_ranges_tests PRIVATE ${inc_headers})

add_executable(archive_file_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ArchiveFileTests.cpp"
)
target_link_libraries(archive_file_tests common ZArchive::zarchive)
target_include_directories(archive_file_tests PRIVATE ${inc_headers})

add_executable(trophy_system_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/TrophySystemTests.cpp"
)
target_link_libraries(trophy_system_tests common)
target_include_directories(trophy_system_tests PRIVATE ${inc_headers})

add_executable(avplayer_file_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/AvPlayerFileTests.cpp"
	"${KYTY_SOURCE_DIR}/loader/timer.cpp"
)
target_link_libraries(avplayer_file_tests common FFmpeg::ffmpeg ZArchive::zarchive)
target_include_directories(avplayer_file_tests PRIVATE ${inc_headers})

add_executable(bit_array_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/BitArrayTests.cpp"
)
target_include_directories(bit_array_tests PRIVATE ${inc_headers})

add_executable(atomic_bit_mirror_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/AtomicBitMirrorTests.cpp"
)
target_include_directories(atomic_bit_mirror_tests PRIVATE ${inc_headers})

add_executable(gds_copy_plan_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/GdsCopyPlanTests.cpp"
)
target_include_directories(gds_copy_plan_tests PRIVATE ${inc_headers})

add_executable(playgo_api_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/PlayGoApiTests.cpp"
	"${KYTY_SOURCE_DIR}/libs/libPlayGo.cpp"
	"${KYTY_SOURCE_DIR}/loader/symbolDatabase.cpp"
)
target_link_libraries(playgo_api_tests common fmt::fmt)
target_include_directories(playgo_api_tests PRIVATE ${inc_headers})

add_executable(swapchain_extent_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/SwapchainExtentTests.cpp"
)
target_include_directories(swapchain_extent_tests PRIVATE ${inc_headers})

add_executable(null_page_read_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/NullPageReadTests.cpp"
)
target_include_directories(null_page_read_tests PRIVATE ${inc_headers})

add_executable(compare_view_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/CompareViewTests.cpp"
)
target_include_directories(compare_view_tests PRIVATE ${inc_headers})

add_executable(mesh_dispatch_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/MeshDispatchTests.cpp"
)
target_include_directories(mesh_dispatch_tests PRIVATE ${inc_headers})

add_executable(sys_file_read_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/SysFileReadTests.cpp"
)
target_link_libraries(sys_file_read_tests common)
target_include_directories(sys_file_read_tests PRIVATE ${inc_headers})

add_executable(lru_cache_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/LruCacheTests.cpp"
)
target_include_directories(lru_cache_tests PRIVATE ${inc_headers})

add_executable(vram_policy_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/VramPolicyTests.cpp"
)
target_include_directories(vram_policy_tests PRIVATE ${inc_headers})

add_executable(heap_trim_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/HeapTrimTests.cpp"
)
target_include_directories(heap_trim_tests PRIVATE ${inc_headers})

add_executable(texture_gc_policy_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/TextureGcPolicyTests.cpp"
)
target_include_directories(texture_gc_policy_tests PRIVATE ${inc_headers})

add_executable(phase_timers_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/PhaseTimersTests.cpp"
)
target_include_directories(phase_timers_tests PRIVATE ${inc_headers})

add_executable(runtime_toggles_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/RuntimeTogglesTests.cpp"
)
target_include_directories(runtime_toggles_tests PRIVATE ${inc_headers})

add_executable(submit_coalescer_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/SubmitCoalescerTests.cpp"
)
target_include_directories(submit_coalescer_tests PRIVATE ${inc_headers})

add_executable(drain_causes_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/DrainCausesTests.cpp"
)
target_include_directories(drain_causes_tests PRIVATE ${inc_headers})

add_executable(barrier_elision_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/BarrierElisionTests.cpp"
)
target_include_directories(barrier_elision_tests PRIVATE ${inc_headers})

add_executable(pending_writes_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/PendingWritesTests.cpp"
)
target_include_directories(pending_writes_tests PRIVATE ${inc_headers})

add_executable(dispatch_hazard_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/DispatchHazardTests.cpp"
)
target_include_directories(dispatch_hazard_tests PRIVATE ${inc_headers})

add_executable(known_fills_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/KnownFillsTests.cpp"
)
target_include_directories(known_fills_tests PRIVATE ${inc_headers})

add_executable(known_copies_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/KnownCopiesTests.cpp"
)
target_include_directories(known_copies_tests PRIVATE ${inc_headers})

add_executable(known_stores_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/KnownStoresTests.cpp"
)
target_include_directories(known_stores_tests PRIVATE ${inc_headers})

add_executable(draw_key_memo_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/DrawKeyMemoTests.cpp"
)
target_include_directories(draw_key_memo_tests PRIVATE ${inc_headers})
add_executable(image_lookup_memo_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ImageLookupMemoTests.cpp"
)
target_include_directories(image_lookup_memo_tests PRIVATE ${inc_headers})
target_link_libraries(image_lookup_memo_tests PRIVATE Vulkan::Headers)
add_executable(srt_plan_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/SrtPlanTests.cpp"
)
target_include_directories(srt_plan_tests PRIVATE ${inc_headers})
add_executable(lookahead_governor_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/LookaheadGovernorTests.cpp"
)
target_include_directories(lookahead_governor_tests PRIVATE ${inc_headers})
add_executable(scalar_read_hazard_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ScalarReadHazardTests.cpp"
)
target_include_directories(scalar_read_hazard_tests PRIVATE ${inc_headers})
add_executable(indirect_draw_args_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/IndirectDrawArgsTests.cpp"
)
target_include_directories(indirect_draw_args_tests PRIVATE ${inc_headers})

add_executable(indirect_draw_policy_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/IndirectDrawPolicyTests.cpp"
)
target_include_directories(indirect_draw_policy_tests PRIVATE ${inc_headers})

add_executable(readback_plan_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ReadbackPlanTests.cpp"
)
target_include_directories(readback_plan_tests PRIVATE ${inc_headers})

add_executable(poll_wait_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/PollWaitTests.cpp"
)
target_include_directories(poll_wait_tests PRIVATE ${inc_headers})

add_executable(hot_pages_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/HotPagesTests.cpp"
)
target_include_directories(hot_pages_tests PRIVATE ${inc_headers})

add_executable(readback_future_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ReadbackFutureTests.cpp"
)
target_include_directories(readback_future_tests PRIVATE ${inc_headers})

add_executable(loop_guard_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/LoopGuardTests.cpp"
)
target_include_directories(loop_guard_tests PRIVATE ${inc_headers})

add_executable(gpu_op_ring_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/GpuOpRingTests.cpp"
)
target_include_directories(gpu_op_ring_tests PRIVATE ${inc_headers})

add_executable(gc_clock_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/GcClockTests.cpp"
)
target_include_directories(gc_clock_tests PRIVATE ${inc_headers})

add_executable(pipeline_cache_file_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/PipelineCacheFileTests.cpp"
)
target_link_libraries(pipeline_cache_file_tests xxhash fmt::fmt)
target_include_directories(pipeline_cache_file_tests PRIVATE ${inc_headers})

add_executable(skipped_draw_log_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/SkippedDrawLogTests.cpp"
)
target_include_directories(skipped_draw_log_tests PRIVATE ${inc_headers})

add_executable(skip_dependencies_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/SkipDependenciesTests.cpp"
)
target_include_directories(skip_dependencies_tests PRIVATE ${inc_headers})

add_executable(config_defaults_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ConfigDefaultsTests.cpp"
)
target_link_libraries(config_defaults_tests common)
target_include_directories(config_defaults_tests PRIVATE ${inc_headers})

# Host memory advice of the launcher (pure decision functions and the Linux probe; no Qt).
add_executable(host_memory_policy_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/HostMemoryPolicyTests.cpp"
	"${KYTY_SOURCE_DIR}/launcher/src/hostMemoryPolicy.cpp"
	"${KYTY_SOURCE_DIR}/launcher/src/hostMemoryProbe.cpp"
	"${KYTY_SOURCE_DIR}/launcher/src/hostMemoryTrust.cpp"
)
target_include_directories(host_memory_policy_tests PRIVATE "${KYTY_SOURCE_DIR}/launcher/include")

add_executable(buffer_gc_gate_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/BufferGcGateTests.cpp"
)
target_include_directories(buffer_gc_gate_tests PRIVATE ${inc_headers})

add_executable(bda_sync_gate_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/BdaSyncGateTests.cpp"
)
target_include_directories(bda_sync_gate_tests PRIVATE ${inc_headers})
find_package(Threads REQUIRED)
target_link_libraries(bda_sync_gate_tests Threads::Threads)

add_executable(page_presence_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/PagePresenceTests.cpp"
)
target_include_directories(page_presence_tests PRIVATE ${inc_headers})
target_link_libraries(page_presence_tests Threads::Threads)

add_executable(hardware_context_epoch_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/HardwareContextEpochTests.cpp"
)
target_include_directories(hardware_context_epoch_tests PRIVATE ${inc_headers})

add_executable(push_descriptor_cache_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/PushDescriptorCacheTests.cpp"
)
target_include_directories(push_descriptor_cache_tests PRIVATE ${inc_headers})

add_executable(small_copy_dedup_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/SmallCopyDedupTests.cpp"
)
target_include_directories(small_copy_dedup_tests PRIVATE ${inc_headers})

add_executable(adaptive_lock_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/AdaptiveLockTests.cpp"
)
target_include_directories(adaptive_lock_tests PRIVATE ${inc_headers})
target_link_libraries(adaptive_lock_tests Threads::Threads)

add_executable(sharded_counter_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ShardedCounterTests.cpp"
)
target_include_directories(sharded_counter_tests PRIVATE ${inc_headers})
target_link_libraries(sharded_counter_tests Threads::Threads)

add_executable(finished_prefix_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/FinishedPrefixTests.cpp"
)
target_include_directories(finished_prefix_tests PRIVATE ${inc_headers})
target_link_libraries(finished_prefix_tests Threads::Threads)

add_executable(scalable_shared_mutex_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ScalableSharedMutexTests.cpp"
)
target_include_directories(scalable_shared_mutex_tests PRIVATE ${inc_headers})
target_link_libraries(scalable_shared_mutex_tests Threads::Threads)

add_executable(remembered_mapping_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/RememberedMappingTests.cpp"
)
target_include_directories(remembered_mapping_tests PRIVATE ${inc_headers})

add_executable(driver_cache_checkpoint_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/DriverCacheCheckpointTests.cpp"
)
target_include_directories(driver_cache_checkpoint_tests PRIVATE ${inc_headers})
target_link_libraries(driver_cache_checkpoint_tests Threads::Threads)

add_executable(lds_span_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/LdsSpanTests.cpp"
)
target_include_directories(lds_span_tests PRIVATE ${inc_headers})
add_executable(hle_trace_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/HleTraceTests.cpp"
)
target_link_libraries(hle_trace_tests common)
target_include_directories(hle_trace_tests PRIVATE ${inc_headers})

add_executable(async_pipeline_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/AsyncPipelineTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/renderer/pipeline/asyncPipelineCompiler.cpp"
)
target_link_libraries(async_pipeline_tests fmt::fmt common)
target_include_directories(async_pipeline_tests PRIVATE ${inc_headers})
add_executable(speculation_isolation_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/SpeculationIsolationTests.cpp"
	"${KYTY_SOURCE_DIR}/common/speculation.cpp"
)
target_include_directories(speculation_isolation_tests PRIVATE ${inc_headers})
find_package(Threads REQUIRED)
target_link_libraries(speculation_isolation_tests Threads::Threads)
add_executable(guest_retirement_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/GuestRetirementTests.cpp"
)
target_include_directories(guest_retirement_tests PRIVATE ${inc_headers})
target_link_libraries(guest_retirement_tests Threads::Threads)
add_executable(completion_signal_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/CompletionSignalTests.cpp"
)
target_include_directories(completion_signal_tests PRIVATE ${inc_headers})
add_executable(speculation_item_watch_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/SpeculationItemWatchTests.cpp"
	"${KYTY_SOURCE_DIR}/common/speculation.cpp"
)
target_include_directories(speculation_item_watch_tests PRIVATE ${inc_headers})
target_link_libraries(speculation_item_watch_tests Threads::Threads)
add_executable(pipeline_variant_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/PipelineVariantTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/renderer/pipeline/variantStore.cpp"
)
target_include_directories(pipeline_variant_tests PRIVATE ${inc_headers})
add_executable(gpu_clock_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/GpuClockTests.cpp"
)
target_include_directories(gpu_clock_tests PRIVATE ${inc_headers})

add_executable(frame_pacer_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/FramePacerTests.cpp"
)
target_include_directories(frame_pacer_tests PRIVATE ${inc_headers})

add_executable(ime_dialog_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ImeDialogTests.cpp"
	"${KYTY_SOURCE_DIR}/libs/imeCommon.cpp"
	"${KYTY_SOURCE_DIR}/libs/imeDialog.cpp"
)
target_include_directories(ime_dialog_tests PRIVATE ${inc_headers})

add_executable(safe_call_flags_tests EXCLUDE_FROM_ALL "${KYTY_TESTS_DIR}/SafeCallFlagsTests.cpp")
target_link_libraries(safe_call_flags_tests PRIVATE common xbyak::xbyak)
target_include_directories(safe_call_flags_tests PRIVATE ${inc_headers})

add_executable(memory_tracker_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/MemoryTrackerTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/pageManager.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/memoryTracker.cpp"
)
target_link_libraries(memory_tracker_tests fmt::fmt common)
target_include_directories(memory_tracker_tests PRIVATE ${inc_headers})

add_executable(shader_vertex_metadata_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ShaderVertexMetadataTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/hostMemory.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/shaderVertexMetadata.cpp"
)
target_include_directories(shader_vertex_metadata_tests PRIVATE ${inc_headers})

add_executable(resource_materialization_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ResourceMaterializationTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/guest_gpu/gpu_format.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/hostMemory.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/ResourceMaterialization.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/SrtWalker.cpp"
)
target_link_libraries(resource_materialization_tests fmt::fmt xbyak::xbyak)
target_include_directories(resource_materialization_tests PRIVATE ${inc_headers})

add_executable(resource_tracking_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ResourceTrackingTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/guest_gpu/gpu_format.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/hostMemory.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/shaderPixelParameter.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/SrtWalker.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/ResourceTracking.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/ResourceMaterialization.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/ShaderInfoCollection.cpp"
)
target_link_libraries(resource_tracking_tests fmt::fmt xbyak::xbyak)
target_include_directories(resource_tracking_tests PRIVATE ${inc_headers})

add_executable(audio_out2_port_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/AudioOut2PortTests.cpp"
	"${KYTY_SOURCE_DIR}/libs/libAudio2.cpp"
	"${KYTY_SOURCE_DIR}/loader/timer.cpp"
)
target_link_libraries(audio_out2_port_tests common fmt::fmt)
target_include_directories(audio_out2_port_tests PRIVATE ${inc_headers})

add_executable(audio_out_timing_tests EXCLUDE_FROM_ALL "${KYTY_TESTS_DIR}/AudioOutTimingTests.cpp")
target_link_libraries(audio_out_timing_tests common fmt::fmt SDL3::SDL3-static)
target_include_directories(audio_out_timing_tests PRIVATE ${inc_headers})

add_executable(ngs2_sampler_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/Ngs2SamplerTests.cpp"
	"${KYTY_SOURCE_DIR}/loader/timer.cpp"
)
target_link_libraries(ngs2_sampler_tests common fmt::fmt LibAtrac9)
target_include_directories(ngs2_sampler_tests PRIVATE ${inc_headers})

add_executable(pad_haptics_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/PadHapticsTests.cpp"
)
target_link_libraries(pad_haptics_tests common fmt::fmt SDL3::SDL3-static opus)
target_include_directories(pad_haptics_tests PRIVATE ${inc_headers})

add_executable(controller_settings_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ControllerSettingsTests.cpp"
)
target_link_libraries(controller_settings_tests common fmt::fmt SDL3::SDL3-static)
target_include_directories(controller_settings_tests PRIVATE ${inc_headers})

add_executable(save_data_memory_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/SaveDataMemoryTests.cpp"
)
target_link_libraries(save_data_memory_tests common fmt::fmt)
target_include_directories(save_data_memory_tests PRIVATE ${inc_headers})

add_executable(png_enc_tests EXCLUDE_FROM_ALL "${KYTY_TESTS_DIR}/PngEncTests.cpp")
target_link_libraries(png_enc_tests common fmt::fmt)
target_include_directories(png_enc_tests PRIVATE ${inc_headers})

add_executable(ces_tests EXCLUDE_FROM_ALL "${KYTY_TESTS_DIR}/CesTests.cpp")
target_link_libraries(ces_tests common fmt::fmt SDL3::SDL3-static)
target_include_directories(ces_tests PRIVATE ${inc_headers})

add_executable(save_data_dialog_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/SaveDataDialogTests.cpp"
	"${KYTY_SOURCE_DIR}/libs/dialog.cpp"
)
target_link_libraries(save_data_dialog_tests common fmt::fmt)
target_include_directories(save_data_dialog_tests PRIVATE ${inc_headers})

add_kyty_full_emulator_test(http_uri_parse_tests
	"${KYTY_TESTS_DIR}/HttpUriParseTests.cpp")

add_executable(event_queue_lifetime_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/EventQueueLifetimeTests.cpp"
	"${KYTY_SOURCE_DIR}/kernel/eventQueue.cpp"
	"${KYTY_SOURCE_DIR}/loader/timer.cpp"
)
target_link_libraries(event_queue_lifetime_tests common fmt::fmt)
target_include_directories(event_queue_lifetime_tests PRIVATE ${inc_headers})

add_kyty_full_emulator_test(kernel_file_system_tests
	"${KYTY_TESTS_DIR}/KernelFileSystemTests.cpp")
target_link_libraries(kernel_file_system_tests ZArchive::zarchive)

add_executable(sync_on_address_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/SyncOnAddressTests.cpp"
	"${KYTY_SOURCE_DIR}/kernel/syncOnAddress.cpp"
)
target_link_libraries(sync_on_address_tests common)
target_include_directories(sync_on_address_tests PRIVATE ${inc_headers})

add_executable(image_page_table_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ImagePageTableTests.cpp"
)
target_link_libraries(image_page_table_tests common fmt::fmt)
target_include_directories(image_page_table_tests PRIVATE ${inc_headers})

add_kyty_full_emulator_test(shader_recompiler_compute_tests "${KYTY_TESTS_DIR}/ShaderRecompilerComputeTests.cpp")

set(gpu_test_generated_dir "${PROJECT_BINARY_DIR}/gpu_test_shaders")
set(gpu_test_ms_depth_source
	"${gpu_tiler_shader_dir}/gpu_test_ms_depth.comp")
set(gpu_test_ms_depth_spv
	"${gpu_test_generated_dir}/gpu_test_ms_depth.spv")
set(gpu_test_ms_depth_header
	"${gpu_test_generated_dir}/gpu_test_ms_depth_spv.h")
add_custom_command(
	OUTPUT "${gpu_test_ms_depth_header}"
	COMMAND ${CMAKE_COMMAND} -E make_directory "${gpu_test_generated_dir}"
	COMMAND "${KYTY_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 -Os
	        -o "${gpu_test_ms_depth_spv}" "${gpu_test_ms_depth_source}"
	COMMAND ${CMAKE_COMMAND} -DINPUT=${gpu_test_ms_depth_spv}
	        -DOUTPUT=${gpu_test_ms_depth_header} -DSYMBOL=GPU_TEST_MS_DEPTH_SPV
	        -P "${KYTY_SOURCE_DIR}/embed_spirv.cmake"
	DEPENDS "${gpu_test_ms_depth_source}"
	VERBATIM
)
target_sources(shader_recompiler_compute_tests PRIVATE
	"${gpu_test_ms_depth_header}")

add_kyty_full_emulator_test(virtual_memory_allocation_tests "${KYTY_TESTS_DIR}/VirtualMemoryAllocationTests.cpp")
target_compile_definitions(virtual_memory_allocation_tests PRIVATE
	KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS=1)

# These tests use exceptions.
if(NOT KYTY_CLANG_CL)
	foreach(kyty_exception_test scalar_provenance_tests resource_tracking_tests
			virtual_memory_allocation_tests)
		target_compile_options(${kyty_exception_test} PRIVATE -fexceptions)
	endforeach()
endif()

if(BUILD_TESTING)
	add_test(NAME emulator_user_name_cli
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --user-name 1234567890123456)
	add_test(NAME emulator_user_name_too_long
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --user-name 12345678901234567)
	set_tests_properties(emulator_user_name_too_long PROPERTIES WILL_FAIL TRUE)
	add_test(NAME emulator_present_mode_cli
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --present-mode Immediate)
	add_test(NAME emulator_present_mode_invalid
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --present-mode Invalid)
	set_tests_properties(emulator_present_mode_invalid PROPERTIES WILL_FAIL TRUE)
	add_test(NAME emulator_skip_ir_validation_cli
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --skip-ir-validation true)
	add_test(NAME emulator_skip_ir_validation_invalid
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --skip-ir-validation maybe)
	set_tests_properties(emulator_skip_ir_validation_invalid PROPERTIES WILL_FAIL TRUE)
	add_test(NAME emulator_spirv_robust_bounds_cli
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --spirv-robust-bounds true)
	add_test(NAME emulator_spirv_robust_bounds_invalid
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --spirv-robust-bounds maybe)
	set_tests_properties(emulator_spirv_robust_bounds_invalid PROPERTIES WILL_FAIL TRUE)
	add_test(NAME emulator_unoptimized_first_cli
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --pipelines-unoptimized-first true)
	add_test(NAME emulator_unoptimized_first_invalid
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --pipelines-unoptimized-first maybe)
	set_tests_properties(emulator_unoptimized_first_invalid PROPERTIES WILL_FAIL TRUE)
	add_test(NAME emulator_gpu_timestamps_legacy_cli
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --gpu-timestamps legacy)
	add_test(NAME emulator_gpu_timestamps_invalid
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --gpu-timestamps maybe)
	set_tests_properties(emulator_gpu_timestamps_invalid PROPERTIES WILL_FAIL TRUE)
	add_test(NAME emulator_gc_buffer_own_share_set_cli
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --gc-buffer-own-share 25)
	add_test(NAME emulator_gc_buffer_own_share_invalid
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --gc-buffer-own-share 101)
	set_tests_properties(emulator_gc_buffer_own_share_invalid PROPERTIES WILL_FAIL TRUE)
	add_test(NAME emulator_readback_linear_images_on_cli
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --readback-linear-images true)
	add_test(NAME emulator_pipeline_cache_key_revision_cli
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --pipeline-cache-key revision)
	add_test(NAME emulator_pipeline_cache_key_invalid
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --pipeline-cache-key stale)
	set_tests_properties(emulator_pipeline_cache_key_invalid PROPERTIES WILL_FAIL TRUE)
	add_test(NAME emulator_vram_paging_off_cli
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --vram-paging off)
	add_test(NAME emulator_vram_paging_invalid
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --vram-paging maybe)
	set_tests_properties(emulator_vram_paging_invalid PROPERTIES WILL_FAIL TRUE)
	add_test(NAME ime_dialog COMMAND $<TARGET_FILE:ime_dialog_tests>)
	add_test(NAME shader_cfg COMMAND $<TARGET_FILE:shader_cfg_tests>)
	add_test(NAME dynamic_state COMMAND $<TARGET_FILE:dynamic_state_tests>)
	add_test(NAME slot_vector COMMAND $<TARGET_FILE:slot_vector_tests>)
	add_test(NAME scalar_provenance COMMAND $<TARGET_FILE:scalar_provenance_tests>)
	add_test(NAME image_page_table COMMAND $<TARGET_FILE:image_page_table_tests>)
	add_test(NAME memory_tracker COMMAND $<TARGET_FILE:memory_tracker_tests>)
	add_test(NAME safe_call_flags COMMAND $<TARGET_FILE:safe_call_flags_tests>)
	set_tests_properties(safe_call_flags PROPERTIES TIMEOUT 10 RUN_SERIAL TRUE)
	add_test(NAME page_manager COMMAND $<TARGET_FILE:page_manager_tests>)
	add_test(NAME mem_residency_ranges COMMAND $<TARGET_FILE:mem_residency_ranges_tests>)
	add_test(NAME archive_file COMMAND $<TARGET_FILE:archive_file_tests>)
	add_test(NAME trophy_system COMMAND $<TARGET_FILE:trophy_system_tests>)
	add_test(NAME avplayer_file COMMAND $<TARGET_FILE:avplayer_file_tests>)
	add_test(NAME bit_array COMMAND $<TARGET_FILE:bit_array_tests>)
	add_test(NAME atomic_bit_mirror COMMAND $<TARGET_FILE:atomic_bit_mirror_tests>)
	add_test(NAME mesh_dispatch COMMAND $<TARGET_FILE:mesh_dispatch_tests>)
	add_test(NAME null_page_read COMMAND $<TARGET_FILE:null_page_read_tests>)
	add_test(NAME driver_cache_checkpoint COMMAND $<TARGET_FILE:driver_cache_checkpoint_tests>)
	add_test(NAME compare_view COMMAND $<TARGET_FILE:compare_view_tests>)
	add_test(NAME swapchain_extent COMMAND $<TARGET_FILE:swapchain_extent_tests>)
	add_test(NAME playgo_api COMMAND $<TARGET_FILE:playgo_api_tests>)
	add_test(NAME gds_copy_plan COMMAND $<TARGET_FILE:gds_copy_plan_tests>)
	add_test(NAME lru_cache COMMAND $<TARGET_FILE:lru_cache_tests>)
	add_test(NAME vram_policy COMMAND $<TARGET_FILE:vram_policy_tests>)
	add_test(NAME heap_trim COMMAND $<TARGET_FILE:heap_trim_tests>)
	add_test(NAME lds_span COMMAND $<TARGET_FILE:lds_span_tests>)
	add_test(NAME texture_gc_policy COMMAND $<TARGET_FILE:texture_gc_policy_tests>)
	add_test(NAME phase_timers COMMAND $<TARGET_FILE:phase_timers_tests>)
	add_test(NAME runtime_toggles COMMAND $<TARGET_FILE:runtime_toggles_tests>)
	add_test(NAME submit_coalescer COMMAND $<TARGET_FILE:submit_coalescer_tests>)
	add_test(NAME drain_causes COMMAND $<TARGET_FILE:drain_causes_tests>)
	add_test(NAME barrier_elision COMMAND $<TARGET_FILE:barrier_elision_tests>)
	add_test(NAME pending_writes COMMAND $<TARGET_FILE:pending_writes_tests>)
	add_test(NAME dispatch_hazard COMMAND $<TARGET_FILE:dispatch_hazard_tests>)
	add_test(NAME known_fills COMMAND $<TARGET_FILE:known_fills_tests>)
	add_test(NAME known_stores COMMAND $<TARGET_FILE:known_stores_tests>)
	add_test(NAME known_copies COMMAND $<TARGET_FILE:known_copies_tests>)
	add_test(NAME draw_key_memo COMMAND $<TARGET_FILE:draw_key_memo_tests>)
	add_test(NAME image_lookup_memo COMMAND $<TARGET_FILE:image_lookup_memo_tests>)
	add_test(NAME srt_plan COMMAND $<TARGET_FILE:srt_plan_tests>)
	add_test(NAME lookahead_governor COMMAND $<TARGET_FILE:lookahead_governor_tests>)
	add_test(NAME emulator_srt_plan_shadow_cli
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --srt-plan-shadow true)
	add_test(NAME emulator_srt_plan_shadow_invalid
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --srt-plan-shadow maybe)
	set_tests_properties(emulator_srt_plan_shadow_invalid PROPERTIES WILL_FAIL TRUE)
	add_test(NAME emulator_srt_plans_cli
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --srt-plans true)
	add_test(NAME emulator_lookahead_workers_cli
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --lookahead-workers 4)
	add_test(NAME emulator_lookahead_workers_invalid
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --lookahead-workers 99)
	set_tests_properties(emulator_lookahead_workers_invalid PROPERTIES WILL_FAIL TRUE)
	add_test(NAME emulator_resolved_draw_cli
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --resolved-draw true --resolved-draw-verify true --mapping-memo true)
	add_test(NAME emulator_resolved_draw_invalid
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --resolved-draw maybe)
	set_tests_properties(emulator_resolved_draw_invalid PROPERTIES WILL_FAIL TRUE)
	add_test(NAME emulator_srt_plans_invalid
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --srt-plans maybe)
	set_tests_properties(emulator_srt_plans_invalid PROPERTIES WILL_FAIL TRUE)
	add_test(NAME emulator_skip_guard_cli
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --skip-guard true --skip-guard-64bit false)
	add_test(NAME emulator_skip_guard_invalid
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --skip-guard maybe)
	set_tests_properties(emulator_skip_guard_invalid PROPERTIES WILL_FAIL TRUE)
	add_test(NAME scalar_read_hazard COMMAND $<TARGET_FILE:scalar_read_hazard_tests>)
	add_test(NAME indirect_draw_args COMMAND $<TARGET_FILE:indirect_draw_args_tests>)
	add_test(NAME indirect_draw_policy COMMAND $<TARGET_FILE:indirect_draw_policy_tests>)
	add_test(NAME readback_plan COMMAND $<TARGET_FILE:readback_plan_tests>)
	add_test(NAME hot_pages COMMAND $<TARGET_FILE:hot_pages_tests>)
	add_test(NAME poll_wait COMMAND $<TARGET_FILE:poll_wait_tests>)
	add_test(NAME readback_future COMMAND $<TARGET_FILE:readback_future_tests>)
	add_test(NAME gc_clock COMMAND $<TARGET_FILE:gc_clock_tests>)
	add_test(NAME gpu_op_ring COMMAND $<TARGET_FILE:gpu_op_ring_tests>)
	add_test(NAME loop_guard COMMAND $<TARGET_FILE:loop_guard_tests>)
	add_test(NAME buffer_gc_gate COMMAND $<TARGET_FILE:buffer_gc_gate_tests>)
	add_test(NAME config_defaults COMMAND $<TARGET_FILE:config_defaults_tests>)
	add_test(NAME host_memory_policy COMMAND $<TARGET_FILE:host_memory_policy_tests>)
	if(UNIX)
		# The root helper of the "Host memory" window, run against a fake root (no root, no system change).
		add_test(NAME host_memory_helper
		         COMMAND bash "${KYTY_TESTS_DIR}/host_memory_helper_test.sh"
		                 "${KYTY_SOURCE_DIR}/launcher/host/kyty-host-memory"
		                 "${KYTY_SOURCE_DIR}/launcher/include/hostMemoryPolicy.h"
		                 "${KYTY_SOURCE_DIR}/launcher/include/hostMemoryProbe.h")
	endif()
	add_test(NAME skipped_draw_log COMMAND $<TARGET_FILE:skipped_draw_log_tests>)
	add_test(NAME skip_dependencies COMMAND $<TARGET_FILE:skip_dependencies_tests>)
	add_test(NAME pipeline_cache_file COMMAND $<TARGET_FILE:pipeline_cache_file_tests>)
	add_test(NAME bda_sync_gate COMMAND $<TARGET_FILE:bda_sync_gate_tests>)
	add_test(NAME remembered_mapping COMMAND $<TARGET_FILE:remembered_mapping_tests>)
	add_test(NAME page_presence COMMAND $<TARGET_FILE:page_presence_tests>)
	add_test(NAME adaptive_lock COMMAND $<TARGET_FILE:adaptive_lock_tests>)
	add_test(NAME sharded_counter COMMAND $<TARGET_FILE:sharded_counter_tests>)
	add_test(NAME scalable_shared_mutex COMMAND $<TARGET_FILE:scalable_shared_mutex_tests>)
	add_test(NAME finished_prefix COMMAND $<TARGET_FILE:finished_prefix_tests>)
	add_test(NAME push_descriptor_cache COMMAND $<TARGET_FILE:push_descriptor_cache_tests>)
	add_test(NAME small_copy_dedup COMMAND $<TARGET_FILE:small_copy_dedup_tests>)
	add_test(NAME hardware_context_epoch COMMAND $<TARGET_FILE:hardware_context_epoch_tests>)
	add_test(NAME sys_file_read COMMAND $<TARGET_FILE:sys_file_read_tests>)
	add_test(NAME sys_file_read_fallback_disabled COMMAND $<TARGET_FILE:sys_file_read_tests>)
	set_tests_properties(sys_file_read_fallback_disabled PROPERTIES
		ENVIRONMENT "KYTY_SYSFILE_NO_READ_FALLBACK=1")
	add_test(NAME hle_trace COMMAND $<TARGET_FILE:hle_trace_tests>)
	add_test(NAME sys_file_read_chaos COMMAND $<TARGET_FILE:sys_file_read_tests>)
	set_tests_properties(sys_file_read_chaos PROPERTIES
		ENVIRONMENT "KYTY_SYSFILE_CHAOS_EFAULT=1000")
	add_test(NAME sys_file_read_chaos_no_fallback COMMAND $<TARGET_FILE:sys_file_read_tests>)
	set_tests_properties(sys_file_read_chaos_no_fallback PROPERTIES
		ENVIRONMENT "KYTY_SYSFILE_CHAOS_EFAULT=1000;KYTY_SYSFILE_NO_READ_FALLBACK=1")
	add_test(NAME async_pipeline COMMAND $<TARGET_FILE:async_pipeline_tests>)
	add_test(NAME pipeline_variants COMMAND $<TARGET_FILE:pipeline_variant_tests>)
	add_test(NAME speculation_isolation COMMAND $<TARGET_FILE:speculation_isolation_tests>)
	add_test(NAME guest_retirement COMMAND $<TARGET_FILE:guest_retirement_tests>)
	add_test(NAME completion_signal COMMAND $<TARGET_FILE:completion_signal_tests>)
	add_test(NAME speculation_item_watch COMMAND $<TARGET_FILE:speculation_item_watch_tests>)
	add_test(NAME gpu_clock COMMAND $<TARGET_FILE:gpu_clock_tests>)
	add_test(NAME frame_pacer COMMAND $<TARGET_FILE:frame_pacer_tests>)
	add_test(NAME shader_vertex_metadata COMMAND $<TARGET_FILE:shader_vertex_metadata_tests>)
	add_test(NAME resource_materialization COMMAND $<TARGET_FILE:resource_materialization_tests>)
	add_test(NAME resource_tracking COMMAND $<TARGET_FILE:resource_tracking_tests>)
	add_test(NAME event_queue_lifetime COMMAND $<TARGET_FILE:event_queue_lifetime_tests>)
	add_test(NAME kernel_file_system COMMAND $<TARGET_FILE:kernel_file_system_tests>)
	add_test(NAME sync_on_address COMMAND $<TARGET_FILE:sync_on_address_tests>)
	add_test(NAME audio_out2_port COMMAND $<TARGET_FILE:audio_out2_port_tests>)
	add_test(NAME audio_out_timing COMMAND $<TARGET_FILE:audio_out_timing_tests>)
	add_test(NAME ngs2_sampler COMMAND $<TARGET_FILE:ngs2_sampler_tests>)
	add_test(NAME pad_haptics COMMAND $<TARGET_FILE:pad_haptics_tests>)
	add_test(NAME controller_settings COMMAND $<TARGET_FILE:controller_settings_tests>)
	add_test(NAME save_data_memory COMMAND $<TARGET_FILE:save_data_memory_tests>)
	add_test(NAME png_enc COMMAND $<TARGET_FILE:png_enc_tests>)
	add_test(NAME ces COMMAND $<TARGET_FILE:ces_tests>)
	add_test(NAME save_data_dialog COMMAND $<TARGET_FILE:save_data_dialog_tests>)
	add_test(NAME http_uri_parse COMMAND $<TARGET_FILE:http_uri_parse_tests>)
	add_test(NAME shader_recompiler_bda_fault_readback
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --bda-fault-readback-only)
	add_test(NAME shader_recompiler_bda_fault_bitmap
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --bda-fault-bitmap-only)
	add_test(NAME shader_recompiler_compute COMMAND $<TARGET_FILE:shader_recompiler_compute_tests>)
	add_test(NAME guest_gpu_timestamps
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --gpu-timestamps-only)
	add_test(NAME shader_recompiler_helper_quad
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --ds-append-helper-quad-only)
	# --swizzle-helper-quad-only (the GPU draw) is not registered: on NVIDIA v_mbcnt of the
	# helper-free exec ballot numbers the covered lanes by rank, the test assumes physical lane
	# numbers (AMD), so its expectation fails there for a reason unrelated to the helper exclusion.
	add_test(NAME shader_recompiler_helper_host
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --fragment-helper-host-only)
	add_test(NAME shader_recompiler_alignbyte
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --alignbyte-only)
	add_test(NAME graphics_pipeline_rasterization
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --polygon-mode-only)
	# --async-pipelines through the real PipelineCache: the whole suite with every program and
	# pipeline built on a worker (stall), and the graphics pipeline check with draws that retry
	# until the worker has published the pipeline (skip; compute dispatches wait either way).
	add_test(NAME shader_recompiler_compute_async_stall
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests>)
	set_tests_properties(shader_recompiler_compute_async_stall PROPERTIES
		ENVIRONMENT "KYTY_TEST_ASYNC_PIPELINES=stall")
	add_test(NAME graphics_pipeline_rasterization_async_skip
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --polygon-mode-only)
	set_tests_properties(graphics_pipeline_rasterization_async_skip PROPERTIES
		ENVIRONMENT "KYTY_TEST_ASYNC_PIPELINES=skip")
	# The lookahead's prefetch must derive the same pipeline key as the draw (stall mode, the
	# pipeline is requested through PrefetchGraphicsPipeline before the draw asks for it).
	add_test(NAME graphics_pipeline_prefetch_key
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --polygon-mode-only)
	set_tests_properties(graphics_pipeline_prefetch_key PROPERTIES
		ENVIRONMENT "KYTY_TEST_ASYNC_PIPELINES=stall;KYTY_TEST_PREFETCH_KEY=1")
	# --pipelines-unoptimized-first: the whole suite on unoptimized pipelines that are swapped for
	# their optimized builds while it runs, and the promotion check itself.
	add_test(NAME shader_recompiler_compute_unoptimized_first
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests>)
	set_tests_properties(shader_recompiler_compute_unoptimized_first PROPERTIES
		ENVIRONMENT "KYTY_TEST_UNOPTIMIZED_FIRST=1")
	add_test(NAME graphics_pipeline_rasterization_unoptimized_first
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --polygon-mode-only)
	set_tests_properties(graphics_pipeline_rasterization_unoptimized_first PROPERTIES
		ENVIRONMENT "KYTY_TEST_UNOPTIMIZED_FIRST=1")
	add_test(NAME pipeline_unoptimized_first_promotion
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --unoptimized-first-only)
	set_tests_properties(pipeline_unoptimized_first_promotion PROPERTIES
		ENVIRONMENT "KYTY_TEST_UNOPTIMIZED_FIRST=1")
	# --spirv-robust-bounds: the whole suite with storage-buffer bounds left to robustBufferAccess2
	# (device created with it; plain loads and stores carry no explicit check), directly compiled
	# and through the async worker pool.
	add_test(NAME shader_recompiler_compute_robust_bounds
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests>)
	set_tests_properties(shader_recompiler_compute_robust_bounds PROPERTIES
		ENVIRONMENT "KYTY_TEST_ROBUST_BOUNDS=1")
	add_test(NAME shader_recompiler_compute_robust_bounds_async_stall
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests>)
	set_tests_properties(shader_recompiler_compute_robust_bounds_async_stall PROPERTIES
		ENVIRONMENT "KYTY_TEST_ROBUST_BOUNDS=1;KYTY_TEST_ASYNC_PIPELINES=stall")
	# --precompile-on-load / --pipeline-variants-replay: the whole suite with variant tracking on
	# (every first dispatch of a program), and the registration -> build -> hit flow itself.
	add_test(NAME shader_recompiler_compute_shader_load
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests>)
	set_tests_properties(shader_recompiler_compute_shader_load PROPERTIES
		ENVIRONMENT "KYTY_TEST_SHADER_LOAD=1")
	# ... and the graphics half: the pipelines of the pixel-wave test are recorded by one cache and
	# rebuilt by a second one from the registrations.
	add_test(NAME graphics_pipeline_shader_load
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --polygon-mode-only)
	set_tests_properties(graphics_pipeline_shader_load PROPERTIES
		ENVIRONMENT "KYTY_TEST_SHADER_LOAD=1")
	add_test(NAME shader_load_precompile
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --shader-load-only)
	set_tests_properties(shader_load_precompile PROPERTIES
		ENVIRONMENT "KYTY_TEST_SHADER_LOAD=1")
	add_test(NAME graphics_draw_offsets
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --draw-offset-only)
	add_test(NAME guest_stack_exit
		COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --guest-stack-exit-only)
	add_test(NAME virtual_memory_allocation
		COMMAND $<TARGET_FILE:virtual_memory_allocation_tests>)
	add_test(NAME guest_red_zone_patcher
		COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --red-zone-patcher-only)
	add_test(NAME command_scheduler_timeline
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --scheduler-only)
	add_test(NAME stream_buffer_ring
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --stream-buffer-only)
	add_test(NAME gpu_command_lane
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --gpu-command-lane-only)
	add_test(NAME lookahead_shadow_processor
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --shadow-only)
	add_test(NAME pm4_context_state
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --context-state-only)
	add_test(NAME gpu_tiler
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --gpu-tiler-only)
	add_test(NAME image_copy_kernels
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --image-copy-only)
	add_test(NAME image_transfer_layout
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --image-layout-only)

	add_test(NAME texture_cache_layered_image
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --layered-image-only)
	add_test(NAME host_image_allocation
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --host-image-allocation-only)
	add_test(NAME texture_cache_image_views
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --image-view-cache-only)
	add_test(NAME texture_cache_storage_sampled
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --storage-sampled-only)
	add_test(NAME texture_cache_depth_readback
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --depth-readback-only)
	add_test(NAME compute_meta_clear_classification
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --compute-meta-clear-only)
	add_test(NAME buffer_cache_dirty_gc
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --buffer-cache-gc-only)
	add_test(NAME buffer_cache_known_stores
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --known-stores-only)
	add_test(NAME buffer_cache_eager_readback
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --buffer-cache-eager-only)
	add_test(NAME buffer_cache_readback_future
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --buffer-cache-future-only)
	add_test(NAME buffer_cache_readback_future_stress
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --buffer-cache-future-stress-only)
	add_test(NAME bda_page_table_initialization
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --bda-page-table-init-only)
	add_test(NAME null_texture_descriptors
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --null-descriptors-only)
	add_test(NAME color_render_target_1d
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --rt1d-only)

	add_test(NAME texture_cache_image_overlap
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --image-overlap-only)
	add_test(NAME texture_cache_htile_clear
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --htile-clear-only)
	add_test(NAME buffer_cache_ranges
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --buffer-cache-range-only)

	add_custom_target(kyty_tests DEPENDS
		ime_dialog_tests
		shader_cfg_tests
		dynamic_state_tests
		slot_vector_tests
		scalar_provenance_tests
		image_page_table_tests
		memory_tracker_tests
		safe_call_flags_tests
		page_manager_tests
		mem_residency_ranges_tests
		archive_file_tests
		trophy_system_tests
		avplayer_file_tests
		bit_array_tests
		atomic_bit_mirror_tests
		mesh_dispatch_tests
		null_page_read_tests
		driver_cache_checkpoint_tests
		compare_view_tests
		swapchain_extent_tests
		playgo_api_tests
		gds_copy_plan_tests
		lru_cache_tests
		vram_policy_tests
		heap_trim_tests
		lds_span_tests
		texture_gc_policy_tests
		phase_timers_tests
		runtime_toggles_tests
		submit_coalescer_tests
		drain_causes_tests
		barrier_elision_tests
		pending_writes_tests
		dispatch_hazard_tests
		known_fills_tests
		known_stores_tests
		known_copies_tests
		draw_key_memo_tests
		image_lookup_memo_tests
		srt_plan_tests
		lookahead_governor_tests
		scalar_read_hazard_tests
		indirect_draw_args_tests
		indirect_draw_policy_tests
		readback_plan_tests
		hot_pages_tests
		poll_wait_tests
		readback_future_tests
		gc_clock_tests
		gpu_op_ring_tests
		loop_guard_tests
		buffer_gc_gate_tests
		config_defaults_tests
		host_memory_policy_tests
		skipped_draw_log_tests
		skip_dependencies_tests
		pipeline_cache_file_tests
		bda_sync_gate_tests
		remembered_mapping_tests
		page_presence_tests
		adaptive_lock_tests
		sharded_counter_tests
		scalable_shared_mutex_tests
		finished_prefix_tests
		push_descriptor_cache_tests
		small_copy_dedup_tests
		hardware_context_epoch_tests
		sys_file_read_tests
		hle_trace_tests
		async_pipeline_tests
		pipeline_variant_tests
		speculation_isolation_tests
		guest_retirement_tests
		completion_signal_tests
		speculation_item_watch_tests
		gpu_clock_tests
		frame_pacer_tests
		shader_vertex_metadata_tests
		resource_materialization_tests
		resource_tracking_tests
		event_queue_lifetime_tests
		kernel_file_system_tests
		sync_on_address_tests
		shader_recompiler_compute_tests
		virtual_memory_allocation_tests
		audio_out2_port_tests
		audio_out_timing_tests
		ngs2_sampler_tests
		pad_haptics_tests
		controller_settings_tests
		save_data_memory_tests
		png_enc_tests
		ces_tests
		save_data_dialog_tests
		http_uri_parse_tests
	)
endif()

# Test Darwin signal-context restoration for SSE4a instructions.
if(APPLE AND (CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64" OR
		(NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")))
	add_executable(macos_sse4a_tests EXCLUDE_FROM_ALL
		"${KYTY_TESTS_DIR}/macosSse4aTests.cpp"
		"${KYTY_SOURCE_DIR}/loader/x64InstructionEmulator.cpp"
		"${KYTY_SOURCE_DIR}/loader/guestInstructionPatcher.cpp"
	)
	target_link_libraries(macos_sse4a_tests common fmt::fmt Zydis::Zydis xbyak::xbyak)
	target_include_directories(macos_sse4a_tests PRIVATE ${inc_headers})
	if(BUILD_TESTING)
		add_test(NAME macos_sse4a COMMAND $<TARGET_FILE:macos_sse4a_tests>)
		set_tests_properties(macos_sse4a PROPERTIES TIMEOUT 15)
		add_dependencies(kyty_tests macos_sse4a_tests)
	endif()
endif()

# Diagnostic build: a sampling heap profiler that replaces malloc (src/tools/heapProfile.cpp) and keeps the
# symbol table. Never a release build; see the file header for the output.
option(KYTY_HEAP_PROFILE "Sampling heap profiler in kyty_emulator (Linux diagnostic build)" OFF)

add_executable(kyty_emulator "${KYTY_SOURCE_DIR}/main.cpp" ${kyty_emulator_src})
if(KYTY_HEAP_PROFILE AND LINUX)
	target_sources(kyty_emulator PRIVATE "${KYTY_SOURCE_DIR}/tools/heapProfile.cpp")
	target_link_options(kyty_emulator PRIVATE -Wl,--export-dynamic)
endif()
configure_macos_guest_address_space(kyty_emulator)

set(KYTY_TROPHY_UNLOCK_SOUND "${CMAKE_CURRENT_SOURCE_DIR}/assets/sounds/trophy-unlock.wav")
add_custom_target(kyty_trophy_sound
	COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:kyty_emulator>/assets/sounds"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KYTY_TROPHY_UNLOCK_SOUND}"
		"$<TARGET_FILE_DIR:kyty_emulator>/assets/sounds/trophy-unlock.wav"
	VERBATIM)
add_dependencies(kyty_emulator kyty_trophy_sound)
install(FILES "${KYTY_TROPHY_UNLOCK_SOUND}" DESTINATION assets/sounds)

target_link_libraries(kyty_emulator ${kyty_emulator_link_libraries})
if (WIN32)
	target_link_libraries(kyty_emulator opengl32)
	target_link_libraries(kyty_emulator iphlpapi)
	target_link_libraries(kyty_emulator setupapi)
	target_link_libraries(kyty_emulator ws2_32)
endif()
if (KYTY_CLANG_CL)
	target_link_libraries(kyty_emulator onecore)
endif()
if (CLANG AND NOT KYTY_CLANG_CL)
	target_link_libraries(kyty_emulator pthread)
endif()
# dlopen/dlsym/dladdr for RenderDoc.
target_link_libraries(kyty_emulator ${CMAKE_DL_LIBS})
target_include_directories(kyty_emulator PRIVATE ${inc_headers})

clang_tidy_check(kyty_emulator "" "${check_headers}" "${inc_headers}")

include_what_you_use(kyty_emulator "${inc_headers}")

set(KYTY_EMULATOR_MAP_NAME "kyty_emulator_${KYTY_COMPILER_ID}_${KYTY_LINKER_ID}.map")

set(KYTY_EMULATOR_MAP_LINK_PATH "${CMAKE_CURRENT_BINARY_DIR}/${KYTY_EMULATOR_MAP_NAME}")
set(KYTY_EMULATOR_PDB_LINK_PATH "${CMAKE_CURRENT_BINARY_DIR}/kyty_emulator.pdb")

if(KYTY_CLANG_CL)
	target_link_options(kyty_emulator PRIVATE
		"/DYNAMICBASE:NO"
		"/DEBUG:FULL"
		"/PDB:${KYTY_EMULATOR_PDB_LINK_PATH}"
		"/lldmap:${KYTY_EMULATOR_MAP_LINK_PATH}"
	)
	add_custom_command(TARGET kyty_emulator POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KYTY_THIRD_PARTY_DIR}/winpthread/bin/libwinpthread-1.dll" $<TARGET_FILE_DIR:kyty_emulator>/libwinpthread-1.dll)
elseif(WIN32 OR LINUX)
	set_target_properties(kyty_emulator PROPERTIES LINK_FLAGS "${KYTY_LD_OPTIONS}")
	# LINK_FLAGS is a space-delimited string: a -Map path containing spaces would
	# be split into bogus arguments, so pass it as a discrete link option instead.
	target_link_options(kyty_emulator PRIVATE "-Wl,-Map=${KYTY_EMULATOR_MAP_LINK_PATH}")
endif()

if(LINUX AND NOT KYTY_HEAP_PROFILE AND NOT KYTY_NO_STRIP)
    target_link_options(kyty_emulator PRIVATE
        $<$<CONFIG:Release>:-Wl,--strip-all>
    )
endif()

if(APPLE)
	# The emulator writes x86-64 trampolines/PLT stubs into memory and executes them, and
	# runs guest code under Rosetta. That requires the JIT / unsigned-executable-memory
	# entitlements, so re-sign the binary after every link (an unsigned/relinked binary
	# reverts to the hardened defaults and aborts when it first executes written code).
	add_custom_command(TARGET kyty_emulator POST_BUILD
		COMMAND codesign -s - --force --options runtime
			--entitlements "${KYTY_SOURCE_DIR}/macos_jit.entitlements"
			$<TARGET_FILE:kyty_emulator>
		COMMENT "Codesign kyty_emulator with JIT entitlements (macOS)")
endif()

install(TARGETS kyty_emulator DESTINATION .)
if(KYTY_BUILD_LAUNCHER)
	add_subdirectory("${KYTY_SOURCE_DIR}/launcher" "${CMAKE_CURRENT_BINARY_DIR}/launcher")

	if(LINUX)
		target_link_options(launcher PRIVATE $<$<CONFIG:Release>:-Wl,--strip-all>)
	endif()
endif()
if(KYTY_CLANG_CL)
	install(FILES "${KYTY_THIRD_PARTY_DIR}/winpthread/bin/libwinpthread-1.dll" DESTINATION .)
endif()

# Offline developer tools (not part of the emulator): driver shader-compile timing harness.
option(KYTY_BUILD_TOOLS "Build developer tools from tools/ (spirv-compile-bench)" OFF)
if(KYTY_BUILD_TOOLS AND LINUX)
	add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/tools/spirv-compile-bench" "${CMAKE_CURRENT_BINARY_DIR}/tools/spirv-compile-bench")
endif()
