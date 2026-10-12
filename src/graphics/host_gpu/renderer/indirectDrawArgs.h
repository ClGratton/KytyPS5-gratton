#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_INDIRECTDRAWARGS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_INDIRECTDRAWARGS_H_

// Pure model of --gpu-indirect-draws (K21): how a guest DRAW_INDEX_INDIRECT argument block that a
// compute pass wrote becomes a host draw without the CPU reading it. The compute shader
// mesh_indirect_args.comp does exactly this arithmetic on the GPU; this header is the reference it
// is tested against (tests/IndirectDrawArgsTests.cpp and the GPU case in
// tests/ShaderRecompilerComputeTests.cpp).
//
// Ported from BryanKAdams/BryKytyPS5@1a6817502 (mesh-emulated indirect draws built on the GPU) and
// @d8449bd8e (plain indexed draws, count clamp), GPL-2.0. Re-implemented as a separate pure model
// with tests, plus the user-SGPR offset patch that the fork's base did not have.

#include <algorithm>
#include <cstdint>

namespace Libs::Graphics::IndirectDrawArgs {

// Guest DrawIndexedIndirectArgs (the layout of VkDrawIndexedIndirectCommand).
struct IndexedArgs {
	uint32_t index_count;
	uint32_t instance_count;
	uint32_t start_index;
	uint32_t base_vertex; // int32 on the wire
	uint32_t start_instance;
};
inline constexpr uint32_t IndexedArgsDwords = 5;
inline constexpr uint32_t IndexedArgsBytes  = IndexedArgsDwords * 4;

// What a mesh-emulated draw needs from the host: the primitive shape and the device limits.
struct MeshShape {
	uint32_t primitive_size       = 0; // vertices per primitive
	uint32_t primitive_step       = 0; // vertex advance between primitives
	uint32_t primitives_per_group = 0;
	uint32_t max_groups           = 0; // maxMeshWorkGroupCount[0], capped by the total
	uint32_t max_instances        = 0; // maxMeshWorkGroupCount[1]
	uint32_t max_total            = 0; // maxMeshWorkGroupTotalCount
};

struct MeshCommand {
	uint32_t groups;
	uint32_t instances;
	bool     operator==(const MeshCommand&) const = default;
};

// Output slot of the pass: the 7 dwords a direct mesh draw pushes (see EmitMeshDrawParameter; the
// seventh is the base group of a sliced dispatch, always 0 for a GPU-built draw), padded to 8, then
// the VkDrawMeshTasksIndirectCommandEXT (groups, instances, 1).
inline constexpr uint32_t RecordDwords  = 7;
inline constexpr uint32_t CommandWord   = 8;
inline constexpr uint32_t OutputDwords  = 12;
inline constexpr uint32_t OutputBytes   = OutputDwords * 4;
inline constexpr uint32_t CommandOffset = CommandWord * 4;

struct MeshOutput {
	uint32_t    record[RecordDwords] {};
	MeshCommand command {};
	bool        operator==(const MeshOutput&) const = default;
};

// The CPU path clamps the index count to INDEX_BUFFER_SIZE; 0 means unknown.
[[nodiscard]] constexpr uint32_t ClampIndexCount(uint32_t count, uint32_t index_limit) {
	return index_limit != 0 ? std::min(count, index_limit) : count;
}

// Workgroup grid of a mesh draw. The CPU path exits when a draw exceeds the host limits; the GPU
// path cannot, so it clamps (a draw that large is not expected from a real title).
[[nodiscard]] constexpr MeshCommand MeshGrid(uint32_t count, uint32_t instances,
                                             const MeshShape& shape) {
	uint32_t groups = 0;
	if (count >= shape.primitive_size && instances != 0) {
		const uint32_t primitives = (count - shape.primitive_size) / shape.primitive_step + 1u;
		groups                    = (primitives - 1u) / shape.primitives_per_group + 1u;
	}
	groups    = std::min(groups, shape.max_groups);
	instances = std::min(instances, shape.max_instances);
	if (groups != 0 && instances > shape.max_total / groups) {
		instances = shape.max_total / groups;
	}
	if (groups == 0 || instances == 0) {
		groups    = 0;
		instances = 0;
	}
	return {groups, instances};
}

// Output of the argument pass for a mesh-emulated draw. With `record_offsets` the base vertex and
// first instance go into the record (shaders with an embedded fetch take them from the same push
// constants on the CPU path); without it they are 0 and the draw's user SGPRs carry them.
[[nodiscard]] constexpr MeshOutput BuildMesh(const IndexedArgs& args, uint32_t index_bytes,
                                             uint64_t index_address, uint32_t index_limit,
                                             const MeshShape& shape, bool record_offsets) {
	MeshOutput     out {};
	const uint32_t count   = ClampIndexCount(args.index_count, index_limit);
	const uint64_t address = index_address + uint64_t {args.start_index} * index_bytes;
	out.record[0]          = count;
	out.record[1]          = record_offsets ? args.base_vertex : 0u;
	out.record[2]          = record_offsets ? args.start_instance : 0u;
	out.record[3]          = index_bytes;
	out.record[4]          = static_cast<uint32_t>(address);
	out.record[5]          = static_cast<uint32_t>(address >> 32u);
	out.command            = MeshGrid(count, args.instance_count, shape);
	return out;
}

// A plain (non mesh-emulated) indexed draw: the guest block is already a VkDrawIndexedIndirectCommand
// except that the count is clamped to INDEX_BUFFER_SIZE, and that a native-fetch shader takes the
// base vertex and first instance from its patched user SGPRs, so the command carries 0 for them
// (what the CPU path passes to vkCmdDrawIndexed). The index buffer is bound at its base and the
// first index travels in the command.
struct PlainCommand {
	uint32_t index_count;
	uint32_t instance_count;
	uint32_t first_index;
	uint32_t vertex_offset;
	uint32_t first_instance;
	bool     operator==(const PlainCommand&) const = default;
};

[[nodiscard]] constexpr PlainCommand BuildPlain(const IndexedArgs& args, uint32_t index_limit,
                                                bool record_offsets) {
	return {ClampIndexCount(args.index_count, index_limit), args.instance_count, args.start_index,
	        record_offsets ? args.base_vertex : 0u, record_offsets ? args.start_instance : 0u};
}

} // namespace Libs::Graphics::IndirectDrawArgs

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_INDIRECTDRAWARGS_H_
