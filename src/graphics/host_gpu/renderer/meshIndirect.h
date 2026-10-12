#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MESHINDIRECT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MESHINDIRECT_H_

#include "common/common.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/indirectDrawArgs.h"

#include <cstdint>

namespace Libs::Graphics {

struct GraphicContext;

// --gpu-indirect-draws (K21). Turns a guest DrawIndexedIndirectArgs block that an earlier GPU pass
// wrote into a mesh draw on the GPU: a one-thread compute pass writes the draw's parameter record
// and a VkDrawMeshTasksIndirectCommandEXT, so the CPU never reads the arguments (and the GPU
// thread never drains the queue to wait for the pass that wrote them). The arithmetic is
// IndirectDrawArgs::BuildMesh.
//
// Ported from BryanKAdams/BryKytyPS5@1a6817502 (GPL-2.0), re-implemented.
class MeshIndirectArgs {
public:
	static constexpr uint64_t RecordOffset  = 0;
	static constexpr uint64_t CommandOffset = IndirectDrawArgs::CommandOffset;
	static constexpr uint64_t OutputSize    = IndirectDrawArgs::OutputBytes;
	static constexpr uint64_t ArgumentsSize = IndirectDrawArgs::IndexedArgsBytes;

	struct Params {
		uint32_t                      index_bytes   = 0;
		uint64_t                      index_address = 0;
		uint32_t                      index_limit   = 0; // INDEX_BUFFER_SIZE in elements; 0 unknown
		IndirectDrawArgs::MeshShape   shape {};
		bool                          record_offsets = true;
		// Write a VkDrawIndexedIndirectCommand (BuildPlain) at the slot for a draw that is not
		// mesh-emulated; the shape is unused.
		bool                          plain = false;
	};

	explicit MeshIndirectArgs(GraphicContext& graphics);
	~MeshIndirectArgs();
	KYTY_CLASS_NO_COPY(MeshIndirectArgs);

	// Records the pass and the barriers that order it after earlier GPU writes of the arguments and
	// before the indirect mesh draw. Must be recorded outside a rendering instance.
	void Record(vk::CommandBuffer command, const Buffer& arguments, uint64_t arguments_offset,
	            const Buffer& output, uint64_t output_offset, const Params& params);

private:
	GraphicContext&         m_graphics;
	vk::DescriptorSetLayout m_descriptor_layout = nullptr;
	vk::PipelineLayout      m_pipeline_layout   = nullptr;
	vk::Pipeline            m_pipeline          = nullptr;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MESHINDIRECT_H_
