# Optional release-performance switches (K54): target ISA level, floating-point contraction and
# ThinLTO. Everything here defaults to OFF/empty, so an unconfigured build is unchanged.
#
# Included from the top-level CMakeLists.txt after config_compiler_and_linker() and before the
# first add_subdirectory(), so the directory-scoped options below reach every target, including
# the third-party libraries that sit on the command processor's hot path.
#
# Options (cache variables, set with -D on the configure line):
#   KYTY_MARCH      ISA level passed as -march, for example x86-64-v3 (AVX2/BMI2/FMA, which every
#                   host able to run a PS5 title has). Empty (default) = the compiler's baseline.
#                   Use a level, never "native": a native build is not portable between hosts.
#   KYTY_LTO        ON = ThinLTO for non-Debug configurations (Clang, not Apple). Default OFF.
#   KYTY_LTO_JOBS   ThinLTO backend threads at link time (0 = all cores). Lower it on a machine
#                   short of RAM: the final link of the emulator is the memory peak.
#
# With KYTY_MARCH set, -ffp-contract=off is added too. A level with FMA makes Clang's default
# (-ffp-contract=on) fuse a*b+c, which changes float results bit for bit (viewport and scissor
# math that feeds floor/lround); off keeps the build numerically identical to the baseline one.
#
# Ported from Yugendren/Kestrel@5a17583cd (cmake/KytyOptimization.cmake, GPL-2.0); the same switches
# exist in chenxiao07/KytyPS5 (KYTY_NATIVE_OPT, KYTY_THIN_LTO) and Jetsku U59. Options are the
# same idea, defaults differ: they are all off here.
#
# Profile-guided optimisation is not wired in. It needs an instrumented training run of a game,
# and that profile is game-derived data, so it would stay local to bench/ and never go in git.

include_guard(GLOBAL)

set(KYTY_MARCH "" CACHE STRING "Host ISA level passed as -march, e.g. x86-64-v3 (empty = compiler baseline)")
option(KYTY_LTO "ThinLTO for non-Debug builds (Clang)" OFF)
set(KYTY_LTO_JOBS "0" CACHE STRING "ThinLTO backend threads at link time (0 = all cores)")

# clang-cl only forwards GCC-style options it does not know through /clang:.
function(kyty_add_driver_compile_option opt)
	if(KYTY_CLANG_CL)
		add_compile_options("/clang:${opt}")
	else()
		add_compile_options("${opt}")
	endif()
endfunction()

if(NOT KYTY_MARCH STREQUAL "")
	if(NOT (CLANG OR GCC))
		message(FATAL_ERROR "KYTY_MARCH needs Clang or GCC")
	endif()
	if(KYTY_MARCH STREQUAL "native")
		message(FATAL_ERROR "KYTY_MARCH=native is not portable; use an ISA level such as x86-64-v3")
	endif()
	kyty_add_driver_compile_option("-march=${KYTY_MARCH}")
	kyty_add_driver_compile_option("-ffp-contract=off")
endif()

if(KYTY_LTO)
	if(NOT CLANG OR APPLE)
		message(FATAL_ERROR "KYTY_LTO is ThinLTO and needs Clang (not on Apple)")
	endif()
	include(CheckIPOSupported)
	check_ipo_supported(RESULT KYTY_LTO_SUPPORTED OUTPUT KYTY_LTO_ERROR LANGUAGES C CXX)
	if(NOT KYTY_LTO_SUPPORTED)
		message(FATAL_ERROR "KYTY_LTO requested but not supported by this toolchain: ${KYTY_LTO_ERROR}")
	endif()
	# CMake's Clang IPO support is -flto=thin plus llvm-ar/llvm-ranlib for archives. Bundled
	# projects with an old cmake_minimum_required would otherwise ignore the property.
	set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
	set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
	set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
	set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
	if(KYTY_LINKER STREQUAL LLD)
		# Every executable (the tests too) must link with lld: the system ld cannot read bitcode.
		# The cache makes a relink after a one-file edit run the backend for that module only.
		add_link_options("-fuse-ld=lld" "LINKER:--thinlto-cache-dir=${CMAKE_BINARY_DIR}/thinlto-cache")
		if(NOT KYTY_LTO_JOBS STREQUAL "0")
			add_link_options("LINKER:--thinlto-jobs=${KYTY_LTO_JOBS}")
		endif()
	elseif(KYTY_LINKER STREQUAL LLD_LINK)
		add_link_options("/lldltocache:${CMAKE_BINARY_DIR}/thinlto-cache")
		if(NOT KYTY_LTO_JOBS STREQUAL "0")
			add_link_options("/opt:lldltojobs=${KYTY_LTO_JOBS}")
		endif()
	endif()
endif()

message(STATUS "Kyty optimisation: march='${KYTY_MARCH}' lto=${KYTY_LTO} lto_jobs=${KYTY_LTO_JOBS}")
