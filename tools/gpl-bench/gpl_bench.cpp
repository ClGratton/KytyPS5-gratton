// D2 spike (shaders.md Decision 2, Step 1): standalone VK_EXT_graphics_pipeline_library benchmark.
//
// Replays pipelines captured by `kyty_emulator --dump-pipelines <dir>` and measures, on this
// driver and one worker thread per measurement:
//   mono  : vkCreateGraphicsPipelines of the full pipeline (what Kyty does today)
//   gpl   : the four libraries (vertex-input, pre-raster, fragment-shader, fragment-output), then
//           the fast link (no LINK_TIME_OPTIMIZATION) and the optimized link (with LTO)
//   e2e   : the four libraries compiled concurrently (4 / 2 workers) + fast link, wall clock
//   par   : makespan of the whole sample on N concurrent workers (mono or gpl-fast)
//   diff  : offscreen render of mono vs fast-link vs optimized-link, byte comparison
// Layouts: `own` = the captured per-pipeline layout; `super` = one shared superset layout.
//
// Run cold: __GL_SHADER_DISK_CACHE=0 (see run_bench.sh). One variant per process.
//
// SPDX-License-Identifier: GPL-2.0-only (part of the KytyPS5 fork; no third-party code)

#include <vulkan/vulkan.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

#define VKCHK(x)                                                                                   \
	do {                                                                                           \
		VkResult r_ = (x);                                                                         \
		if (r_ != VK_SUCCESS) {                                                                    \
			std::fprintf(stderr, "%s failed: %d (%s:%d)\n", #x, static_cast<int>(r_), __FILE__,    \
			             __LINE__);                                                                \
			std::abort();                                                                          \
		}                                                                                          \
	} while (0)

