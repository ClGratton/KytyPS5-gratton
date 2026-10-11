// Offscreen image diff: monolithic vs fast-linked vs optimized-linked output of the same
// captured pipeline, rendered with dummy resources (deterministic pseudo-random data).
// Included by gpl_bench.cpp (needs its globals); not a separate translation unit.

static uint32_t FindMem(uint32_t bits, VkMemoryPropertyFlags want) {
	for (uint32_t i = 0; i < g.mem.memoryTypeCount; i++) {
		if ((bits & (1u << i)) && (g.mem.memoryTypes[i].propertyFlags & want) == want) return i;
	}
	std::fprintf(stderr, "no memory type\n");
	std::abort();
}

struct Buf {
	VkBuffer       b = nullptr;
	VkDeviceMemory m = nullptr;
	void*          map = nullptr;
	VkDeviceSize   size = 0;
};

static Buf MakeBuf(VkDeviceSize size, VkBufferUsageFlags usage) {
	Buf r;
	r.size = size;
	VkBufferCreateInfo ci {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	ci.size  = size;
	ci.usage = usage;
	VKCHK(vkCreateBuffer(g.dev, &ci, nullptr, &r.b));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(g.dev, r.b, &mr);
	VkMemoryAllocateInfo ai {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize  = mr.size;
	ai.memoryTypeIndex = FindMem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VKCHK(vkAllocateMemory(g.dev, &ai, nullptr, &r.m));
	VKCHK(vkBindBufferMemory(g.dev, r.b, r.m, 0));
	VKCHK(vkMapMemory(g.dev, r.m, 0, size, 0, &r.map));
	return r;
}

static void FillRandomFloats(Buf& b, uint32_t seed) {
	std::mt19937 rng(seed);
	auto*        f = static_cast<float*>(b.map);
	for (VkDeviceSize i = 0; i < b.size / 4; i++) f[i] = static_cast<float>(rng() & 0xffffff) / 16777216.0f;
}

struct Img {
	VkImage        img  = nullptr;
	VkDeviceMemory mem  = nullptr;
	VkImageView    view = nullptr;
	VkFormat       fmt {};
	uint32_t       w = 0, h = 0, d = 1, layers = 1;
	VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
};

static Img MakeImg(VkImageType type, VkImageViewType vtype, VkFormat fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t layers,
                   VkImageUsageFlags usage, VkImageAspectFlags aspect) {
	Img r;
	r.fmt = fmt; r.w = w; r.h = h; r.d = d; r.layers = layers; r.aspect = aspect;
	VkImageCreateInfo ci {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ci.imageType     = type;
	ci.format        = fmt;
	ci.extent        = {w, h, d};
	ci.mipLevels     = 1;
	ci.arrayLayers   = layers;
	ci.samples       = VK_SAMPLE_COUNT_1_BIT;
	ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
	ci.usage         = usage;
	ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VKCHK(vkCreateImage(g.dev, &ci, nullptr, &r.img));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(g.dev, r.img, &mr);
	VkMemoryAllocateInfo ai {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize  = mr.size;
	ai.memoryTypeIndex = FindMem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VKCHK(vkAllocateMemory(g.dev, &ai, nullptr, &r.mem));
	VKCHK(vkBindImageMemory(g.dev, r.img, r.mem, 0));
	VkImageViewCreateInfo vi {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vi.image            = r.img;
	vi.viewType         = vtype;
	vi.format           = fmt;
	vi.subresourceRange = {aspect, 0, 1, 0, layers};
	VKCHK(vkCreateImageView(g.dev, &vi, nullptr, &r.view));
	return r;
}

static uint32_t FormatBytes(VkFormat f) {
	switch (f) {
		case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_SINT: return 1;
		case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_UINT:
		case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R16_SNORM: return 2;
		case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
		case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SNORM: case VK_FORMAT_R8G8B8A8_SINT:
		case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
		case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SNORM:
		case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
		case VK_FORMAT_A2B10G10R10_UINT_PACK32: return 4;
		case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_UINT:
		case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_UINT: return 8;
		case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: return 16;
		default: return 0;
	}
}

static VkCommandPool   g_pool;
static VkCommandBuffer BeginCmd() {
	if (!g_pool) {
		VkCommandPoolCreateInfo pi {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pi.queueFamilyIndex = g.qfam;
		pi.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		VKCHK(vkCreateCommandPool(g.dev, &pi, nullptr, &g_pool));
	}
	VkCommandBufferAllocateInfo ai {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	ai.commandPool        = g_pool;
	ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	VkCommandBuffer cb;
	VKCHK(vkAllocateCommandBuffers(g.dev, &ai, &cb));
	VkCommandBufferBeginInfo bi {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VKCHK(vkBeginCommandBuffer(cb, &bi));
	return cb;
}
static void EndSubmit(VkCommandBuffer cb) {
	VKCHK(vkEndCommandBuffer(cb));
	VkSubmitInfo si {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers    = &cb;
	VKCHK(vkQueueSubmit(g.queue, 1, &si, VK_NULL_HANDLE));
	VKCHK(vkQueueWaitIdle(g.queue));
	vkFreeCommandBuffers(g.dev, g_pool, 1, &cb);
}

static void ImageBarrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, uint32_t layers, VkImageLayout from, VkImageLayout to,
                         VkPipelineStageFlags2 s0, VkAccessFlags2 a0, VkPipelineStageFlags2 s1, VkAccessFlags2 a1) {
	VkImageMemoryBarrier2 b {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
	b.srcStageMask = s0; b.srcAccessMask = a0; b.dstStageMask = s1; b.dstAccessMask = a1;
	b.oldLayout = from; b.newLayout = to;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = img;
	b.subresourceRange = {aspect, 0, 1, 0, layers};
	VkDependencyInfo di {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	di.imageMemoryBarrierCount = 1;
	di.pImageMemoryBarriers    = &b;
	vkCmdPipelineBarrier2(cb, &di);
}

struct DummyRes {
	Buf rnd, zero;
	std::map<uint32_t, Img> sampled; // by kind
	VkSampler sampler = nullptr;
};

// Sampled image kinds: 1..7 float, 8..14 uint, 15..21 sint, 22..28 depth-compare (left null).
static bool KindImage(uint32_t kind, VkFormat& fmt, VkImageType& type, VkImageViewType& vt, uint32_t& layers, uint32_t& d) {
	if (kind < 1 || kind > 21) return false;
	uint32_t cls = (kind - 1) / 7, dim = (kind - 1) % 7;
	fmt = cls == 0 ? VK_FORMAT_R8G8B8A8_UNORM : cls == 1 ? VK_FORMAT_R8G8B8A8_UINT : VK_FORMAT_R8G8B8A8_SINT;
	layers = 1; d = 1;
	switch (dim) {
		case 0: type = VK_IMAGE_TYPE_1D; vt = VK_IMAGE_VIEW_TYPE_1D; break;
		case 1: type = VK_IMAGE_TYPE_1D; vt = VK_IMAGE_VIEW_TYPE_1D_ARRAY; layers = 2; break;
		case 2: type = VK_IMAGE_TYPE_2D; vt = VK_IMAGE_VIEW_TYPE_2D; break;
		case 3: type = VK_IMAGE_TYPE_2D; vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY; layers = 2; break;
		case 6: type = VK_IMAGE_TYPE_3D; vt = VK_IMAGE_VIEW_TYPE_3D; d = 8; break;
		default: return false; // MSAA: null descriptor
	}
	return true;
}

static void InitDummy(DummyRes& dr) {
	dr.rnd  = MakeBuf(1u << 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
	FillRandomFloats(dr.rnd, 1234);
	dr.zero = MakeBuf(1u << 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	std::memset(dr.zero.map, 0, dr.zero.size);
	VkSamplerCreateInfo si {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	si.magFilter = si.minFilter = VK_FILTER_LINEAR;
	si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	VKCHK(vkCreateSampler(g.dev, &si, nullptr, &dr.sampler));
}

static Img& GetSampled(DummyRes& dr, uint32_t kind, bool& ok) {
	auto it = dr.sampled.find(kind);
	if (it != dr.sampled.end()) { ok = it->second.img != nullptr; return it->second; }
	VkFormat fmt; VkImageType type; VkImageViewType vt; uint32_t layers, d;
	Img img;
	ok = KindImage(kind, fmt, type, vt, layers, d);
	if (ok) {
		uint32_t w = 8, h = type == VK_IMAGE_TYPE_1D ? 1 : 8;
		img = MakeImg(type, vt, fmt, w, h, d, layers, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
		const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * d * layers * 4;
		auto cb = BeginCmd();
		ImageBarrier(cb, img.img, VK_IMAGE_ASPECT_COLOR_BIT, layers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		             VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
		VkBufferImageCopy c {};
		c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
		c.imageExtent      = {w, h, d};
		vkCmdCopyBufferToImage(cb, dr.rnd.b, img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
		(void)bytes;
		ImageBarrier(cb, img.img, VK_IMAGE_ASPECT_COLOR_BIT, layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		             VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
		EndSubmit(cb);
	}
	return dr.sampled[kind] = img;
}

struct DiffStats {
	size_t bytes = 0, diff_bytes = 0, changed_from_clear = 0;
	int    max_abs = 0;
};

static const uint8_t kClearByte = 0x40;

static void SetDynamicState(Pipe& p, VkCommandBuffer cb, uint32_t w, uint32_t h) {
	for (auto d: p.dyn) {
		switch (d) {
			case VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT: { VkViewport v {0, 0, float(w), float(h), 0.f, 1.f}; vkCmdSetViewportWithCount(cb, 1, &v); break; }
			case VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT: { VkRect2D s {{0, 0}, {w, h}}; vkCmdSetScissorWithCount(cb, 1, &s); break; }
			case VK_DYNAMIC_STATE_LINE_WIDTH: vkCmdSetLineWidth(cb, 1.0f); break;
			case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE: vkCmdSetDepthTestEnable(cb, VK_TRUE); break;
			case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE: vkCmdSetDepthWriteEnable(cb, VK_TRUE); break;
			case VK_DYNAMIC_STATE_DEPTH_COMPARE_OP: vkCmdSetDepthCompareOp(cb, VK_COMPARE_OP_LESS_OR_EQUAL); break;
			case VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE: vkCmdSetDepthBiasEnable(cb, VK_FALSE); break;
			case VK_DYNAMIC_STATE_DEPTH_BIAS: vkCmdSetDepthBias(cb, 0.f, 0.f, 0.f); break;
			case VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE: vkCmdSetStencilTestEnable(cb, VK_FALSE); break;
			case VK_DYNAMIC_STATE_STENCIL_OP: vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS); break;
			case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK: vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff); break;
			case VK_DYNAMIC_STATE_STENCIL_REFERENCE: vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0); break;
			case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK: vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff); break;
			case VK_DYNAMIC_STATE_BLEND_CONSTANTS: { float c[4] = {0.25f, 0.5f, 0.75f, 1.0f}; vkCmdSetBlendConstants(cb, c); break; }
			case VK_DYNAMIC_STATE_COLOR_WRITE_ENABLE_EXT: {
				std::vector<VkBool32> e(p.color_formats.size(), VK_TRUE);
				if (!e.empty()) g.CmdSetColorWriteEnable(cb, static_cast<uint32_t>(e.size()), e.data());
				break;
			}
			case VK_DYNAMIC_STATE_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT: g.CmdSetFeedbackLoop(cb, 0); break;
			default: std::fprintf(stderr, "unhandled dynamic state %d\n", d); break;
		}
	}
}

// Renders `pl` and returns the raw bytes of every color attachment (+ depth if D32/D16).
static bool RenderOnce(Pipe& p, VkPipeline pl, VkPipelineLayout layout, VkDescriptorSet set, DummyRes& dr, std::vector<std::vector<uint8_t>>& out,
                       std::string& why) {
	const uint32_t W = 128, H = 128;
	std::vector<Img> colors;
	std::vector<Buf> rb;
	for (auto f: p.color_formats) {
		if (!FormatBytes(f)) { why = "color format " + std::to_string(f) + " unsupported by diff"; return false; }
	}
	for (auto f: p.color_formats) {
		colors.push_back(MakeImg(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, f, W, H, 1, 1,
		                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT));
	}
	Img  depth;
	bool has_depth = p.rendering.depthAttachmentFormat != VK_FORMAT_UNDEFINED || p.rendering.stencilAttachmentFormat != VK_FORMAT_UNDEFINED;
	VkImageAspectFlags daspect = 0;
	if (p.rendering.depthAttachmentFormat != VK_FORMAT_UNDEFINED) daspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
	if (p.rendering.stencilAttachmentFormat != VK_FORMAT_UNDEFINED) daspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
	if (has_depth) {
		VkFormat df = p.rendering.depthAttachmentFormat != VK_FORMAT_UNDEFINED ? p.rendering.depthAttachmentFormat : p.rendering.stencilAttachmentFormat;
		depth = MakeImg(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, df, W, H, 1, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, daspect);
	}
	// vertex buffers: the shared random buffer for every binding
	auto cb = BeginCmd();
	std::vector<VkRenderingAttachmentInfo> cai;
	for (auto& c: colors) {
		ImageBarrier(cb, c.img, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		             VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
		VkRenderingAttachmentInfo a {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
		a.imageView   = c.view;
		a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		a.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
		a.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
		std::memset(&a.clearValue, kClearByte, sizeof(a.clearValue)); // not a byte-exact fill for all formats; only used as a baseline
		a.clearValue.color = {{0.25f, 0.25f, 0.25f, 0.25f}};
		cai.push_back(a);
	}
	VkRenderingAttachmentInfo dai {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	if (has_depth) {
		ImageBarrier(cb, depth.img, daspect, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		             VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
		             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
		dai.imageView   = depth.view;
		dai.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		dai.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
		dai.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
		dai.clearValue.depthStencil = {1.0f, 0};
	}
	VkRenderingInfo ri {VK_STRUCTURE_TYPE_RENDERING_INFO};
	ri.renderArea           = {{0, 0}, {W, H}};
	ri.layerCount           = 1;
	ri.colorAttachmentCount = static_cast<uint32_t>(cai.size());
	ri.pColorAttachments    = cai.data();
	if (p.rendering.depthAttachmentFormat != VK_FORMAT_UNDEFINED) ri.pDepthAttachment = &dai;
	if (p.rendering.stencilAttachmentFormat != VK_FORMAT_UNDEFINED) ri.pStencilAttachment = &dai;
	vkCmdBeginRendering(cb, &ri);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
	SetDynamicState(p, cb, W, H);
	std::vector<VkBuffer>     vb(p.vib.size(), dr.rnd.b);
	std::vector<VkDeviceSize> vo(p.vib.size(), 0);
	if (!vb.empty()) vkCmdBindVertexBuffers(cb, 0, static_cast<uint32_t>(vb.size()), vb.data(), vo.data());
	if (set) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
	{
		std::vector<uint32_t> pc(p.push_size / 4);
		std::mt19937          rng(99);
		for (auto& x: pc) x = rng() & 0xffff;
		vkCmdPushConstants(cb, layout, p.push_stages ? p.push_stages : kAllGraphics, 0, p.push_size, pc.data());
	}
	vkCmdDraw(cb, 192, 2, 0, 0);
	vkCmdEndRendering(cb);
	std::vector<VkDeviceSize> sizes;
	for (auto& c: colors) {
		ImageBarrier(cb, c.img, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		VkDeviceSize sz = static_cast<VkDeviceSize>(W) * H * FormatBytes(c.fmt);
		rb.push_back(MakeBuf(sz, VK_BUFFER_USAGE_TRANSFER_DST_BIT));
		VkBufferImageCopy cp {};
		cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		cp.imageExtent      = {W, H, 1};
		vkCmdCopyImageToBuffer(cb, c.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.back().b, 1, &cp);
	}
	bool depth_copied = false;
	if (has_depth && p.rendering.depthAttachmentFormat != VK_FORMAT_UNDEFINED &&
	    (depth.fmt == VK_FORMAT_D32_SFLOAT || depth.fmt == VK_FORMAT_D16_UNORM)) {
		ImageBarrier(cb, depth.img, daspect, 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		rb.push_back(MakeBuf(static_cast<VkDeviceSize>(W) * H * (depth.fmt == VK_FORMAT_D32_SFLOAT ? 4 : 2), VK_BUFFER_USAGE_TRANSFER_DST_BIT));
		VkBufferImageCopy cp {};
		cp.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
		cp.imageExtent      = {W, H, 1};
		vkCmdCopyImageToBuffer(cb, depth.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.back().b, 1, &cp);
		depth_copied = true;
	}
	(void)depth_copied;
	EndSubmit(cb);
	out.clear();
	for (auto& b: rb) {
		out.emplace_back(static_cast<uint8_t*>(b.map), static_cast<uint8_t*>(b.map) + b.size);
		vkUnmapMemory(g.dev, b.m);
		vkDestroyBuffer(g.dev, b.b, nullptr);
		vkFreeMemory(g.dev, b.m, nullptr);
	}
	for (auto& c: colors) { vkDestroyImageView(g.dev, c.view, nullptr); vkDestroyImage(g.dev, c.img, nullptr); vkFreeMemory(g.dev, c.mem, nullptr); }
	if (has_depth) { vkDestroyImageView(g.dev, depth.view, nullptr); vkDestroyImage(g.dev, depth.img, nullptr); vkFreeMemory(g.dev, depth.mem, nullptr); }
	return true;
}

static DiffStats Compare(const std::vector<std::vector<uint8_t>>& a, const std::vector<std::vector<uint8_t>>& b) {
	DiffStats s;
	for (size_t i = 0; i < a.size(); i++) {
		for (size_t k = 0; k < a[i].size(); k++) {
			s.bytes++;
			int d = std::abs(int(a[i][k]) - int(b[i][k]));
			if (d) { s.diff_bytes++; s.max_abs = std::max(s.max_abs, d); }
		}
	}
	return s;
}

static size_t CountDistinct(const std::vector<std::vector<uint8_t>>& a) {
	// number of distinct 4-byte (or less) groups in the first attachment: >1 means the render is not a flat fill
	std::set<uint32_t> seen;
	if (a.empty()) return 0;
	for (size_t k = 0; k + 4 <= a[0].size(); k += 4) {
		uint32_t v; std::memcpy(&v, &a[0][k], 4);
		seen.insert(v);
		if (seen.size() > 100000) break;
	}
	return seen.size();
}

struct WriteSet {
	std::vector<std::vector<VkDescriptorBufferInfo>> binfo;
	std::vector<std::vector<VkDescriptorImageInfo>>  iinfo;
	std::vector<VkWriteDescriptorSet>                writes;
	bool                                             ok = true;
	uint32_t                                         descriptors = 0;
};

static void BuildWrites(Pipe* p, DummyRes& dr, VkDescriptorSet set, WriteSet& ws) {
	ws.binfo.assign(p->bindings.size(), {});
	ws.iinfo.assign(p->bindings.size(), {});
	ws.writes.clear();
	ws.ok = true;
	ws.descriptors = 0;
	auto& binfo = ws.binfo; auto& iinfo = ws.iinfo; auto& writes = ws.writes; bool& ok = ws.ok;
	for (size_t bi = 0; bi < p->bindings.size(); bi++) {
		auto& b = p->bindings[bi];
		uint32_t kind = b.binding % 50;
		ws.descriptors += b.count;
		VkWriteDescriptorSet w {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w.dstSet = set; w.dstBinding = b.binding; w.descriptorCount = b.count; w.descriptorType = static_cast<VkDescriptorType>(b.type);
		if (b.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
			Buf& src = (kind == 46 || kind == 47) ? dr.zero : dr.rnd;
			binfo[bi].assign(b.count, VkDescriptorBufferInfo {src.b, 0, VK_WHOLE_SIZE});
			w.pBufferInfo = binfo[bi].data();
		} else if (b.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
			bool have; Img& im = GetSampled(dr, kind, have);
			iinfo[bi].assign(b.count, VkDescriptorImageInfo {VK_NULL_HANDLE, have ? im.view : VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
			if (!have && !g.null_descriptor) ok = false;
			w.pImageInfo = iinfo[bi].data();
		} else if (b.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
			iinfo[bi].assign(b.count, VkDescriptorImageInfo {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL});
			if (!g.null_descriptor) ok = false;
			w.pImageInfo = iinfo[bi].data();
		} else if (b.type == VK_DESCRIPTOR_TYPE_SAMPLER) {
			iinfo[bi].assign(b.count, VkDescriptorImageInfo {dr.sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
			w.pImageInfo = iinfo[bi].data();
		} else { ok = false; }
		writes.push_back(w);
	}
}

static int RunDiff(Opts& o, std::vector<Pipe*>& sel, SuperLayout& super) {
	DummyRes dr;
	InitDummy(dr);
	int n_ok = 0, n_skip = 0, n_same = 0;
	for (auto* p: sel) {
		if (p->mesh || p->rect_list || p->tess) { n_skip++; continue; }
		if (p->ms.rasterizationSamples != VK_SAMPLE_COUNT_1_BIT) { std::printf("seq %3d skip: msaa\n", p->seq); n_skip++; continue; }
		if (p->has_ia && p->ia.topology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST) { std::printf("seq %3d skip: patch list\n", p->seq); n_skip++; continue; }
		// descriptor layout without the push flag (so a plain set can be allocated)
		VkDescriptorSetLayout dsl = MakeDsl(p->bindings, false, kAllGraphics);
		VkPipelineLayout      pl_own = MakePl(dsl, kAllGraphics, 0, 128);
		VkDescriptorSetLayout dsl_use = dsl;
		VkPipelineLayout      pl_use = pl_own;
		if (o.layout == "super") { dsl_use = super.dsl; pl_use = super.layout; }
		// descriptor set + writes
		VkDescriptorPoolSize ps[4] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16384}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16384}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16384}, {VK_DESCRIPTOR_TYPE_SAMPLER, 16384}};
		VkDescriptorPoolCreateInfo dpi {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		dpi.maxSets = 2; dpi.poolSizeCount = 4; dpi.pPoolSizes = ps;
		VkDescriptorPool pool; VKCHK(vkCreateDescriptorPool(g.dev, &dpi, nullptr, &pool));
		VkDescriptorSetAllocateInfo dai {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		dai.descriptorPool = pool; dai.descriptorSetCount = 1; dai.pSetLayouts = &dsl_use;
		VkDescriptorSet set; VKCHK(vkAllocateDescriptorSets(g.dev, &dai, &set));
		WriteSet ws;
		BuildWrites(p, dr, set, ws);
		bool ok = ws.ok;
		auto& writes = ws.writes;
		if (!ok) { std::printf("seq %3d skip: descriptor kinds unsupported by diff\n", p->seq); n_skip++; continue; }
		if (!writes.empty()) vkUpdateDescriptorSets(g.dev, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

		// pipelines: mono on the per-pipeline diff layout, GPL libs on `pl_use`
		VkPipeline mono = nullptr, fast = nullptr, opt = nullptr;
		VkResult   res;
		auto       ci = BuildMono(*p, pl_own);
		VKCHK(vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &ci, nullptr, &mono));
		GplResult gr; double t[4];
		BuildLibs(*p, pl_use, true, VK_NULL_HANDLE, gr, t);
		bool libs_ok = true;
		for (auto r: gr.res) if (r != VK_SUCCESS) libs_ok = false;
		if (!libs_ok) { std::printf("seq %3d lib creation failed\n", p->seq); n_skip++; continue; }
		VkPipelineLibraryCreateInfoKHR li;
		auto cif = BuildLink(gr.libs, li, pl_use, false);
		VKCHK(vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &cif, nullptr, &fast));
		auto cio = BuildLink(gr.libs, li, pl_use, true);
		VKCHK(vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &cio, nullptr, &opt));
		(void)res;

		std::vector<std::vector<uint8_t>> im_mono, im_fast, im_opt, im_mono2;
		std::string why;
		// the mono pipeline is always drawn through pl_own's set layout; bind the same set only if layouts
		// are compatible. With the superset layout the set was allocated from `super.dsl`, so draw mono with
		// a set allocated from `dsl` instead.
		VkDescriptorSet set_own = set;
		if (o.layout == "super") {
			VkDescriptorSetAllocateInfo a2 = dai; a2.pSetLayouts = &dsl;
			VKCHK(vkAllocateDescriptorSets(g.dev, &a2, &set_own));
			for (auto& w: writes) w.dstSet = set_own;
			if (!writes.empty()) vkUpdateDescriptorSets(g.dev, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
		}
		if (!RenderOnce(*p, mono, pl_own, set_own, dr, im_mono, why) ||
		    !RenderOnce(*p, fast, pl_use, set, dr, im_fast, why) ||
		    !RenderOnce(*p, opt, pl_use, set, dr, im_opt, why) ||
		    !RenderOnce(*p, mono, pl_own, set_own, dr, im_mono2, why)) {
			std::printf("seq %3d skip: %s\n", p->seq, why.c_str());
			n_skip++;
			continue;
		}
		auto d_fm = Compare(im_mono, im_fast);
		auto d_om = Compare(im_mono, im_opt);
		auto d_fo = Compare(im_fast, im_opt);
		auto d_mm = Compare(im_mono, im_mono2); // determinism control
		size_t distinct = CountDistinct(im_mono);
		std::printf("seq %3d %-5s layout=%s bytes=%zu distinct_texels=%zu | mono-vs-mono(control) diff=%zu | mono-vs-fast diff=%zu max=%d | mono-vs-opt diff=%zu max=%d | fast-vs-opt diff=%zu max=%d\n",
		            p->seq, p->Class(), o.layout.c_str(), d_fm.bytes, distinct, d_mm.diff_bytes, d_fm.diff_bytes, d_fm.max_abs, d_om.diff_bytes, d_om.max_abs, d_fo.diff_bytes, d_fo.max_abs);
		std::fflush(stdout);
		n_ok++;
		if (d_fm.diff_bytes == 0 && d_om.diff_bytes == 0 && d_fo.diff_bytes == 0) n_same++;
		vkDestroyPipeline(g.dev, mono, nullptr); vkDestroyPipeline(g.dev, fast, nullptr); vkDestroyPipeline(g.dev, opt, nullptr);
		for (auto l: gr.libs) vkDestroyPipeline(g.dev, l, nullptr);
		vkDestroyDescriptorPool(g.dev, pool, nullptr);
	}
	std::printf("DIFF summary: rendered=%d identical=%d skipped=%d\n", n_ok, n_same, n_skip);
	return 0;
}

// ---------------------------------------------------------------------------------------------
// Per-draw descriptor-path cost: push descriptors vs DescriptorHeap-style set (allocate + update +
// bind), with the pipeline's own layout vs the shared superset layout. CPU time only.

static int RunDescBench(Opts& o, std::vector<Pipe*>& sel, SuperLayout& super) {
	(void)o;
	DummyRes dr;
	InitDummy(dr);
	auto push = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(vkGetDeviceProcAddr(g.dev, "vkCmdPushDescriptorSetKHR"));
	constexpr int kIters = 2000, kBatch = 250;
	std::printf("desc-bench: iterations per measurement %d (CPU wall time per draw, microseconds)\n", kIters);
	std::printf("%-5s %5s %7s | %-9s | %-12s | %-13s | alloc/set own | alloc/set super\n", "seq", "ndesc", "nwrites", "push", "heap(own)", "heap(super)");
	for (auto* p: sel) {
		WriteSet probe;
		BuildWrites(p, dr, VK_NULL_HANDLE, probe);
		if (!probe.ok) continue;
		const bool can_push = probe.descriptors <= g.max_push_descriptors;
		VkDescriptorSetLayout dsl_own = MakeDsl(p->bindings, false, kAllGraphics);
		VkDescriptorSetLayout dsl_push = can_push ? MakeDsl(p->bindings, true, kAllGraphics) : nullptr;
		VkPipelineLayout pl_own = MakePl(dsl_own, kAllGraphics, 0, 128);
		VkPipelineLayout pl_push = can_push ? MakePl(dsl_push, kAllGraphics, 0, 128) : nullptr;

		auto measure_heap = [&](VkDescriptorSetLayout dsl, VkPipelineLayout pl, double& alloc_us) -> double {
			// pool sized for kBatch sets
			std::map<uint32_t, uint32_t> per_type;
			std::vector<BindingDesc>& bl = (dsl == super.dsl) ? super.bindings : p->bindings;
			for (auto& b: bl) per_type[b.type] += b.count;
			std::vector<VkDescriptorPoolSize> sizes;
			for (auto& [t, c]: per_type) sizes.push_back({static_cast<VkDescriptorType>(t), c * kBatch});
			VkDescriptorPoolCreateInfo pi {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
			pi.maxSets = kBatch; pi.poolSizeCount = static_cast<uint32_t>(sizes.size()); pi.pPoolSizes = sizes.data();
			VkDescriptorPool pool; VKCHK(vkCreateDescriptorPool(g.dev, &pi, nullptr, &pool));
			std::vector<VkDescriptorSet> sets(kBatch);
			std::vector<VkDescriptorSetLayout> ls(kBatch, dsl);
			double total = 0, alloc_total = 0;
			VkCommandBuffer cb = BeginCmd();
			for (int done = 0; done < kIters; done += kBatch) {
				auto ta = Clock::now();
				VkDescriptorSetAllocateInfo ai {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
				ai.descriptorPool = pool; ai.descriptorSetCount = kBatch; ai.pSetLayouts = ls.data();
				VKCHK(vkAllocateDescriptorSets(g.dev, &ai, sets.data()));
				alloc_total += MsSince(ta);
				auto t0 = Clock::now();
				for (int i = 0; i < kBatch; i++) {
					for (auto& w: probe.writes) w.dstSet = sets[i];
					vkUpdateDescriptorSets(g.dev, static_cast<uint32_t>(probe.writes.size()), probe.writes.data(), 0, nullptr);
					vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &sets[i], 0, nullptr);
				}
				total += MsSince(t0);
				vkResetDescriptorPool(g.dev, pool, 0);
				VKCHK(vkResetCommandBuffer(cb, 0));
				VkCommandBufferBeginInfo bi {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
				VKCHK(vkBeginCommandBuffer(cb, &bi));
			}
			vkEndCommandBuffer(cb);
			vkFreeCommandBuffers(g.dev, g_pool, 1, &cb);
			vkDestroyDescriptorPool(g.dev, pool, nullptr);
			alloc_us = alloc_total * 1000.0 / kIters;
			return total * 1000.0 / kIters;
		};
		double a_own = 0, a_super = 0, push_us = -1;
		double heap_own = measure_heap(dsl_own, pl_own, a_own);
		// superset: writes target the same binding numbers
		double heap_super = measure_heap(super.dsl, super.layout, a_super);
		if (can_push) {
			VkCommandBuffer cb = BeginCmd();
			double total = 0;
			for (int done = 0; done < kIters; done += kBatch) {
				auto t0 = Clock::now();
				for (int i = 0; i < kBatch; i++) {
					push(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_push, 0, static_cast<uint32_t>(probe.writes.size()), probe.writes.data());
				}
				total += MsSince(t0);
				VKCHK(vkResetCommandBuffer(cb, 0));
				VkCommandBufferBeginInfo bi {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
				VKCHK(vkBeginCommandBuffer(cb, &bi));
			}
			vkEndCommandBuffer(cb);
			vkFreeCommandBuffers(g.dev, g_pool, 1, &cb);
			push_us = total * 1000.0 / kIters;
		}
		std::printf("%-5d %5u %7zu | %9.2f | %12.2f | %13.2f | %13.2f | %15.2f\n", p->seq, probe.descriptors, probe.writes.size(), push_us, heap_own, heap_super, a_own, a_super);
		std::fflush(stdout);
	}
	return 0;
}
