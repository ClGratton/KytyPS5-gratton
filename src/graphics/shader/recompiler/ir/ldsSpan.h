#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_IR_LDSSPAN_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_IR_LDSSPAN_H_

// Size of the per-invocation LDS array of non-compute shaders (see ShaderInfo::function_lds_dwords).
// Dependency-free so it can be unit-tested (tests/LdsSpanTests.cpp).

#include <algorithm>
#include <bit>
#include <cstdint>

namespace Libs::Graphics::ShaderRecompiler::IR {

// What a non-compute shader gets when its LDS use cannot be bounded: 32 KiB per invocation.
constexpr uint32_t FullLdsDwords = 8192;
// The smallest reduced array.
constexpr uint32_t MinReducedLdsDwords = 64;

// Dwords one LDS access can reach: its largest immediate byte offset plus the data it moves,
// plus one dword per lane of the wave (the dynamic base is a per-lane address).
[[nodiscard]] constexpr uint32_t LdsAccessSpanDwords(uint32_t offset_bytes,
                                                     uint32_t secondary_offset_bytes,
                                                     uint32_t data_dwords, uint32_t wave_size) {
	const uint32_t last_byte = std::max(offset_bytes, secondary_offset_bytes) +
	                           4u * std::max<uint32_t>(data_dwords, 1u);
	return (last_byte + 3u) / 4u + wave_size;
}

// Array size for a shader whose LDS accesses span `span_dwords`: the next power of two (so the
// index can wrap with a mask), at least MinReducedLdsDwords. 0 when that would not be smaller
// than the full array (or the span is unknown, 0): keep FullLdsDwords and exact bounds.
[[nodiscard]] constexpr uint32_t ReducedLdsDwords(uint32_t span_dwords) {
	if (span_dwords == 0) {
		return 0;
	}
	const uint32_t size = std::bit_ceil(std::max(span_dwords, MinReducedLdsDwords));
	return size < FullLdsDwords ? size : 0;
}

// Address inside a reduced array. Only distinct addresses of one lane matter, the array is
// private to the invocation, so aliasing modulo the size is harmless when the size covers them.
[[nodiscard]] constexpr uint32_t WrapLdsIndex(uint32_t index, uint32_t array_dwords) {
	return index & (array_dwords - 1u);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_IR_LDSSPAN_H_ */
