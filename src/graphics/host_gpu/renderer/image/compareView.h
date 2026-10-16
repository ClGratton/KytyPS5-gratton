#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_COMPAREVIEW_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_COMPAREVIEW_H_

#include <atomic>
#include <cstdint>

namespace Libs::Graphics {

/**
 * What a shader texture slot that samples with a comparison sampler (OpImageSampleDref*) got bound.
 *
 * Vulkan requires the image view of such a sampling to have a format with
 * SAMPLED_IMAGE_DEPTH_COMPARISON (VUID-vkCmdDraw*-None-06479, VUID-vkCmdDispatch-None-06479). The
 * depth formats offer it everywhere. A colour image the texture cache found at the address of a
 * depth texture (a depth-to-colour copy target, or a render target aliasing a depth buffer) is
 * viewed as R32_SFLOAT, which NVIDIA does not offer it for (AMD does): undefined behaviour there,
 * tied to DEVICE_LOST in upstream issues #1476, #1458 and #1498.
 */
enum class CompareViewVerdict : uint8_t {
	NotCompared, ///< the shader does not sample this slot with a comparison sampler
	Supported,   ///< the bound view's format supports depth comparison: valid
	Unsupported, ///< it does not: undefined behaviour on this GPU
};

[[nodiscard]] constexpr CompareViewVerdict ClassifyComparedView(bool depth_compare,
                                                                bool view_supports_compare) {
	if (!depth_compare) return CompareViewVerdict::NotCompared;
	return view_supports_compare ? CompareViewVerdict::Supported : CompareViewVerdict::Unsupported;
}

/// Counts the Unsupported bindings of a run; the renderer logs the first few loudly.
class CompareViewReport {
public:
	/// Returns the running count including this one when the verdict is Unsupported, else 0.
	uint32_t Note(CompareViewVerdict verdict) {
		return verdict == CompareViewVerdict::Unsupported
		           ? m_unsupported.fetch_add(1, std::memory_order_relaxed) + 1u
		           : 0u;
	}
	[[nodiscard]] uint32_t Unsupported() const { return m_unsupported.load(std::memory_order_relaxed); }

	/// The first kLogLimit occurrences are logged, with the running count.
	static constexpr uint32_t kLogLimit = 16;
	[[nodiscard]] static constexpr bool ShouldLog(uint32_t count) {
		return count != 0 && count <= kLogLimit;
	}

private:
	std::atomic<uint32_t> m_unsupported {0};
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_COMPAREVIEW_H_
