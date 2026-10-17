// DynamicState::Commit must set every dynamic state a graphics pipeline declares, also while the
// matching test (depth, stencil, bias) is disabled: Vulkan requires it before a draw
// (VUID-vkCmdDraw-None-07845, -07848). No device is needed: the dispatcher entries are replaced by
// recording stubs and the command buffer handle is fake.
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/dynamicState.h"

#include <cstdint>
#include <cstdio>

using namespace Libs::Graphics;

namespace {

struct Recorded {
	uint32_t depth_compare_op = 0;
	uint32_t depth_bias       = 0;
	uint32_t stencil_op_faces = 0;
	uint32_t stencil_ref_faces = 0;
	uint32_t stencil_write_faces = 0;
	uint32_t stencil_compare_faces = 0;
	uint32_t last_depth_compare_op = 0;
	uint32_t other = 0;
	uint32_t Total() const {
		return depth_compare_op + depth_bias + (stencil_op_faces != 0) + (stencil_ref_faces != 0) +
		       (stencil_write_faces != 0) + (stencil_compare_faces != 0) + other;
	}
} g;

int g_failures = 0;
void Require(bool ok, const char* what) {
	if (!ok) {
		std::printf("FAIL: %s\n", what);
		++g_failures;
	}
}

// Stubs. The command buffer handle is never dereferenced.
void VKAPI_CALL SetViewportWithCount(VkCommandBuffer, uint32_t, const VkViewport*) { ++g.other; }
void VKAPI_CALL SetScissorWithCount(VkCommandBuffer, uint32_t, const VkRect2D*) { ++g.other; }
void VKAPI_CALL SetBool(VkCommandBuffer, VkBool32) { ++g.other; }
void VKAPI_CALL SetDepthCompareOp(VkCommandBuffer, VkCompareOp op) {
	++g.depth_compare_op;
	g.last_depth_compare_op = op;
}
void VKAPI_CALL SetDepthBounds(VkCommandBuffer, float, float) { ++g.other; }
void VKAPI_CALL SetDepthBias(VkCommandBuffer, float, float, float) { ++g.depth_bias; }
void VKAPI_CALL SetStencilOp(VkCommandBuffer, VkStencilFaceFlags faces, VkStencilOp, VkStencilOp,
                             VkStencilOp, VkCompareOp) {
	g.stencil_op_faces |= faces;
}
void VKAPI_CALL SetStencilReference(VkCommandBuffer, VkStencilFaceFlags faces, uint32_t) {
	g.stencil_ref_faces |= faces;
}
void VKAPI_CALL SetStencilWriteMask(VkCommandBuffer, VkStencilFaceFlags faces, uint32_t) {
	g.stencil_write_faces |= faces;
}
void VKAPI_CALL SetStencilCompareMask(VkCommandBuffer, VkStencilFaceFlags faces, uint32_t) {
	g.stencil_compare_faces |= faces;
}
void VKAPI_CALL SetBlendConstants(VkCommandBuffer, const float[4]) { ++g.other; }
void VKAPI_CALL SetLineWidth(VkCommandBuffer, float) { ++g.other; }
void VKAPI_CALL SetColorWriteEnable(VkCommandBuffer, uint32_t, const VkBool32*) { ++g.other; }
void VKAPI_CALL SetFeedbackLoop(VkCommandBuffer, VkImageAspectFlags) { ++g.other; }

void InstallStubs() {
	auto& d = VULKAN_HPP_DEFAULT_DISPATCHER;
	d.vkCmdSetViewportWithCount        = SetViewportWithCount;
	d.vkCmdSetScissorWithCount         = SetScissorWithCount;
	d.vkCmdSetDepthTestEnable          = SetBool;
	d.vkCmdSetDepthWriteEnable         = SetBool;
	d.vkCmdSetDepthCompareOp           = SetDepthCompareOp;
	d.vkCmdSetDepthBoundsTestEnable    = SetBool;
	d.vkCmdSetDepthBounds              = SetDepthBounds;
	d.vkCmdSetDepthBiasEnable          = SetBool;
	d.vkCmdSetDepthBias                = SetDepthBias;
	d.vkCmdSetStencilTestEnable        = SetBool;
	d.vkCmdSetStencilOp                = SetStencilOp;
	d.vkCmdSetStencilReference         = SetStencilReference;
	d.vkCmdSetStencilWriteMask         = SetStencilWriteMask;
	d.vkCmdSetStencilCompareMask       = SetStencilCompareMask;
	d.vkCmdSetBlendConstants           = SetBlendConstants;
	d.vkCmdSetLineWidth                = SetLineWidth;
	d.vkCmdSetColorWriteEnableEXT      = SetColorWriteEnable;
	d.vkCmdSetAttachmentFeedbackLoopEnableEXT = SetFeedbackLoop;
}

constexpr uint32_t kBothFaces = VK_STENCIL_FACE_FRONT_BIT | VK_STENCIL_FACE_BACK_BIT;

} // namespace

int main() {
	InstallStubs();
	const GraphicContext graphics {};
	const vk::CommandBuffer cmdbuf(reinterpret_cast<VkCommandBuffer>(uintptr_t {0x1000}));

	// Depth test, stencil test and polygon offset all disabled, as for a plain colour draw.
	DynamicState state {};
	state.Invalidate();
	state.SetDepthTestEnabled(false);
	state.SetStencilTestEnabled(false);
	state.SetDepthBiasEnabled(false);
	state.Commit(graphics, cmdbuf);
	Require(g.depth_compare_op == 1, "depth compare op not set while the depth test is disabled");
	Require(g.depth_bias == 1, "depth bias not set while polygon offset is disabled");
	Require((g.stencil_op_faces & kBothFaces) == kBothFaces,
	        "stencil op not set for both faces while the stencil test is disabled");
	Require((g.stencil_ref_faces & kBothFaces) == kBothFaces,
	        "stencil reference not set for both faces while the stencil test is disabled");
	Require((g.stencil_write_faces & kBothFaces) == kBothFaces,
	        "stencil write mask not set for both faces while the stencil test is disabled");
	Require((g.stencil_compare_faces & kBothFaces) == kBothFaces,
	        "stencil compare mask not set for both faces while the stencil test is disabled");

	// Nothing changed: a second commit emits nothing.
	const auto after_first = g.Total();
	state.Commit(graphics, cmdbuf);
	Require(g.Total() == after_first, "a clean second commit emitted dynamic state again");

	// A new command buffer invalidates everything: all of it is set again.
	state.Invalidate();
	state.Commit(graphics, cmdbuf);
	Require(g.depth_compare_op == 2 && g.depth_bias == 2,
	        "an invalidated state did not re-emit depth compare op and bias");

	// A compare op change while the depth test is disabled is still emitted before the next draw.
	state.SetDepthCompareOp(vk::CompareOp::eGreaterOrEqual);
	state.Commit(graphics, cmdbuf);
	Require(g.depth_compare_op == 3 &&
	            g.last_depth_compare_op == static_cast<uint32_t>(vk::CompareOp::eGreaterOrEqual),
	        "a depth compare op change was dropped while the depth test is disabled");

	if (g_failures != 0) {
		return 1;
	}
	std::printf("dynamic state: all dynamic state set while the tests are disabled: ok\n");
	return 0;
}
