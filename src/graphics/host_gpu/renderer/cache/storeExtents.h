#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_STOREEXTENTS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_STOREEXTENTS_H_

// The buffer descriptors of a dispatch's snapshot as StoreExtents (knownStores.h): where each buffer
// resource lives and whether index * stride + offset is the address a store or load of it uses.

#include "graphics/host_gpu/renderer/cache/knownStores.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/shader/recompiler/ir/ResourceSnapshot.h"
#include "kernel/memory.h"

#include <vector>

namespace Libs::Graphics {

inline void MakeStoreExtents(const ShaderRecompiler::IR::ResourceSnapshot& resources,
                             std::vector<StoreExtent>&                      extents) {
	extents.assign(resources.buffers.size(), {});
	for (uint32_t i = 0; i < resources.buffers.size(); i++) {
		const auto d    = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
		const auto base = d.Base48();
		const auto size = static_cast<uint64_t>(d.GetSize());
		auto&      e    = extents[i];
		e.base          = base;
		e.size          = size;
		e.stride        = d.Stride();
		e.direct        = base != 0 && size != 0 && !d.AddTid() && (d.Stride() == 0 || !d.SwizzleEnabled()) &&
		                  LibKernel::Memory::IsFullyMapped(base, size);
	}
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_STOREEXTENTS_H_