static double MsSince(Clock::time_point t0) {
	return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// ------------------------------------------------------------------------------------------------
// device

struct Ctx {
	VkInstance       instance = nullptr;
	VkPhysicalDevice phys     = nullptr;
	VkDevice         dev      = nullptr;
	VkQueue          queue    = nullptr;
	uint32_t         qfam     = 0;
	VkPhysicalDeviceProperties props {};
	VkPhysicalDeviceMemoryProperties mem {};
	VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gpl_props {};
	uint32_t max_push_descriptors = 0;
	PFN_vkCmdSetColorWriteEnableEXT              CmdSetColorWriteEnable             = nullptr;
	PFN_vkCmdSetAttachmentFeedbackLoopEnableEXT  CmdSetFeedbackLoop                 = nullptr;
	bool has_feedback_loop = false;
	bool null_descriptor   = false;
};
static Ctx g;

static bool HasExt(const std::vector<VkExtensionProperties>& v, const char* n) {
	for (auto& e: v) {
		if (std::strcmp(e.extensionName, n) == 0) return true;
	}
	return false;
}

static void InitVulkan(bool need_gpl) {
	VkApplicationInfo app {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_3;
	VkInstanceCreateInfo ici {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	VKCHK(vkCreateInstance(&ici, nullptr, &g.instance));
	uint32_t n = 0;
	vkEnumeratePhysicalDevices(g.instance, &n, nullptr);
	std::vector<VkPhysicalDevice> pds(n);
	vkEnumeratePhysicalDevices(g.instance, &n, pds.data());
	const char* want = std::getenv("GPLB_DEVICE");
	for (auto pd: pds) {
		VkPhysicalDeviceProperties p;
		vkGetPhysicalDeviceProperties(pd, &p);
		if (want ? std::strstr(p.deviceName, want) != nullptr : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
			g.phys = pd;
			break;
		}
	}
	if (!g.phys) g.phys = pds.at(0);
	vkGetPhysicalDeviceProperties(g.phys, &g.props);
	vkGetPhysicalDeviceMemoryProperties(g.phys, &g.mem);

	uint32_t qn = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qn, nullptr);
	std::vector<VkQueueFamilyProperties> qf(qn);
	vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qn, qf.data());
	for (uint32_t i = 0; i < qn; i++) {
		if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			g.qfam = i;
			break;
		}
	}

	uint32_t en = 0;
	vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &en, nullptr);
	std::vector<VkExtensionProperties> exts(en);
	vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &en, exts.data());

	std::vector<const char*> names = {VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME,
	                                  VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME,
	                                  VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
	                                  VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME,
	                                  VK_EXT_COLOR_WRITE_ENABLE_EXTENSION_NAME,
	                                  VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME};
	for (const char* e: {VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME,
	                     VK_EXT_MESH_SHADER_EXTENSION_NAME,
	                     VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME,
	                     VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME}) {
		if (HasExt(exts, e)) names.push_back(e);
	}
	g.has_feedback_loop = HasExt(exts, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME) &&
	                      HasExt(exts, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME);
	if (g.has_feedback_loop) {
		names.push_back(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME);
		names.push_back(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME);
	}
	if (need_gpl) {
		names.push_back(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
		names.push_back(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
	}

	// Query supported features (mirrors VulkanCreateDevice in vulkanWindow.cpp).
	VkPhysicalDeviceRobustness2FeaturesEXT s_rob {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
	VkPhysicalDeviceVulkan12Features       s12 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
	VkPhysicalDeviceVulkan13Features       s13 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	VkPhysicalDeviceMeshShaderFeaturesEXT  s_mesh {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
	VkPhysicalDeviceProvokingVertexFeaturesEXT s_pv {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
	VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR s_wg {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR};
	VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT s_fl {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT};
	VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT s_fld {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_FEATURES_EXT};
	VkPhysicalDeviceFeatures2 sf2 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	sf2.pNext     = &s12;
	s12.pNext     = &s13;
	s13.pNext     = &s_rob;
	s_rob.pNext   = &s_mesh;
	s_mesh.pNext  = &s_pv;
	s_pv.pNext    = &s_wg;
	s_wg.pNext    = &s_fl;
	s_fl.pNext    = &s_fld;
	vkGetPhysicalDeviceFeatures2(g.phys, &sf2);

	VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gplf {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
	gplf.graphicsPipelineLibrary = need_gpl ? VK_TRUE : VK_FALSE;
	VkPhysicalDeviceColorWriteEnableFeaturesEXT cwe {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COLOR_WRITE_ENABLE_FEATURES_EXT};
	cwe.colorWriteEnable = VK_TRUE;
	VkPhysicalDeviceDepthClipEnableFeaturesEXT dce {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
	dce.depthClipEnable = VK_TRUE;
	VkPhysicalDeviceDepthClipControlFeaturesEXT dcc {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
	dcc.depthClipControl = VK_TRUE;
	VkPhysicalDeviceImageViewMinLodFeaturesEXT ivml {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT};
	ivml.minLod = VK_TRUE;
	VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR frb {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
	frb.fragmentShaderBarycentric = VK_TRUE;
	VkPhysicalDeviceRobustness2FeaturesEXT rob {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
	rob.robustBufferAccess2 = s_rob.robustBufferAccess2;
	rob.robustImageAccess2  = s_rob.robustImageAccess2;
	rob.nullDescriptor      = s_rob.nullDescriptor;
	g.null_descriptor       = s_rob.nullDescriptor;
	VkPhysicalDeviceMeshShaderFeaturesEXT mesh {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
	mesh.meshShader = s_mesh.meshShader;
	VkPhysicalDeviceProvokingVertexFeaturesEXT pv {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
	pv.provokingVertexLast = s_pv.provokingVertexLast;
	VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR wg {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR};
	wg.workgroupMemoryExplicitLayout = s_wg.workgroupMemoryExplicitLayout;
	VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT fl {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT};
	fl.attachmentFeedbackLoopLayout = s_fl.attachmentFeedbackLoopLayout;
	VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT fld {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_FEATURES_EXT};
	fld.attachmentFeedbackLoopDynamicState = s_fld.attachmentFeedbackLoopDynamicState;
	VkPhysicalDeviceVulkan12Features f12 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
	f12.samplerMirrorClampToEdge  = VK_TRUE;
	f12.timelineSemaphore         = VK_TRUE;
	f12.shaderOutputLayer         = VK_TRUE;
	f12.shaderOutputViewportIndex = VK_TRUE;
	f12.bufferDeviceAddress       = VK_TRUE;
	f12.shaderBufferInt64Atomics  = VK_TRUE;
	f12.shaderSharedInt64Atomics  = s12.shaderSharedInt64Atomics;
	VkPhysicalDeviceVulkan13Features f13 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	f13.dynamicRendering   = VK_TRUE;
	f13.synchronization2   = VK_TRUE;
	f13.robustImageAccess  = s13.robustImageAccess;
	f13.subgroupSizeControl = s13.subgroupSizeControl;

	// chain
	std::vector<VkBaseOutStructure*> chain = {
	    reinterpret_cast<VkBaseOutStructure*>(&f12),  reinterpret_cast<VkBaseOutStructure*>(&f13),
	    reinterpret_cast<VkBaseOutStructure*>(&rob),  reinterpret_cast<VkBaseOutStructure*>(&cwe),
	    reinterpret_cast<VkBaseOutStructure*>(&dce),  reinterpret_cast<VkBaseOutStructure*>(&dcc),
	    reinterpret_cast<VkBaseOutStructure*>(&ivml), reinterpret_cast<VkBaseOutStructure*>(&frb),
	    reinterpret_cast<VkBaseOutStructure*>(&mesh), reinterpret_cast<VkBaseOutStructure*>(&pv),
	    reinterpret_cast<VkBaseOutStructure*>(&wg)};
	if (g.has_feedback_loop) {
		chain.push_back(reinterpret_cast<VkBaseOutStructure*>(&fl));
		chain.push_back(reinterpret_cast<VkBaseOutStructure*>(&fld));
	}
	if (need_gpl) chain.push_back(reinterpret_cast<VkBaseOutStructure*>(&gplf));
	for (size_t i = 0; i + 1 < chain.size(); i++) chain[i]->pNext = chain[i + 1];
	chain.back()->pNext = nullptr;

	VkPhysicalDeviceFeatures2 f2 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	f2.pNext = chain[0];
	auto& F  = f2.features;
	F.fragmentStoresAndAtomics = F.samplerAnisotropy = F.robustBufferAccess = VK_TRUE;
	F.depthBounds = F.depthClamp = VK_TRUE;
	F.shaderStorageImageWriteWithoutFormat = F.shaderImageGatherExtended = F.independentBlend = VK_TRUE;
	F.dualSrcBlend = F.tessellationShader = F.sampleRateShading = F.depthBiasClamp = VK_TRUE;
	F.shaderClipDistance = F.shaderCullDistance = F.largePoints = F.multiViewport = VK_TRUE;
	F.fillModeNonSolid = F.vertexPipelineStoresAndAtomics = F.shaderInt64 = VK_TRUE;
	F.shaderFloat64 = sf2.features.shaderFloat64;
	// also needed by some captured shaders; harmless for compile cost
	F.shaderStorageImageReadWithoutFormat = sf2.features.shaderStorageImageReadWithoutFormat;

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = g.qfam;
	qci.queueCount       = 1;
	qci.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.pNext                   = &f2;
	dci.queueCreateInfoCount    = 1;
	dci.pQueueCreateInfos       = &qci;
	dci.enabledExtensionCount   = static_cast<uint32_t>(names.size());
	dci.ppEnabledExtensionNames = names.data();
	VKCHK(vkCreateDevice(g.phys, &dci, nullptr, &g.dev));
	vkGetDeviceQueue(g.dev, g.qfam, 0, &g.queue);

	g.CmdSetColorWriteEnable = reinterpret_cast<PFN_vkCmdSetColorWriteEnableEXT>(vkGetDeviceProcAddr(g.dev, "vkCmdSetColorWriteEnableEXT"));
	g.CmdSetFeedbackLoop     = reinterpret_cast<PFN_vkCmdSetAttachmentFeedbackLoopEnableEXT>(vkGetDeviceProcAddr(g.dev, "vkCmdSetAttachmentFeedbackLoopEnableEXT"));

	VkPhysicalDevicePushDescriptorPropertiesKHR pd {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
	VkPhysicalDeviceProperties2 p2 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
	p2.pNext = &pd;
	g.gpl_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT;
	pd.pNext = &g.gpl_props;
	vkGetPhysicalDeviceProperties2(g.phys, &p2);
	g.max_push_descriptors = pd.maxPushDescriptors;
}

// ------------------------------------------------------------------------------------------------
// captured pipeline

struct BindingDesc {
	uint32_t binding, type, count, stages;
};

struct StageDesc {
	uint32_t       stage;
	uint64_t       shader_id;
	std::string    spv;
	VkShaderModule module = nullptr;
	std::vector<uint32_t> code;
};

struct Pipe {
	int         seq = 0;
	std::string file;
	bool        mesh = false, rect_list = false, tess = false;
	uint64_t    vs_id = 0, ps_id = 0;
	int64_t     game_driver_ns = 0;
	std::vector<StageDesc>   stages;
	std::vector<BindingDesc> bindings;
	bool        push_descriptors = false;
	uint32_t    push_stages = 0, push_offset = 0, push_size = 0;

	// vulkan state (stable addresses; the object is heap allocated and never moved)
	std::vector<VkVertexInputBindingDescription>   vib;
	std::vector<VkVertexInputAttributeDescription> via;
	bool has_vi = false, has_ia = false, has_tess = false, has_ds = false;
	VkPipelineVertexInputStateCreateInfo   vi {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	VkPipelineTessellationStateCreateInfo  ts {VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
	VkPipelineViewportDepthClipControlCreateInfoEXT clipctl {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT};
	VkPipelineViewportStateCreateInfo      vp {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	VkPipelineRasterizationDepthClipStateCreateInfoEXT clip {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT};
	VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pv {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT};
	VkPipelineRasterizationStateCreateInfo rs {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	VkPipelineMultisampleStateCreateInfo   ms {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	VkPipelineDepthStencilStateCreateInfo  ds {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	VkPipelineColorWriteCreateInfoEXT      cw {VK_STRUCTURE_TYPE_PIPELINE_COLOR_WRITE_CREATE_INFO_EXT};
	std::vector<VkBool32>                  cw_enable;
	bool has_cw = false;
	VkPipelineColorBlendStateCreateInfo    cb {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	std::vector<VkPipelineColorBlendAttachmentState> cba;
	std::vector<VkDynamicState>            dyn;
	VkPipelineDynamicStateCreateInfo       dynci {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	std::vector<VkFormat>                  color_formats;
	VkPipelineRenderingCreateInfo          rendering {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	std::vector<VkPipelineShaderStageCreateInfo> stage_ci; // all stages, same order as `stages`

	// created objects (owned by the layout cache)
	VkDescriptorSetLayout own_dsl = nullptr;
	VkPipelineLayout      own_layout = nullptr;

	const char* Class() const {
		return mesh ? "mesh" : rect_list ? "rectlist" : tess ? "tess" : "plain";
	}
};

static std::vector<uint32_t> ReadSpv(const fs::path& p) {
	std::ifstream f(p, std::ios::binary);
	if (!f) {
		std::fprintf(stderr, "cannot read %s\n", p.string().c_str());
		std::abort();
	}
	std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	std::vector<uint32_t> w(b.size() / 4);
	std::memcpy(w.data(), b.data(), w.size() * 4);
	return w;
}

static std::map<std::string, VkShaderModule> g_modules;

static VkShaderModule GetModule(const fs::path& dir, const std::string& spv) {
	auto it = g_modules.find(spv);
	if (it != g_modules.end()) return it->second;
	auto code = ReadSpv(dir / spv);
	VkShaderModuleCreateInfo ci {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	ci.codeSize = code.size() * 4;
	ci.pCode    = code.data();
	VkShaderModule m;
	VKCHK(vkCreateShaderModule(g.dev, &ci, nullptr, &m));
	g_modules[spv] = m;
	return m;
}

static std::unique_ptr<Pipe> LoadPipe(const fs::path& dir, const fs::path& file) {
	std::ifstream f(file);
	json          j = json::parse(f);
	auto          p = std::make_unique<Pipe>();
	p->seq          = j["seq"];
	p->file         = file.filename().string();
	p->mesh         = j["mesh"];
	p->rect_list    = j["rect_list"];
	p->tess         = j["tess"];
	p->vs_id        = j["vs_id"];
	p->ps_id        = j["ps_id"];
	p->game_driver_ns = j["game_driver_ns"];
	for (auto& s: j["stages"]) {
		StageDesc sd;
		sd.stage     = s["stage"];
		sd.shader_id = s["shader_id"];
		sd.spv       = s["spv"];
		sd.module    = GetModule(dir, sd.spv);
		p->stages.push_back(sd);
	}
	for (auto& b: j["layout"]["bindings"]) {
		p->bindings.push_back({b["binding"], b["type"], b["count"], b["stages"]});
	}
	p->push_descriptors = j["layout"]["push_descriptors"];
	p->push_stages      = j["layout"]["push_range"]["stages"];
	p->push_offset      = j["layout"]["push_range"]["offset"];
	p->push_size        = j["layout"]["push_range"]["size"];

	if (j.contains("vertex_input")) {
		p->has_vi = true;
		for (auto& b: j["vertex_input"]["bindings"]) {
			p->vib.push_back({b["binding"], b["stride"], static_cast<VkVertexInputRate>(b["rate"].get<int>())});
		}
		for (auto& a: j["vertex_input"]["attributes"]) {
			p->via.push_back({a["location"], a["binding"], static_cast<VkFormat>(a["format"].get<int>()), a["offset"]});
		}
		p->vi.vertexBindingDescriptionCount   = static_cast<uint32_t>(p->vib.size());
		p->vi.pVertexBindingDescriptions      = p->vib.data();
		p->vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(p->via.size());
		p->vi.pVertexAttributeDescriptions    = p->via.data();
	}
	if (j.contains("input_assembly")) {
		p->has_ia = true;
		p->ia.topology = static_cast<VkPrimitiveTopology>(j["input_assembly"]["topology"].get<int>());
		p->ia.primitiveRestartEnable = j["input_assembly"]["restart"];
	}
	if (j.contains("tess_patch_control_points")) {
		p->has_tess = true;
		p->ts.patchControlPoints = j["tess_patch_control_points"];
	}
	p->clipctl.negativeOneToOne = j["negative_one_to_one"].get<bool>() ? VK_TRUE : VK_FALSE;
	p->vp.pNext = &p->clipctl;
	{
		auto& r = j["raster"];
		void* tail = nullptr;
		p->rs.depthClampEnable = r["depth_clamp"].get<int>() ? VK_TRUE : VK_FALSE;
		if (r["has_depth_clip_ext"].get<bool>()) {
			p->clip.depthClipEnable = r["depth_clip"].get<bool>() ? VK_TRUE : VK_FALSE;
			tail = &p->clip;
		}
		if (r["provoking"].get<int>() >= 0) {
			p->pv.provokingVertexMode = r["provoking"].get<int>() ? VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT
			                                                         : VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT;
			p->pv.pNext = tail;
			tail = &p->pv;
		}
		p->rs.pNext       = tail;
		p->rs.cullMode    = r["cull"];
		p->rs.frontFace   = static_cast<VkFrontFace>(r["front_face"].get<int>());
		p->rs.polygonMode = static_cast<VkPolygonMode>(r["polygon_mode"].get<int>());
		p->rs.lineWidth   = r["line_width"];
	}
	{
		auto& m = j["multisample"];
		p->ms.rasterizationSamples = static_cast<VkSampleCountFlagBits>(m["samples"].get<int>());
		p->ms.sampleShadingEnable  = m["sample_shading"].get<int>() ? VK_TRUE : VK_FALSE;
		p->ms.minSampleShading     = m["min_sample_shading"];
	}
	if (j.contains("depth_stencil")) {
		p->has_ds = true;
		p->ds.depthBoundsTestEnable = j["depth_stencil"]["bounds_test"].get<int>() ? VK_TRUE : VK_FALSE;
		p->ds.minDepthBounds        = j["depth_stencil"]["min"];
		p->ds.maxDepthBounds        = j["depth_stencil"]["max"];
	}
	for (auto& a: j["blend"]["attachments"]) {
		VkPipelineColorBlendAttachmentState s {};
		s.colorWriteMask      = a["write_mask"];
		s.blendEnable         = a["enable"].get<int>() ? VK_TRUE : VK_FALSE;
		s.srcColorBlendFactor = static_cast<VkBlendFactor>(a["src_color"].get<int>());
		s.dstColorBlendFactor = static_cast<VkBlendFactor>(a["dst_color"].get<int>());
		s.colorBlendOp        = static_cast<VkBlendOp>(a["color_op"].get<int>());
		s.srcAlphaBlendFactor = static_cast<VkBlendFactor>(a["src_alpha"].get<int>());
		s.dstAlphaBlendFactor = static_cast<VkBlendFactor>(a["dst_alpha"].get<int>());
		s.alphaBlendOp        = static_cast<VkBlendOp>(a["alpha_op"].get<int>());
		p->cba.push_back(s);
	}
	p->cb.logicOp         = VK_LOGIC_OP_COPY;
	p->cb.attachmentCount = static_cast<uint32_t>(p->cba.size());
	p->cb.pAttachments    = p->cba.data();
	if (j["blend"]["color_write_ext"].get<bool>()) {
		p->has_cw = true;
		p->cw_enable.assign(p->cba.size(), VK_TRUE);
		p->cw.attachmentCount    = static_cast<uint32_t>(p->cba.size());
		p->cw.pColorWriteEnables = p->cw_enable.data();
		p->cb.pNext              = &p->cw;
	}
	for (auto& d: j["dynamic_states"]) p->dyn.push_back(static_cast<VkDynamicState>(d.get<int>()));
	p->dynci.dynamicStateCount = static_cast<uint32_t>(p->dyn.size());
	p->dynci.pDynamicStates    = p->dyn.data();
	for (auto& c: j["rendering"]["color_formats"]) p->color_formats.push_back(static_cast<VkFormat>(c.get<int>()));
	p->rendering.colorAttachmentCount    = static_cast<uint32_t>(p->color_formats.size());
	p->rendering.pColorAttachmentFormats = p->color_formats.data();
	p->rendering.depthAttachmentFormat   = static_cast<VkFormat>(j["rendering"]["depth"].get<int>());
	p->rendering.stencilAttachmentFormat = static_cast<VkFormat>(j["rendering"]["stencil"].get<int>());
	for (auto& s: p->stages) {
		VkPipelineShaderStageCreateInfo ci {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
		ci.stage  = static_cast<VkShaderStageFlagBits>(s.stage);
		ci.module = s.module;
		ci.pName  = "main";
		p->stage_ci.push_back(ci);
	}
	return p;
}

// ------------------------------------------------------------------------------------------------
// layouts

static VkDescriptorSetLayout MakeDsl(const std::vector<BindingDesc>& b, bool push, uint32_t stages_override) {
	std::vector<VkDescriptorSetLayoutBinding> v;
	for (auto& x: b) {
		VkDescriptorSetLayoutBinding s {};
		s.binding         = x.binding;
		s.descriptorType  = static_cast<VkDescriptorType>(x.type);
		s.descriptorCount = x.count;
		s.stageFlags      = stages_override ? stages_override : x.stages;
		v.push_back(s);
	}
	VkDescriptorSetLayoutCreateInfo ci {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	ci.flags        = push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0;
	ci.bindingCount = static_cast<uint32_t>(v.size());
	ci.pBindings    = v.data();
	VkDescriptorSetLayout l;
	VKCHK(vkCreateDescriptorSetLayout(g.dev, &ci, nullptr, &l));
	return l;
}

static VkPipelineLayout MakePl(VkDescriptorSetLayout dsl, uint32_t stages, uint32_t off, uint32_t size) {
	VkPushConstantRange pc {stages, off, size};
	VkPipelineLayoutCreateInfo ci {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	ci.setLayoutCount         = 1;
	ci.pSetLayouts            = &dsl;
	ci.pushConstantRangeCount = 1;
	ci.pPushConstantRanges    = &pc;
	VkPipelineLayout l;
	VKCHK(vkCreatePipelineLayout(g.dev, &ci, nullptr, &l));
	return l;
}

struct SuperLayout {
	std::vector<BindingDesc> bindings;
	VkDescriptorSetLayout    dsl    = nullptr;
	VkPipelineLayout         layout = nullptr;
	uint32_t                 total_descriptors = 0;
	std::map<uint32_t, uint32_t> per_type; // descriptor type -> count
};

constexpr uint32_t kAllGraphics = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                                  VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT |
                                  VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_MESH_BIT_EXT;

static SuperLayout BuildSuper(const std::vector<std::unique_ptr<Pipe>>& pipes) {
	SuperLayout s;
	std::map<uint32_t, BindingDesc> m;
	for (auto& p: pipes) {
		for (auto& b: p->bindings) {
			auto it = m.find(b.binding);
			if (it == m.end()) {
				m[b.binding] = b;
			} else {
				if (it->second.type != b.type) {
					std::fprintf(stderr, "binding %u has two descriptor types (%u, %u)\n", b.binding, it->second.type, b.type);
					std::abort();
				}
				it->second.count = std::max(it->second.count, b.count);
			}
		}
	}
	for (auto& [k, v]: m) {
		v.stages = kAllGraphics;
		s.bindings.push_back(v);
		s.total_descriptors += v.count;
		s.per_type[v.type] += v.count;
	}
	s.dsl    = MakeDsl(s.bindings, false, 0);
	s.layout = MakePl(s.dsl, kAllGraphics, 0, 128);
	return s;
}

static void EnsureOwnLayout(Pipe& p) {
	if (p.own_layout) return;
	p.own_dsl    = MakeDsl(p.bindings, p.push_descriptors, 0);
	p.own_layout = MakePl(p.own_dsl, p.push_stages, p.push_offset, p.push_size);
}

// ------------------------------------------------------------------------------------------------
// create-info builders

enum LibPart { kVI = 0, kPRE = 1, kFS = 2, kFO = 3 };

static bool g_dup_layout = false; // --dup-layout: every library gets its own identically-defined layout object
static VkPipelineCreateFlags g_extra_flags = 0; // --disable-opt: VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT on everything

static VkGraphicsPipelineCreateInfo BuildMono(Pipe& p, VkPipelineLayout layout) {
	VkGraphicsPipelineCreateInfo ci {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	ci.flags               = g_extra_flags;
	ci.pNext               = &p.rendering;
	ci.stageCount          = static_cast<uint32_t>(p.stage_ci.size());
	ci.pStages             = p.stage_ci.data();
	ci.pVertexInputState   = p.has_vi ? &p.vi : nullptr;
	ci.pInputAssemblyState = p.has_ia ? &p.ia : nullptr;
	ci.pTessellationState  = p.has_tess ? &p.ts : nullptr;
	ci.pViewportState      = &p.vp;
	ci.pRasterizationState = &p.rs;
	ci.pMultisampleState   = &p.ms;
	ci.pDepthStencilState  = p.has_ds ? &p.ds : nullptr;
	ci.pColorBlendState    = &p.cb;
	ci.pDynamicState       = &p.dynci;
	ci.layout              = layout;
	ci.basePipelineIndex   = -1;
	return ci;
}

struct LibCI {
	VkGraphicsPipelineLibraryCreateInfoEXT lib {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
	VkGraphicsPipelineCreateInfo           ci {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	std::vector<VkPipelineShaderStageCreateInfo> stages;
};

static void BuildLib(Pipe& p, LibPart part, VkPipelineLayout layout, bool retain, LibCI& out) {
	out.stages.clear();
	auto& ci = out.ci;
	ci       = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	ci.flags = g_extra_flags | VK_PIPELINE_CREATE_LIBRARY_BIT_KHR |
	           (retain ? VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT : 0);
	out.lib.pNext = &p.rendering; // dynamic rendering info (viewMask / formats)
	ci.pNext      = &out.lib;
	ci.basePipelineIndex = -1;
	ci.pDynamicState     = &p.dynci;
	switch (part) {
		case kVI:
			out.lib.flags          = VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT;
			// mesh pipelines have no vertex input state in the monolithic create-info, but the
			// vertex-input-interface library itself must be given (empty) state or the driver crashes.
			static const VkPipelineVertexInputStateCreateInfo   kEmptyVi {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
			static const VkPipelineInputAssemblyStateCreateInfo kEmptyIa {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
			ci.pVertexInputState   = p.has_vi ? &p.vi : &kEmptyVi;
			ci.pInputAssemblyState = p.has_ia ? &p.ia : &kEmptyIa;
			break;
		case kPRE:
			out.lib.flags = VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT;
			for (size_t i = 0; i < p.stages.size(); i++) {
				if (p.stages[i].stage != VK_SHADER_STAGE_FRAGMENT_BIT) out.stages.push_back(p.stage_ci[i]);
			}
			ci.stageCount          = static_cast<uint32_t>(out.stages.size());
			ci.pStages             = out.stages.data();
			ci.pTessellationState  = p.has_tess ? &p.ts : nullptr;
			ci.pViewportState      = &p.vp;
			ci.pRasterizationState = &p.rs;
			ci.layout              = layout;
			break;
		case kFS:
			out.lib.flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;
			for (size_t i = 0; i < p.stages.size(); i++) {
				if (p.stages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT) out.stages.push_back(p.stage_ci[i]);
			}
			ci.stageCount         = static_cast<uint32_t>(out.stages.size());
			ci.pStages            = out.stages.empty() ? nullptr : out.stages.data();
			ci.pMultisampleState  = &p.ms;
			ci.pDepthStencilState = p.has_ds ? &p.ds : nullptr;
			ci.layout             = layout;
			break;
		case kFO:
			out.lib.flags        = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
			ci.pColorBlendState  = &p.cb;
			ci.pMultisampleState = &p.ms;
			break;
	}
}

static VkGraphicsPipelineCreateInfo BuildLink(const VkPipeline libs[4], VkPipelineLibraryCreateInfoKHR& li,
                                              VkPipelineLayout layout, bool lto) {
	li = {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
	li.libraryCount = 4;
	li.pLibraries   = libs;
	VkGraphicsPipelineCreateInfo ci {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	ci.pNext  = &li;
	ci.flags  = g_extra_flags | (lto ? VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT : 0);
	ci.layout = layout;
	ci.basePipelineIndex = -1;
	return ci;
}

static VkPipelineCache MakeCache() {
	VkPipelineCacheCreateInfo ci {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
	VkPipelineCache c;
	VKCHK(vkCreatePipelineCache(g.dev, &ci, nullptr, &c));
	return c;
}

// One timed driver call on the *calling* thread. Returns ms; result code in `res`.
static double TimedCreate(VkPipelineCache cache, const VkGraphicsPipelineCreateInfo& ci, VkPipeline& out, VkResult& res) {
	auto t0 = Clock::now();
	res     = vkCreateGraphicsPipelines(g.dev, cache, 1, &ci, nullptr, &out);
	return MsSince(t0);
}

// Run `f` on a fresh worker thread and wait (the measurement thread is never the main thread).
static void OnWorker(const std::function<void()>& f) {
	std::thread t(f);
	t.join();
}

// ------------------------------------------------------------------------------------------------
// measurement records

struct Row {
	int         seq;
	std::string cls;
	std::string variant, layout;
	double mono = -1, mono_warm = -1;
	double vi = -1, pre = -1, fs = -1, fo = -1, fast = -1, opt = -1;
	double vi_w = -1, pre_w = -1, fs_w = -1, fo_w = -1, fast_w = -1, opt_w = -1;
	double e2e4 = -1, e2e2 = -1;
	std::string status = "ok";
};

static void PrintCsvHeader(FILE* f) {
	std::fprintf(f, "seq,class,variant,layout,mono_ms,mono_warm_ms,vi_ms,pre_ms,fs_ms,fo_ms,fast_ms,opt_ms,vi_warm_ms,pre_warm_ms,fs_warm_ms,fo_warm_ms,fast_warm_ms,opt_warm_ms,e2e4_ms,e2e2_ms,status\n");
}
static void PrintCsvRow(FILE* f, const Row& r) {
	std::fprintf(f, "%d,%s,%s,%s,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%s\n",
	             r.seq, r.cls.c_str(), r.variant.c_str(), r.layout.c_str(), r.mono, r.mono_warm, r.vi, r.pre, r.fs, r.fo,
	             r.fast, r.opt, r.vi_w, r.pre_w, r.fs_w, r.fo_w, r.fast_w, r.opt_w, r.e2e4, r.e2e2, r.status.c_str());
}

struct Opts {
	std::string dump, variant = "mono", layout = "own", csv;
	bool        warm = false, retain = true;
	int         max_n = 0, par = 0;
	std::string only, order = "forward";
	uint32_t    seed = 1;
	bool        include_mesh = false, include_rect = true;
};

// ---- variants ----

static void RunMono(Pipe& p, VkPipelineLayout layout, bool warm, Row& r) {
	OnWorker([&] {
		VkPipelineCache cache = warm ? MakeCache() : VK_NULL_HANDLE;
		VkPipeline      pl    = nullptr;
		VkResult        res;
		auto            ci = BuildMono(p, layout);
		r.mono             = TimedCreate(cache, ci, pl, res);
		if (res != VK_SUCCESS) {
			r.status = "mono_fail_" + std::to_string(res);
			return;
		}
		if (warm) {
			vkDestroyPipeline(g.dev, pl, nullptr);
			pl = nullptr;
			r.mono_warm = TimedCreate(cache, ci, pl, res);
			vkDestroyPipelineCache(g.dev, cache, nullptr);
		}
		vkDestroyPipeline(g.dev, pl, nullptr);
	});
}

struct GplResult {
	VkPipeline libs[4] {};
	double     t[4] {};
	VkResult   res[4] {};
};

static void BuildLibs(Pipe& p, VkPipelineLayout layout, bool retain, VkPipelineCache cache, GplResult& out, double* tout) {
	for (int part = 0; part < 4; part++) {
		LibCI l;
		VkPipelineLayout lay = layout;
		if (g_dup_layout && (part == kPRE || part == kFS)) {
			lay = MakePl(MakeDsl(p.bindings, p.push_descriptors, 0), p.push_stages, p.push_offset, p.push_size);
		}
		BuildLib(p, static_cast<LibPart>(part), lay, retain, l);
		tout[part] = TimedCreate(cache, l.ci, out.libs[part], out.res[part]);
	}
}

static void RunGpl(Pipe& p, VkPipelineLayout layout, bool warm, bool retain, Row& r) {
	OnWorker([&] {
		VkPipelineCache cache = warm ? MakeCache() : VK_NULL_HANDLE;
		GplResult       gr;
		double          t[4];
		BuildLibs(p, layout, retain, cache, gr, t);
		for (int i = 0; i < 4; i++) {
			if (gr.res[i] != VK_SUCCESS) {
				r.status = "lib" + std::to_string(i) + "_fail_" + std::to_string(gr.res[i]);
				return;
			}
		}
		r.vi = t[0]; r.pre = t[1]; r.fs = t[2]; r.fo = t[3];
		VkPipelineLibraryCreateInfoKHR li;
		VkPipeline fast = nullptr, opt = nullptr;
		VkResult   res;
		auto       cif = BuildLink(gr.libs, li, layout, false);
		r.fast         = TimedCreate(cache, cif, fast, res);
		if (res != VK_SUCCESS) { r.status = "fast_fail_" + std::to_string(res); return; }
		auto cio = BuildLink(gr.libs, li, layout, true);
		r.opt    = TimedCreate(cache, cio, opt, res);
		if (res != VK_SUCCESS) { r.status = "opt_fail_" + std::to_string(res); return; }
		if (warm) {
			// second pass with the same cache -> warm numbers
			GplResult gw;
			double    tw[4];
			BuildLibs(p, layout, retain, cache, gw, tw);
			r.vi_w = tw[0]; r.pre_w = tw[1]; r.fs_w = tw[2]; r.fo_w = tw[3];
			VkPipeline f2 = nullptr, o2 = nullptr;
			auto       a = BuildLink(gw.libs, li, layout, false);
			r.fast_w     = TimedCreate(cache, a, f2, res);
			auto       b = BuildLink(gw.libs, li, layout, true);
			r.opt_w      = TimedCreate(cache, b, o2, res);
			vkDestroyPipeline(g.dev, f2, nullptr);
			vkDestroyPipeline(g.dev, o2, nullptr);
			for (auto l: gw.libs) vkDestroyPipeline(g.dev, l, nullptr);
			vkDestroyPipelineCache(g.dev, cache, nullptr);
		}
		vkDestroyPipeline(g.dev, fast, nullptr);
		vkDestroyPipeline(g.dev, opt, nullptr);
		for (auto l: gr.libs) vkDestroyPipeline(g.dev, l, nullptr);
	});
}

// Four libraries on 4 concurrent workers (or two workers: {PRE,VI} and {FS,FO}), then fast link.
// Returns wall-clock ms from "start" to "usable fast-linked pipeline".
static double RunE2E(Pipe& p, VkPipelineLayout layout, bool retain, int workers, VkResult& status) {
	GplResult gr;
	double    t[4];
	auto      t0 = Clock::now();
	std::vector<std::thread> th;
	auto build = [&](int part) {
		LibCI l;
		BuildLib(p, static_cast<LibPart>(part), layout, retain, l);
		t[part] = TimedCreate(VK_NULL_HANDLE, l.ci, gr.libs[part], gr.res[part]);
	};
	if (workers >= 4) {
		for (int i = 0; i < 4; i++) th.emplace_back(build, i);
	} else {
		th.emplace_back([&] { build(kPRE); build(kVI); });
		th.emplace_back([&] { build(kFS); build(kFO); });
	}
	for (auto& x: th) x.join();
	status = VK_SUCCESS;
	for (auto rr: gr.res) if (rr != VK_SUCCESS) status = rr;
	double ms = -1;
	if (status == VK_SUCCESS) {
		VkPipelineLibraryCreateInfoKHR li;
		VkPipeline                    fast = nullptr;
		VkResult                      res;
		auto                          cif = BuildLink(gr.libs, li, layout, false);
		TimedCreate(VK_NULL_HANDLE, cif, fast, res);
		ms = MsSince(t0);
		status = res;
		vkDestroyPipeline(g.dev, fast, nullptr);
	}
	for (auto l: gr.libs) vkDestroyPipeline(g.dev, l, nullptr);
	return ms;
}

// Four libraries one after another + fast link, all on the calling thread (one pool worker).
static void RunFastSeq(Pipe& p, VkPipelineLayout layout, bool retain) {
	GplResult gr;
	double    t[4];
	BuildLibs(p, layout, retain, VK_NULL_HANDLE, gr, t);
	VkPipelineLibraryCreateInfoKHR li;
	VkPipeline                    fast = nullptr;
	VkResult                      res;
	auto                          cif = BuildLink(gr.libs, li, layout, false);
	TimedCreate(VK_NULL_HANDLE, cif, fast, res);
	vkDestroyPipeline(g.dev, fast, nullptr);
	for (auto l: gr.libs) vkDestroyPipeline(g.dev, l, nullptr);
}

// ------------------------------------------------------------------------------------------------

#include "diff.inc"

int main(int argc, char** argv) {
	Opts o;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto        next = [&]() -> std::string { return argv[++i]; };
		if (a == "--dump") o.dump = next();
		else if (a == "--variant") o.variant = next();
		else if (a == "--layout") o.layout = next();
		else if (a == "--csv") o.csv = next();
		else if (a == "--warm") o.warm = true;
		else if (a == "--no-retain") o.retain = false;
		else if (a == "--max") o.max_n = std::atoi(next().c_str());
		else if (a == "--par") o.par = std::atoi(next().c_str());
		else if (a == "--only") o.only = next();
		else if (a == "--order") o.order = next();
		else if (a == "--seed") o.seed = static_cast<uint32_t>(std::atoi(next().c_str()));
		else if (a == "--include-mesh") o.include_mesh = true;
		else if (a == "--no-rect") o.include_rect = false;
		else if (a == "--dup-layout") g_dup_layout = true;
		else if (a == "--disable-opt") g_extra_flags = VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
		else { std::fprintf(stderr, "unknown arg %s\n", a.c_str()); return 2; }
	}
	if (o.dump.empty()) { std::fprintf(stderr, "usage: gpl_bench --dump <dir> --variant mono|gpl|e2e|par-mono|par-gpl|diff|info [--layout own|super] [--warm] [--csv f]\n"); return 2; }

	const bool need_gpl = o.variant != "mono" && o.variant != "par-mono" && o.variant != "desc";
	InitVulkan(need_gpl || o.variant == "info");
	std::printf("device: %s driver=0x%x api=%u.%u.%u\n", g.props.deviceName, g.props.driverVersion,
	            VK_API_VERSION_MAJOR(g.props.apiVersion), VK_API_VERSION_MINOR(g.props.apiVersion), VK_API_VERSION_PATCH(g.props.apiVersion));
	std::printf("graphicsPipelineLibraryFastLinking=%u graphicsPipelineLibraryIndependentInterpolationDecoration=%u maxPushDescriptors=%u\n",
	            g.gpl_props.graphicsPipelineLibraryFastLinking, g.gpl_props.graphicsPipelineLibraryIndependentInterpolationDecoration,
	            g.max_push_descriptors);
	std::printf("GL_SHADER_DISK_CACHE=%s\n", std::getenv("__GL_SHADER_DISK_CACHE") ? std::getenv("__GL_SHADER_DISK_CACHE") : "(unset)");
	if (o.variant == "info") return 0;

	// load pipelines
	std::vector<fs::path> files;
	for (auto& e: fs::directory_iterator(fs::path(o.dump) / "pipelines")) files.push_back(e.path());
	std::sort(files.begin(), files.end());
	std::vector<std::unique_ptr<Pipe>> all;
	for (auto& f: files) all.push_back(LoadPipe(o.dump, f));
	std::printf("loaded %zu graphics pipelines\n", all.size());

	SuperLayout super = BuildSuper(all);
	std::printf("superset layout: %zu bindings, %u descriptors total (maxPushDescriptors=%u)", super.bindings.size(),
	            super.total_descriptors, g.max_push_descriptors);
	for (auto& [t, c]: super.per_type) std::printf(" type%u=%u", t, c);
	std::printf("\n");

	// sample selection
	std::set<int> only;
	if (!o.only.empty()) {
		std::stringstream ss(o.only);
		std::string       tok;
		while (std::getline(ss, tok, ',')) only.insert(std::atoi(tok.c_str()));
	}
	std::vector<Pipe*> sel;
	for (auto& p: all) {
		if (!only.empty() && !only.count(p->seq)) continue;
		if (p->mesh && !o.include_mesh) continue;
		if (p->rect_list && !o.include_rect) continue;
		sel.push_back(p.get());
	}
	if (o.order == "reverse") std::reverse(sel.begin(), sel.end());
	else if (o.order == "random") { std::mt19937 rng(o.seed); std::shuffle(sel.begin(), sel.end(), rng); }
	if (o.max_n > 0 && static_cast<int>(sel.size()) > o.max_n) sel.resize(o.max_n);
	std::printf("selected %zu pipelines (variant=%s layout=%s warm=%d retain=%d order=%s)\n", sel.size(), o.variant.c_str(),
	            o.layout.c_str(), o.warm, o.retain, o.order.c_str());

	auto layout_for = [&](Pipe& p) -> VkPipelineLayout {
		if (o.layout == "super") return super.layout;
		EnsureOwnLayout(p);
		return p.own_layout;
	};

	if (o.variant == "diff") return RunDiff(o, sel, super);
	if (o.variant == "desc") return RunDescBench(o, sel, super);

	FILE* csv = o.csv.empty() ? nullptr : std::fopen(o.csv.c_str(), "w");
	if (csv) PrintCsvHeader(csv);

	if (o.variant == "par-mono" || o.variant == "par-gpl") {
		// Makespan of the whole selection on N concurrent workers. Fast-link path for gpl:
		// 4 libs + fast link per pipeline (the optimized link is not part of the critical path).
		std::atomic<size_t> next {0};
		auto                t0 = Clock::now();
		std::vector<std::thread> th;
		std::vector<double> per(sel.size(), -1);
		for (int w = 0; w < std::max(1, o.par); w++) {
			th.emplace_back([&] {
				for (;;) {
					size_t i = next.fetch_add(1);
					if (i >= sel.size()) return;
					Row r;
					auto ta = Clock::now();
					if (o.variant == "par-mono") {
						auto ci = BuildMono(*sel[i], layout_for(*sel[i]));
						VkPipeline pl; VkResult res;
						TimedCreate(VK_NULL_HANDLE, ci, pl, res);
						if (res == VK_SUCCESS) vkDestroyPipeline(g.dev, pl, nullptr);
					} else {
						RunFastSeq(*sel[i], layout_for(*sel[i]), o.retain);
					}
					per[i] = MsSince(ta);
				}
			});
		}
		for (auto& t: th) t.join();
		std::printf("PAR variant=%s workers=%d pipelines=%zu makespan_ms=%.1f sum_ms=%.1f\n", o.variant.c_str(), o.par,
		            sel.size(), MsSince(t0), std::accumulate(per.begin(), per.end(), 0.0));
		return 0;
	}

	// per-pipeline sequential measurement, each on a worker thread
	std::vector<Row> rows;
	for (auto* p: sel) {
		Row r;
		r.seq     = p->seq;
		r.cls     = p->Class();
		r.variant = o.variant;
		r.layout  = o.layout;
		auto layout = layout_for(*p);
		if (o.variant == "mono") {
			RunMono(*p, layout, o.warm, r);
		} else if (o.variant == "gpl") {
			RunGpl(*p, layout, o.warm, o.retain, r);
		} else if (o.variant == "e2e") {
			VkResult st;
			r.e2e4 = RunE2E(*p, layout, o.retain, 4, st);
			if (st != VK_SUCCESS) r.status = "e2e4_fail_" + std::to_string(st);
			r.e2e2 = RunE2E(*p, layout, o.retain, 2, st);
		} else {
			std::fprintf(stderr, "unknown variant %s\n", o.variant.c_str());
			return 2;
		}
		std::printf("seq %3d %-8s mono=%8.2f vi=%6.2f pre=%8.2f fs=%8.2f fo=%6.2f fast=%7.2f opt=%8.2f e2e4=%8.2f %s\n", r.seq, r.cls.c_str(),
		            r.mono, r.vi, r.pre, r.fs, r.fo, r.fast, r.opt, r.e2e4, r.status.c_str());
		std::fflush(stdout);
		if (csv) PrintCsvRow(csv, r);
		rows.push_back(r);
	}
	if (csv) std::fclose(csv);
	return 0;
}
