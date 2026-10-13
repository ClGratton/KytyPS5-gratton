// spirv-compile-bench: offline timing of driver shader compilation for dumped SPIR-V.
//
// For every .spv given on the command line it derives a minimal matching pipeline layout by
// reflecting the module (descriptor decorations, push constants, entry point), creates a compute
// pipeline with the pipeline cache disabled, repeats that N times and reports the median wall time
// of vkCreateComputePipelines.  It can also time the same SPIR-V through VK_EXT_shader_object, run
// several compiles on separate threads, time a graphics VS/PS pair, and execute a small shader to
// compare results of rewritten variants (see docs/spirv-compile-bench.md).
//
// Output is one JSON object per line on stdout; diagnostics go to stderr.
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

static void die(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(stderr, fmt, ap);
	va_end(ap);
	std::fputc('\n', stderr);
	std::exit(2);
}

#define VK_OK(expr)                                                                          \
	do {                                                                                     \
		VkResult r_ = (expr);                                                                \
		if (r_ != VK_SUCCESS) die("%s:%d: %s failed: VkResult %d", __FILE__, __LINE__, #expr, r_); \
	} while (0)

// ---- options ---------------------------------------------------------------------------------------
struct Options {
	std::vector<std::string> files;
	std::string mode          = "pipeline"; // pipeline | shader-object | both
	int         reps          = 5;
	int         warmup        = 1;
	int         parallel      = 0;          // >0: that many concurrent compiles per rep
	bool        distinct      = false;      // parallel: cycle through the files instead of repeating file 0
	std::string robustness    = "emu";      // emu | none | rb1 | rb2
	int         subgroup      = 32;         // required subgroup size, 0 = do not request one
	bool        push_desc     = true;
	bool        uniq          = true;       // make every compile's SPIR-V binary unique (defeats caches)
	bool        keep_nv_cache = false;
	bool        gfx           = false;      // files are vs.spv ps.spv [vs.spv ps.spv ...]
	bool        exec          = false;      // run the shader once and dump its storage buffers
	std::string gfx_seq;                    // --gfx-seq a,b:v1p0,...: graphics pipeline state steps (see BenchGraphicsStates)
	std::map<uint32_t, uint32_t> ranges;    // exec: binding -> descriptor range in bytes
	int         gpu           = 0;
	bool        list_only     = false;
};

// ---- SPIR-V reflection -------------------------------------------------------------------------------
struct SpvInst {
	uint32_t                op;
	std::vector<uint32_t>   ops;
};

struct Binding {
	uint32_t           set = 0, binding = 0, count = 1;
	VkDescriptorType   type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	VkShaderStageFlags stages = 0;
};

struct VarInfo {
	uint32_t location;
	uint32_t type_id;
};

struct Reflection {
	std::vector<Binding>  bindings;
	bool                  has_push = false;
	uint32_t              local_size[3] {0, 0, 0};
	VkShaderStageFlagBits stage = VK_SHADER_STAGE_COMPUTE_BIT;
	std::vector<VarInfo>  inputs, outputs;
	bool                  writes_frag_depth = false;
	uint32_t              words = 0, blocks = 0;
	std::map<uint32_t, SpvInst> by_id; // result id -> instruction (types, constants, variables)
};

static std::vector<uint32_t> ReadSpirv(const std::string& path) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) die("cannot open %s", path.c_str());
	std::fseek(f, 0, SEEK_END);
	long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	if (size < 20 || size % 4) die("%s: not a SPIR-V binary", path.c_str());
	std::vector<uint32_t> w(static_cast<size_t>(size) / 4);
	if (std::fread(w.data(), 1, static_cast<size_t>(size), f) != static_cast<size_t>(size)) die("short read %s", path.c_str());
	std::fclose(f);
	if (w[0] != 0x07230203u) die("%s: bad magic", path.c_str());
	return w;
}

static Reflection Reflect(const std::vector<uint32_t>& w) {
	Reflection r;
	r.words = static_cast<uint32_t>(w.size());
	struct Deco { uint32_t set = 0, binding = 0, location = ~0u, builtin = ~0u; bool has_set = false, has_binding = false, block = false, bufblock = false; };
	std::map<uint32_t, Deco> deco;
	std::vector<uint32_t>    variables;
	for (size_t i = 5; i < w.size();) {
		uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
		if (wc == 0 || i + wc > w.size()) die("malformed SPIR-V");
		SpvInst inst {op, std::vector<uint32_t>(w.begin() + static_cast<long>(i) + 1, w.begin() + static_cast<long>(i + wc))};
		i += wc;
		switch (op) {
			case 248: r.blocks++; break; // OpLabel
			case 15: // OpEntryPoint: execution model
				switch (inst.ops[0]) {
					case 0: r.stage = VK_SHADER_STAGE_VERTEX_BIT; break;
					case 4: r.stage = VK_SHADER_STAGE_FRAGMENT_BIT; break;
					case 5: r.stage = VK_SHADER_STAGE_COMPUTE_BIT; break;
					case 1: r.stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT; break;
					case 2: r.stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT; break;
					case 3: r.stage = VK_SHADER_STAGE_GEOMETRY_BIT; break;
					case 5267: r.stage = VK_SHADER_STAGE_TASK_BIT_EXT; break;
					case 5268: r.stage = VK_SHADER_STAGE_MESH_BIT_EXT; break;
					default: die("unsupported execution model %u", inst.ops[0]);
				}
				break;
			case 16: // OpExecutionMode
				if (inst.ops.size() >= 5 && inst.ops[1] == 17) { for (int k = 0; k < 3; k++) r.local_size[k] = inst.ops[2 + static_cast<size_t>(k)]; }
				break;
			case 71: { // OpDecorate
				auto& d = deco[inst.ops[0]];
				switch (inst.ops[1]) {
					case 2: d.block = true; break;
					case 3: d.bufblock = true; break;
					case 11: d.builtin = inst.ops[2]; break;
					case 30: d.location = inst.ops[2]; break;
					case 33: d.binding = inst.ops[2]; d.has_binding = true; break;
					case 34: d.set = inst.ops[2]; d.has_set = true; break;
					default: break;
				}
				break;
			}
			default: break;
		}
		// types / constants / variables: result id position differs (OpType* put it first)
		bool type_like = (op >= 19 && op <= 39) || op == 5341 || op == 5342;
		if (type_like) r.by_id[inst.ops[0]] = inst;
		else if (op == 43 || op == 44 || op == 45) r.by_id[inst.ops[1]] = inst; // constants: [type, id, ...]
		else if (op == 59) { r.by_id[inst.ops[1]] = inst; variables.push_back(inst.ops[1]); }
	}
	auto strip = [&](uint32_t type_id, uint32_t& count, bool& runtime) -> const SpvInst* {
		const SpvInst* t = &r.by_id.at(type_id);
		count = 1;
		runtime = false;
		while (t->op == 28 || t->op == 29) { // OpTypeArray / OpTypeRuntimeArray
			if (t->op == 28) {
				const auto& c = r.by_id.at(t->ops[2]);
				count *= c.ops[2];
			} else {
				runtime = true;
			}
			t = &r.by_id.at(t->ops[1]);
		}
		return t;
	};
	for (uint32_t v : variables) {
		const auto& var = r.by_id.at(v);
		uint32_t    sc  = var.ops[2];
		const auto& ptr = r.by_id.at(var.ops[0]);
		uint32_t    pointee = ptr.ops[2];
		const auto  dit = deco.find(v);
		if (sc == 9) { r.has_push = true; continue; } // PushConstant
		if (sc == 1 || sc == 3) { // Input / Output
			if (dit != deco.end() && dit->second.builtin != ~0u) {
				if (sc == 3 && dit->second.builtin == 22) r.writes_frag_depth = true; // FragDepth
				continue;
			}
			if (dit != deco.end() && dit->second.location != ~0u) (sc == 1 ? r.inputs : r.outputs).push_back({dit->second.location, pointee});
			continue;
		}
		if (sc != 0 && sc != 2 && sc != 12) continue; // UniformConstant, Uniform, StorageBuffer
		if (dit == deco.end() || !dit->second.has_binding) continue;
		uint32_t count; bool runtime;
		const SpvInst* t = strip(pointee, count, runtime);
		Binding b;
		b.set = dit->second.set; b.binding = dit->second.binding; b.count = runtime ? 16 : count;
		if (t->op == 30) { // struct
			bool block = false, bufblock = false;
			auto sd = deco.find(t->ops[0]);
			if (sd != deco.end()) { block = sd->second.block; bufblock = sd->second.bufblock; }
			b.type = (sc == 12 || bufblock) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			(void)block;
		} else if (t->op == 25) { // image: [id, sampled type, dim, depth, arrayed, ms, sampled, format]
			uint32_t dim = t->ops[2], sampled = t->ops[6];
			if (dim == 6) b.type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
			else if (sampled == 2) b.type = dim == 5 ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			else b.type = dim == 5 ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		} else if (t->op == 26) {
			b.type = VK_DESCRIPTOR_TYPE_SAMPLER;
		} else if (t->op == 27) {
			b.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		} else {
			die("binding %u: unsupported descriptor type (opcode %u)", b.binding, t->op);
		}
		r.bindings.push_back(b);
	}
	std::sort(r.bindings.begin(), r.bindings.end(), [](const Binding& a, const Binding& b) { return a.set != b.set ? a.set < b.set : a.binding < b.binding; });
	return r;
}

// Make the binary unique without changing what the driver compiles: OpModuleProcessed "<tag>" is a
// debug instruction that sits after the OpName block and before the annotations.
static std::vector<uint32_t> Uniquify(const std::vector<uint32_t>& w, uint64_t tag) {
	size_t i = 5, at = 0;
	while (i < w.size()) {
		uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
		bool debug = op == 2 || op == 3 || op == 4 || op == 5 || op == 6 || op == 7 || op == 330;
		bool early = op <= 16 || op == 17; // capabilities .. execution modes (before the debug block)
		if (!debug && !early) { at = i; break; }
		i += wc;
	}
	if (at == 0) return w;
	char text[40];
	std::snprintf(text, sizeof text, "uniq-%016" PRIx64, tag);
	size_t nwords = (std::strlen(text) + 1 + 3) / 4;
	std::vector<uint32_t> ins(1 + nwords, 0);
	ins[0] = static_cast<uint32_t>((1 + nwords) << 16) | 330u;
	std::memcpy(&ins[1], text, std::strlen(text) + 1);
	std::vector<uint32_t> out(w.begin(), w.begin() + static_cast<long>(at));
	out.insert(out.end(), ins.begin(), ins.end());
	out.insert(out.end(), w.begin() + static_cast<long>(at), w.end());
	return out;
}

// ---- Vulkan device -----------------------------------------------------------------------------------
struct Ctx {
	VkInstance       instance = nullptr;
	VkPhysicalDevice phys     = nullptr;
	VkDevice         device   = nullptr;
	uint32_t         queue_family = 0;
	VkQueue          queue    = nullptr;
	char             name[256] {};
	uint32_t         driver_version = 0;
	bool             push_desc = false, shader_object = false, subgroup_control = false, robustness2 = false;
	uint32_t         min_subgroup = 0, max_subgroup = 0;
	VkDeviceSize     robust_storage_align = 0;
	VkPhysicalDeviceMemoryProperties mem {};
	PFN_vkCreateShadersEXT        CreateShaders  = nullptr;
	PFN_vkDestroyShaderEXT        DestroyShader  = nullptr;
	PFN_vkCmdBindShadersEXT       CmdBindShaders = nullptr;
	PFN_vkCmdPushDescriptorSetKHR CmdPush        = nullptr;
};

static bool HasExt(const std::vector<VkExtensionProperties>& v, const char* n) {
	for (auto& e : v) if (!std::strcmp(e.extensionName, n)) return true;
	return false;
}

static Ctx MakeContext(const Options& opt) {
	Ctx c;
	VkApplicationInfo app {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "spirv-compile-bench";
	app.apiVersion       = VK_API_VERSION_1_3;
	VkInstanceCreateInfo ici {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	VK_OK(vkCreateInstance(&ici, nullptr, &c.instance));
	uint32_t n = 0;
	VK_OK(vkEnumeratePhysicalDevices(c.instance, &n, nullptr));
	std::vector<VkPhysicalDevice> devs(n);
	VK_OK(vkEnumeratePhysicalDevices(c.instance, &n, devs.data()));
	if (opt.list_only || static_cast<uint32_t>(opt.gpu) >= n) {
		for (uint32_t i = 0; i < n; i++) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(devs[i], &p); std::fprintf(stderr, "gpu %u: %s\n", i, p.deviceName); }
		if (!opt.list_only) die("no such gpu %d", opt.gpu);
		std::exit(0);
	}
	c.phys = devs[static_cast<size_t>(opt.gpu)];
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(c.phys, &props);
	std::snprintf(c.name, sizeof c.name, "%s", props.deviceName);
	c.driver_version = props.driverVersion;
	vkGetPhysicalDeviceMemoryProperties(c.phys, &c.mem);

	uint32_t ne = 0;
	vkEnumerateDeviceExtensionProperties(c.phys, nullptr, &ne, nullptr);
	std::vector<VkExtensionProperties> exts(ne);
	vkEnumerateDeviceExtensionProperties(c.phys, nullptr, &ne, exts.data());

	// supported features (core 1.1-1.3 + the extensions the emulator uses)
	VkPhysicalDeviceVulkan11Features v11 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
	VkPhysicalDeviceVulkan12Features v12 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
	VkPhysicalDeviceVulkan13Features v13 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	VkPhysicalDeviceRobustness2FeaturesEXT rb2 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
	VkPhysicalDeviceShaderObjectFeaturesEXT so {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
	VkPhysicalDeviceFeatures2 f2 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	const bool have_rb2 = HasExt(exts, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
	const bool have_so  = HasExt(exts, VK_EXT_SHADER_OBJECT_EXTENSION_NAME);
	c.push_desc = HasExt(exts, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
	void** tail = &f2.pNext;
	auto chain = [&](auto& s) { *tail = &s; tail = &s.pNext; };
	chain(v11); chain(v12); chain(v13);
	if (have_rb2) chain(rb2);
	if (have_so) chain(so);
	vkGetPhysicalDeviceFeatures2(c.phys, &f2);

	// robustness configuration under test
	const std::string& rm = opt.robustness;
	bool rb1 = f2.features.robustBufferAccess, rb2_on = rb2.robustBufferAccess2, nulld = rb2.nullDescriptor, rimg2 = rb2.robustImageAccess2, rimg = v13.robustImageAccess;
	if (rm == "none") { rb1 = rb2_on = nulld = rimg2 = rimg = false; }
	else if (rm == "rb1") { rb2_on = nulld = rimg2 = rimg = false; }
	else if (rm == "rb2") { rb1 = true; }
	f2.features.robustBufferAccess = rb1;
	v13.robustImageAccess          = rimg;
	rb2.robustBufferAccess2        = rb2_on;
	rb2.nullDescriptor             = nulld;
	rb2.robustImageAccess2         = rimg2;
	c.robustness2 = have_rb2;
	c.subgroup_control = v13.subgroupSizeControl;
	c.shader_object    = have_so && so.shaderObject;
	so.shaderObject    = c.shader_object;

	VkPhysicalDeviceVulkan13Properties p13 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
	VkPhysicalDeviceRobustness2PropertiesEXT rbp {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT};
	VkPhysicalDeviceProperties2 p2 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
	p2.pNext  = &p13;
	p13.pNext = have_rb2 ? static_cast<void*>(&rbp) : nullptr;
	vkGetPhysicalDeviceProperties2(c.phys, &p2);
	c.min_subgroup = p13.minSubgroupSize;
	c.max_subgroup = p13.maxSubgroupSize;
	c.robust_storage_align = have_rb2 ? rbp.robustStorageBufferAccessSizeAlignment : 0;

	uint32_t nq = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(c.phys, &nq, nullptr);
	std::vector<VkQueueFamilyProperties> qf(nq);
	vkGetPhysicalDeviceQueueFamilyProperties(c.phys, &nq, qf.data());
	c.queue_family = ~0u;
	for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { c.queue_family = i; break; }
	if (c.queue_family == ~0u) die("no compute queue");
	float prio = 1.f;
	VkDeviceQueueCreateInfo qci {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = c.queue_family; qci.queueCount = 1; qci.pQueuePriorities = &prio;

	std::vector<const char*> enabled;
	if (c.push_desc) enabled.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
	if (have_rb2) enabled.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
	if (c.shader_object) enabled.push_back(VK_EXT_SHADER_OBJECT_EXTENSION_NAME);
	VkDeviceCreateInfo dci {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.pNext = &f2;
	dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = static_cast<uint32_t>(enabled.size()); dci.ppEnabledExtensionNames = enabled.data();
	VK_OK(vkCreateDevice(c.phys, &dci, nullptr, &c.device));
	vkGetDeviceQueue(c.device, c.queue_family, 0, &c.queue);
	c.CreateShaders  = reinterpret_cast<PFN_vkCreateShadersEXT>(vkGetDeviceProcAddr(c.device, "vkCreateShadersEXT"));
	c.DestroyShader  = reinterpret_cast<PFN_vkDestroyShaderEXT>(vkGetDeviceProcAddr(c.device, "vkDestroyShaderEXT"));
	c.CmdBindShaders = reinterpret_cast<PFN_vkCmdBindShadersEXT>(vkGetDeviceProcAddr(c.device, "vkCmdBindShadersEXT"));
	c.CmdPush        = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(vkGetDeviceProcAddr(c.device, "vkCmdPushDescriptorSetKHR"));
	return c;
}

// ---- pipeline layout from reflection ---------------------------------------------------------------
struct Layout {
	std::vector<VkDescriptorSetLayout> sets;
	VkPipelineLayout                   pipeline = nullptr;
	bool                               push     = false;
};

static Layout MakeLayout(const Ctx& c, const Reflection& r, VkShaderStageFlags stages, bool want_push) {
	Layout l;
	uint32_t max_set = 0;
	for (auto& b : r.bindings) max_set = std::max(max_set, b.set);
	l.sets.resize(max_set + 1);
	for (uint32_t s = 0; s <= max_set; s++) {
		std::vector<VkDescriptorSetLayoutBinding> vb;
		for (auto& b : r.bindings) if (b.set == s) {
			VkDescriptorSetLayoutBinding x {};
			x.binding = b.binding; x.descriptorType = b.type; x.descriptorCount = b.count; x.stageFlags = stages;
			vb.push_back(x);
		}
		VkDescriptorSetLayoutCreateInfo ci {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		const bool push = want_push && c.push_desc && s == 0;
		if (push) ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
		ci.bindingCount = static_cast<uint32_t>(vb.size()); ci.pBindings = vb.data();
		VkResult res = vkCreateDescriptorSetLayout(c.device, &ci, nullptr, &l.sets[s]);
		if (res != VK_SUCCESS && push) { // push descriptor limits: retry as an ordinary set
			ci.flags = 0;
			res = vkCreateDescriptorSetLayout(c.device, &ci, nullptr, &l.sets[s]);
		} else if (push) {
			l.push = true;
		}
		VK_OK(res);
	}
	VkPushConstantRange pcr {stages, 0, 128};
	VkPipelineLayoutCreateInfo pl {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pl.setLayoutCount = static_cast<uint32_t>(l.sets.size()); pl.pSetLayouts = l.sets.data();
	if (r.has_push) { pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr; }
	VK_OK(vkCreatePipelineLayout(c.device, &pl, nullptr, &l.pipeline));
	return l;
}

static void DestroyLayout(const Ctx& c, Layout& l) {
	vkDestroyPipelineLayout(c.device, l.pipeline, nullptr);
	for (auto s : l.sets) vkDestroyDescriptorSetLayout(c.device, s, nullptr);
}

// ---- timed compiles ---------------------------------------------------------------------------------
static double Ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

struct Subgroup { VkPipelineShaderStageRequiredSubgroupSizeCreateInfo info {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO}; bool on = false; };

static Subgroup SubgroupFor(const Ctx& c, const Options& o) {
	Subgroup s;
	if (o.subgroup > 0 && c.subgroup_control && static_cast<uint32_t>(o.subgroup) >= c.min_subgroup && static_cast<uint32_t>(o.subgroup) <= c.max_subgroup) {
		s.info.requiredSubgroupSize = static_cast<uint32_t>(o.subgroup);
		s.on = true;
	}
	return s;
}

// Returns the wall ms of the driver call (module creation excluded, reported separately).
static double CompileComputePipeline(const Ctx& c, const Options& o, const Layout& l, const std::vector<uint32_t>& spv, VkPipeline* out, double* module_ms) {
	VkShaderModuleCreateInfo mci {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	mci.codeSize = spv.size() * 4; mci.pCode = spv.data();
	VkShaderModule mod;
	auto t0 = Clock::now();
	VK_OK(vkCreateShaderModule(c.device, &mci, nullptr, &mod));
	auto t1 = Clock::now();
	Subgroup sg = SubgroupFor(c, o);
	VkComputePipelineCreateInfo ci {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = mod; ci.stage.pName = "main";
	if (sg.on) ci.stage.pNext = &sg.info;
	ci.layout = l.pipeline; ci.basePipelineIndex = -1;
	VkPipeline p = nullptr;
	auto t2 = Clock::now();
	VK_OK(vkCreateComputePipelines(c.device, VK_NULL_HANDLE, 1, &ci, nullptr, &p));
	auto t3 = Clock::now();
	if (module_ms) *module_ms = Ms(t1 - t0);
	if (out) *out = p; else vkDestroyPipeline(c.device, p, nullptr);
	vkDestroyShaderModule(c.device, mod, nullptr);
	return Ms(t3 - t2);
}

static double CompileShaderObject(const Ctx& c, const Options& o, const Layout& l, const Reflection& r, const std::vector<uint32_t>& spv) {
	Subgroup sg = SubgroupFor(c, o);
	VkPushConstantRange pcr {VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
	VkShaderCreateInfoEXT ci {VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
	ci.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
	ci.codeSize = spv.size() * 4; ci.pCode = spv.data(); ci.pName = "main";
	ci.setLayoutCount = static_cast<uint32_t>(l.sets.size()); ci.pSetLayouts = l.sets.data();
	if (r.has_push) { ci.pushConstantRangeCount = 1; ci.pPushConstantRanges = &pcr; }
	if (sg.on) ci.pNext = &sg.info;
	VkShaderEXT sh = nullptr;
	auto t0 = Clock::now();
	VK_OK(c.CreateShaders(c.device, 1, &ci, nullptr, &sh));
	auto t1 = Clock::now();
	c.DestroyShader(c.device, sh, nullptr);
	return Ms(t1 - t0);
}

static double Median(std::vector<double> v) {
	std::sort(v.begin(), v.end());
	return v.empty() ? 0 : (v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]));
}

static std::string Arr(const std::vector<double>& v) {
	std::string s = "[";
	char b[32];
	for (size_t i = 0; i < v.size(); i++) { std::snprintf(b, sizeof b, "%s%.2f", i ? "," : "", v[i]); s += b; }
	return s + "]";
}

static std::string Escape(const std::string& s) {
	std::string o;
	for (char ch : s) { if (ch == '"' || ch == '\\') o += '\\'; o += ch; }
	return o;
}

static std::atomic<uint64_t> g_tag {1};

static void BenchOne(const Ctx& c, const Options& o, const std::string& path) {
	auto spv = ReadSpirv(path);
	auto r   = Reflect(spv);
	if (r.stage != VK_SHADER_STAGE_COMPUTE_BIT) die("%s is not a compute shader (use --gfx for VS/PS pairs)", path.c_str());
	Layout l = MakeLayout(c, r, VK_SHADER_STAGE_COMPUTE_BIT, o.push_desc);
	for (const std::string mode : {"pipeline", "shader-object"}) {
		if (o.mode != "both" && o.mode != mode) continue;
		if (mode == "shader-object" && !c.shader_object) die("VK_EXT_shader_object not available");
		std::vector<double> ms, mod_ms;
		for (int i = -o.warmup; i < o.reps; i++) {
			auto u = o.uniq ? Uniquify(spv, g_tag++) : spv;
			double m = 0, t;
			if (mode == "pipeline") t = CompileComputePipeline(c, o, l, u, nullptr, &m);
			else t = CompileShaderObject(c, o, l, r, u);
			if (i >= 0) { ms.push_back(t); mod_ms.push_back(m); }
		}
		std::printf("{\"file\":\"%s\",\"kind\":\"compute\",\"mode\":\"%s\",\"words\":%u,\"blocks\":%u,\"bindings\":%zu,\"push_set\":%s,\"reps\":%d,"
		            "\"median_ms\":%.2f,\"min_ms\":%.2f,\"max_ms\":%.2f,\"module_ms\":%.2f,\"ms\":%s,\"robustness\":\"%s\",\"subgroup\":%d}\n",
		            Escape(path).c_str(), mode.c_str(), r.words, r.blocks, r.bindings.size(), l.push ? "true" : "false", o.reps, Median(ms),
		            *std::min_element(ms.begin(), ms.end()), *std::max_element(ms.begin(), ms.end()), Median(mod_ms), Arr(ms).c_str(),
		            o.robustness.c_str(), o.subgroup);
		std::fflush(stdout);
	}
	DestroyLayout(c, l);
}

// K concurrent compiles per rep; reports wall time of the group and each member's own time.
static void BenchParallel(const Ctx& c, const Options& o) {
	struct Item { std::vector<uint32_t> spv; Reflection r; Layout l; std::string path; };
	std::vector<Item> items;
	for (auto& f : o.files) {
		Item it; it.spv = ReadSpirv(f); it.r = Reflect(it.spv); it.path = f;
		it.l = MakeLayout(c, it.r, VK_SHADER_STAGE_COMPUTE_BIT, o.push_desc);
		items.push_back(std::move(it));
		if (!o.distinct) break;
	}
	const int K = o.parallel;
	std::vector<double> wall, single;
	std::vector<std::vector<double>> member(static_cast<size_t>(K));
	for (int rep = -o.warmup; rep < o.reps; rep++) {
		std::vector<std::thread> th;
		std::vector<double>      t(static_cast<size_t>(K), 0.0);
		std::atomic<int>         ready {0};
		std::atomic<bool>        go {false};
		Clock::time_point        t0;
		for (int k = 0; k < K; k++) {
			th.emplace_back([&, k] {
				const Item& it = items[static_cast<size_t>(k) % items.size()];
				auto u = o.uniq ? Uniquify(it.spv, g_tag++) : it.spv;
				ready++;
				while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
				double m;
				t[static_cast<size_t>(k)] = o.mode == "shader-object" ? CompileShaderObject(c, o, it.l, it.r, u) : CompileComputePipeline(c, o, it.l, u, nullptr, &m);
			});
		}
		while (ready.load() < K) std::this_thread::yield();
		t0 = Clock::now();
		go.store(true, std::memory_order_release);
		for (auto& x : th) x.join();
		double w = Ms(Clock::now() - t0);
		if (rep >= 0) { wall.push_back(w); for (int k = 0; k < K; k++) member[static_cast<size_t>(k)].push_back(t[static_cast<size_t>(k)]); }
	}
	// serial reference for the same set of K compiles: sum of the members' single-thread medians
	double sum_serial = 0;
	for (int k = 0; k < K; k++) {
		const Item& it = items[static_cast<size_t>(k) % items.size()];
		std::vector<double> s;
		for (int i = 0; i < std::max(1, o.reps / 2); i++) { auto u = Uniquify(it.spv, g_tag++); double m; s.push_back(o.mode == "shader-object" ? CompileShaderObject(c, o, it.l, it.r, u) : CompileComputePipeline(c, o, it.l, u, nullptr, &m)); }
		sum_serial += Median(s);
	}
	std::string members = "[";
	for (int k = 0; k < K; k++) { char b[32]; std::snprintf(b, sizeof b, "%s%.2f", k ? "," : "", Median(member[static_cast<size_t>(k)])); members += b; }
	members += "]";
	std::printf("{\"kind\":\"parallel\",\"mode\":\"%s\",\"threads\":%d,\"distinct\":%s,\"first_file\":\"%s\",\"wall_median_ms\":%.2f,\"wall_ms\":%s,"
	            "\"member_median_ms\":%s,\"serial_sum_ms\":%.2f,\"speedup\":%.2f,\"hw_threads\":%u}\n",
	            o.mode.c_str(), K, o.distinct ? "true" : "false", Escape(items[0].path).c_str(), Median(wall), Arr(wall).c_str(), members.c_str(), sum_serial,
	            Median(wall) > 0 ? sum_serial / Median(wall) : 0.0, std::thread::hardware_concurrency());
	std::fflush(stdout);
	for (auto& it : items) DestroyLayout(c, it.l);
}

// ---- graphics VS/PS pair ------------------------------------------------------------------------------
static VkFormat FormatForInput(const Reflection& r, uint32_t type_id) {
	const SpvInst* t = &r.by_id.at(type_id);
	uint32_t comps = 1;
	if (t->op == 23) { comps = t->ops[2]; t = &r.by_id.at(t->ops[1]); }
	if (t->op == 22) { static const VkFormat f[] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT}; return f[std::min(comps, 4u) - 1]; }
	bool is_signed = t->op == 21 && t->ops[2] == 1;
	static const VkFormat u[] = {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT};
	static const VkFormat s[] = {VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT};
	return (is_signed ? s : u)[std::min(comps, 4u) - 1];
}

static void BenchGraphics(const Ctx& c, const Options& o, const std::string& vs_path, const std::string& ps_path) {
	auto vs = ReadSpirv(vs_path), ps = ReadSpirv(ps_path);
	auto rv = Reflect(vs), rp = Reflect(ps);
	if (rv.stage != VK_SHADER_STAGE_VERTEX_BIT || rp.stage != VK_SHADER_STAGE_FRAGMENT_BIT) die("--gfx expects <vs.spv> <ps.spv>");
	// one merged binding list (the emulator uses one push-descriptor set for both stages)
	Reflection merged = rv;
	for (auto b : rp.bindings) {
		bool found = false;
		for (auto& m : merged.bindings) if (m.set == b.set && m.binding == b.binding) found = true;
		if (!found) merged.bindings.push_back(b);
	}
	merged.has_push = rv.has_push || rp.has_push;
	Layout l = MakeLayout(c, merged, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, o.push_desc);
	std::vector<VkVertexInputBindingDescription>   vb;
	std::vector<VkVertexInputAttributeDescription> va;
	for (auto& in : rv.inputs) {
		vb.push_back({in.location, 16, VK_VERTEX_INPUT_RATE_VERTEX});
		va.push_back({in.location, in.location, FormatForInput(rv, in.type_id), 0});
	}
	std::vector<VkFormat> colors;
	uint32_t max_loc = 0;
	for (auto& out : rp.outputs) max_loc = std::max(max_loc, out.location + 1);
	colors.assign(max_loc, VK_FORMAT_R8G8B8A8_UNORM);
	for (auto& out : rp.outputs) {
		VkFormat f = FormatForInput(rp, out.type_id);
		colors[out.location] = (f >= VK_FORMAT_R32_UINT && f <= VK_FORMAT_R32G32B32A32_SINT) ? f : VK_FORMAT_R8G8B8A8_UNORM;
	}
	std::vector<VkPipelineColorBlendAttachmentState> blend(colors.size());
	for (auto& b : blend) b.colorWriteMask = 0xf;

	std::vector<double> ms;
	for (int i = -o.warmup; i < o.reps; i++) {
		auto vu = o.uniq ? Uniquify(vs, g_tag++) : vs;
		auto pu = o.uniq ? Uniquify(ps, g_tag++) : ps;
		VkShaderModuleCreateInfo m1 {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}, m2 = m1;
		m1.codeSize = vu.size() * 4; m1.pCode = vu.data();
		m2.codeSize = pu.size() * 4; m2.pCode = pu.data();
		VkShaderModule a, b;
		VK_OK(vkCreateShaderModule(c.device, &m1, nullptr, &a));
		VK_OK(vkCreateShaderModule(c.device, &m2, nullptr, &b));
		VkPipelineShaderStageCreateInfo st[2] {};
		st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, a, "main", nullptr};
		st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, b, "main", nullptr};
		VkPipelineVertexInputStateCreateInfo vi {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		vi.vertexBindingDescriptionCount = static_cast<uint32_t>(vb.size()); vi.pVertexBindingDescriptions = vb.data();
		vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(va.size()); vi.pVertexAttributeDescriptions = va.data();
		VkPipelineInputAssemblyStateCreateInfo ia {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		VkPipelineViewportStateCreateInfo vp {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		vp.viewportCount = 1; vp.scissorCount = 1;
		VkPipelineRasterizationStateCreateInfo rs {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.f;
		VkPipelineMultisampleStateCreateInfo ms_ {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		ms_.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineDepthStencilStateCreateInfo ds {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
		VkPipelineColorBlendStateCreateInfo cb {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		cb.attachmentCount = static_cast<uint32_t>(blend.size()); cb.pAttachments = blend.data();
		const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo dsi {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyn;
		VkPipelineRenderingCreateInfo ri {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
		ri.colorAttachmentCount = static_cast<uint32_t>(colors.size()); ri.pColorAttachmentFormats = colors.data();
		ri.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT_S8_UINT; ri.stencilAttachmentFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
		VkGraphicsPipelineCreateInfo gi {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		gi.pNext = &ri; gi.stageCount = 2; gi.pStages = st; gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia;
		gi.pViewportState = &vp; gi.pRasterizationState = &rs; gi.pMultisampleState = &ms_; gi.pDepthStencilState = &ds;
		gi.pColorBlendState = &cb; gi.pDynamicState = &dsi; gi.layout = l.pipeline; gi.basePipelineIndex = -1;
		VkPipeline p = nullptr;
		auto t0 = Clock::now();
		VK_OK(vkCreateGraphicsPipelines(c.device, VK_NULL_HANDLE, 1, &gi, nullptr, &p));
		double t = Ms(Clock::now() - t0);
		vkDestroyPipeline(c.device, p, nullptr);
		vkDestroyShaderModule(c.device, a, nullptr);
		vkDestroyShaderModule(c.device, b, nullptr);
		if (i >= 0) ms.push_back(t);
	}
	std::printf("{\"file\":\"%s+%s\",\"kind\":\"graphics\",\"mode\":\"pipeline\",\"words\":%u,\"vs_words\":%u,\"ps_words\":%u,\"blocks\":%u,\"reps\":%d,"
	            "\"median_ms\":%.2f,\"min_ms\":%.2f,\"max_ms\":%.2f,\"ms\":%s,\"robustness\":\"%s\"}\n",
	            Escape(vs_path).c_str(), Escape(ps_path).c_str(), rv.words + rp.words, rv.words, rp.words, rv.blocks + rp.blocks, o.reps, Median(ms),
	            *std::min_element(ms.begin(), ms.end()), *std::max_element(ms.begin(), ms.end()), Arr(ms).c_str(), o.robustness.c_str());
	std::fflush(stdout);
	DestroyLayout(c, l);
}


// ---- graphics state sequence: does a second pipeline with other state reuse the driver's code? -------------
// Files are vs0 ps0 [vs1 ps1 ...]. Every module is created once (unique per process), then the steps of
// --gfx-seq are created one after the other and timed, with no pipeline cache. A step is
// <state>[:v<i>p<j>]. States: base (RGBA16F colour 0, D32S8, no blend, triangle list, vertex stride 16),
// again (= base), blend, mask7, fmt8 (RGBA8), fmt11 (B10G11R11), fmt32 (RGBA32F), nodepth, depth16,
// cullback, strip, vstride (vertex stride 32), vfmt16 (16-bit float vertex inputs), extra (one more colour
// attachment), msaa4, blendfmt32 (blend + RGBA32F).
struct GfxState {
	VkFormat color = VK_FORMAT_R16G16B16A16_SFLOAT;
	VkFormat depth = VK_FORMAT_D32_SFLOAT_S8_UINT;
	bool blend = false;
	uint32_t mask = 0xf;
	VkCullModeFlags cull = VK_CULL_MODE_NONE;
	VkPrimitiveTopology topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	uint32_t vstride = 16;
	bool vfmt16 = false;
	int extra = 0;
	VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

static GfxState StateByName(const std::string& n) {
	GfxState s;
	if (n == "base" || n == "again") return s;
	if (n == "blend") s.blend = true;
	else if (n == "mask7") s.mask = 7;
	else if (n == "fmt8") s.color = VK_FORMAT_R8G8B8A8_UNORM;
	else if (n == "fmt11") s.color = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
	else if (n == "fmt32") s.color = VK_FORMAT_R32G32B32A32_SFLOAT;
	else if (n == "nodepth") s.depth = VK_FORMAT_UNDEFINED;
	else if (n == "depth16") s.depth = VK_FORMAT_D16_UNORM;
	else if (n == "cullback") s.cull = VK_CULL_MODE_BACK_BIT;
	else if (n == "strip") s.topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	else if (n == "vstride") s.vstride = 32;
	else if (n == "vfmt16") s.vfmt16 = true;
	else if (n == "extra") s.extra = 1;
	else if (n == "msaa4") s.samples = VK_SAMPLE_COUNT_4_BIT;
	else if (n == "blendfmt32") { s.blend = true; s.color = VK_FORMAT_R32G32B32A32_SFLOAT; }
	else die("unknown graphics state %s", n.c_str());
	return s;
}

static void BenchGraphicsStates(const Ctx& c, const Options& o) {
	if (o.files.size() % 2 || o.files.empty()) die("--gfx-seq needs vs/ps pairs");
	size_t n = o.files.size() / 2;
	std::vector<std::vector<uint32_t>> vs(n), ps(n);
	std::vector<Reflection> rv, rp;
	Reflection merged;
	bool first = true;
	for (size_t i = 0; i < n; i++) {
		vs[i] = ReadSpirv(o.files[2 * i]); ps[i] = ReadSpirv(o.files[2 * i + 1]);
		rv.push_back(Reflect(vs[i])); rp.push_back(Reflect(ps[i]));
		if (rv[i].stage != VK_SHADER_STAGE_VERTEX_BIT || rp[i].stage != VK_SHADER_STAGE_FRAGMENT_BIT) die("--gfx-seq expects vs/ps pairs");
		for (auto* r : {&rv[i], &rp[i]}) {
			if (first) { merged = *r; first = false; continue; }
			for (auto b : r->bindings) {
				bool found = false;
				for (auto& m : merged.bindings) if (m.set == b.set && m.binding == b.binding) found = true;
				if (!found) merged.bindings.push_back(b);
			}
			merged.has_push = merged.has_push || r->has_push;
		}
	}
	Layout l = MakeLayout(c, merged, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, o.push_desc);
	std::vector<VkShaderModule> mv(n), mp(n);
	std::map<std::string, VkShaderModule> made; // the same file given twice is the same module
	std::vector<VkShaderModule> owned;
	const auto module = [&](const std::string& path, const std::vector<uint32_t>& code) {
		auto it = made.find(path);
		if (it != made.end()) return it->second;
		auto u = Uniquify(code, g_tag++);
		VkShaderModuleCreateInfo m {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		m.codeSize = u.size() * 4; m.pCode = u.data();
		VkShaderModule out;
		VK_OK(vkCreateShaderModule(c.device, &m, nullptr, &out));
		made[path] = out; owned.push_back(out);
		return out;
	};
	for (size_t i = 0; i < n; i++) { mv[i] = module(o.files[2 * i], vs[i]); mp[i] = module(o.files[2 * i + 1], ps[i]); }
	std::vector<std::string> steps;
	{ std::string cur; for (char ch : o.gfx_seq + ",") { if (ch == ',') { if (!cur.empty()) steps.push_back(cur); cur.clear(); } else cur += ch; } }
	int idx = 0;
	for (auto& step : steps) {
		std::string name = step; size_t vi = 0, pj = 0;
		auto colon = step.find(':');
		if (colon != std::string::npos) {
			name = step.substr(0, colon);
			if (std::sscanf(step.c_str() + colon + 1, "v%zup%zu", &vi, &pj) != 2 || vi >= n || pj >= n) die("bad step %s", step.c_str());
		}
		GfxState st = StateByName(name);
		const Reflection& rvx = rv[vi];
		std::vector<VkVertexInputBindingDescription>   vb;
		std::vector<VkVertexInputAttributeDescription> va;
		for (auto& in : rvx.inputs) {
			vb.push_back({in.location, st.vstride, VK_VERTEX_INPUT_RATE_VERTEX});
			VkFormat f = FormatForInput(rvx, in.type_id);
			if (st.vfmt16 && f == VK_FORMAT_R32G32B32A32_SFLOAT) f = VK_FORMAT_R16G16B16A16_SFLOAT;
			else if (st.vfmt16 && f == VK_FORMAT_R32G32B32_SFLOAT) f = VK_FORMAT_R16G16B16A16_SFLOAT;
			else if (st.vfmt16 && f == VK_FORMAT_R32G32_SFLOAT) f = VK_FORMAT_R16G16_SFLOAT;
			va.push_back({in.location, in.location, f, 0});
		}
		uint32_t max_loc = 0;
		for (auto& out : rp[pj].outputs) max_loc = std::max(max_loc, out.location + 1);
		std::vector<VkFormat> colors(max_loc + st.extra, st.color);
		for (auto& out : rp[pj].outputs) {
			VkFormat f = FormatForInput(rp[pj], out.type_id);
			if (f >= VK_FORMAT_R32_UINT && f <= VK_FORMAT_R32G32B32A32_SINT) colors[out.location] = f; // integer outputs keep an integer target
		}
		std::vector<VkPipelineColorBlendAttachmentState> blend(colors.size());
		for (auto& b : blend) {
			b.colorWriteMask = st.mask;
			if (st.blend) { b.blendEnable = VK_TRUE; b.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; b.colorBlendOp = VK_BLEND_OP_ADD; b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; b.alphaBlendOp = VK_BLEND_OP_ADD; }
		}
		VkPipelineShaderStageCreateInfo sg[2] {};
		sg[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, mv[vi], "main", nullptr};
		sg[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, mp[pj], "main", nullptr};
		VkPipelineVertexInputStateCreateInfo vi_ {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		vi_.vertexBindingDescriptionCount = static_cast<uint32_t>(vb.size()); vi_.pVertexBindingDescriptions = vb.data();
		vi_.vertexAttributeDescriptionCount = static_cast<uint32_t>(va.size()); vi_.pVertexAttributeDescriptions = va.data();
		VkPipelineInputAssemblyStateCreateInfo ia {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		ia.topology = st.topo;
		VkPipelineViewportStateCreateInfo vp {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		vp.viewportCount = 1; vp.scissorCount = 1;
		VkPipelineRasterizationStateCreateInfo rs {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = st.cull; rs.lineWidth = 1.f;
		VkPipelineMultisampleStateCreateInfo ms_ {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		ms_.rasterizationSamples = st.samples;
		VkPipelineDepthStencilStateCreateInfo ds {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
		VkPipelineColorBlendStateCreateInfo cb {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		cb.attachmentCount = static_cast<uint32_t>(blend.size()); cb.pAttachments = blend.data();
		const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo dsi {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyn;
		VkPipelineRenderingCreateInfo ri {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
		ri.colorAttachmentCount = static_cast<uint32_t>(colors.size()); ri.pColorAttachmentFormats = colors.data();
		ri.depthAttachmentFormat = st.depth;
		ri.stencilAttachmentFormat = st.depth == VK_FORMAT_D32_SFLOAT_S8_UINT ? st.depth : VK_FORMAT_UNDEFINED;
		VkGraphicsPipelineCreateInfo gi {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		gi.pNext = &ri; gi.stageCount = 2; gi.pStages = sg; gi.pVertexInputState = &vi_; gi.pInputAssemblyState = &ia;
		gi.pViewportState = &vp; gi.pRasterizationState = &rs; gi.pMultisampleState = &ms_; gi.pDepthStencilState = &ds;
		gi.pColorBlendState = &cb; gi.pDynamicState = &dsi; gi.layout = l.pipeline; gi.basePipelineIndex = -1;
		VkPipeline p = nullptr;
		auto t0 = Clock::now();
		VK_OK(vkCreateGraphicsPipelines(c.device, VK_NULL_HANDLE, 1, &gi, nullptr, &p));
		double t = Ms(Clock::now() - t0);
		vkDestroyPipeline(c.device, p, nullptr);
		std::printf("{\"kind\":\"gfx-state\",\"idx\":%d,\"step\":\"%s\",\"ms\":%.3f,\"vs_words\":%u,\"ps_words\":%u}\n", idx++, step.c_str(), t, rv[vi].words, rp[pj].words);
		std::fflush(stdout);
	}
	for (auto m : owned) vkDestroyShaderModule(c.device, m, nullptr);
	DestroyLayout(c, l);
}

// ---- execute once and dump storage buffers (semantic check of rewritten variants) -------------------------
static uint32_t FindMem(const Ctx& c, uint32_t bits, VkMemoryPropertyFlags want) {
	for (uint32_t i = 0; i < c.mem.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (c.mem.memoryTypes[i].propertyFlags & want) == want) return i;
	die("no suitable memory type");
	return 0;
}

static void ExecOne(const Ctx& c, const Options& o, const std::string& path) {
	auto spv = ReadSpirv(path);
	auto r   = Reflect(spv);
	Options o2 = o;
	o2.push_desc = false;
	Layout l = MakeLayout(c, r, VK_SHADER_STAGE_COMPUTE_BIT, false);
	VkPipeline pipe;
	CompileComputePipeline(c, o2, l, spv, &pipe, nullptr);
	constexpr VkDeviceSize kSize = 1024;
	struct Buf { uint32_t binding; VkBuffer buf; VkDeviceMemory mem; uint32_t* map; VkDeviceSize range; };
	std::vector<Buf> bufs;
	for (auto& b : r.bindings) {
		if (b.type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) die("--exec supports storage buffers only");
		Buf x {b.binding, nullptr, nullptr, nullptr, kSize};
		VkBufferCreateInfo bi {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		bi.size = kSize; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VK_OK(vkCreateBuffer(c.device, &bi, nullptr, &x.buf));
		VkMemoryRequirements mr;
		vkGetBufferMemoryRequirements(c.device, x.buf, &mr);
		VkMemoryAllocateInfo ai {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.allocationSize = mr.size; ai.memoryTypeIndex = FindMem(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		VK_OK(vkAllocateMemory(c.device, &ai, nullptr, &x.mem));
		VK_OK(vkBindBufferMemory(c.device, x.buf, x.mem, 0));
		void* p;
		VK_OK(vkMapMemory(c.device, x.mem, 0, kSize, 0, &p));
		x.map = static_cast<uint32_t*>(p);
		for (uint32_t i = 0; i < kSize / 4; i++) x.map[i] = 0xA5000000u | (b.binding << 16) | i; // known pattern
		auto it = o.ranges.find(b.binding);
		if (it != o.ranges.end()) x.range = it->second;
		bufs.push_back(x);
	}
	VkDescriptorPoolSize ps {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_cast<uint32_t>(bufs.size() + 1)};
	VkDescriptorPoolCreateInfo dpi {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
	VkDescriptorPool pool;
	VK_OK(vkCreateDescriptorPool(c.device, &dpi, nullptr, &pool));
	VkDescriptorSetAllocateInfo dsa {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dsa.descriptorPool = pool; dsa.descriptorSetCount = 1; dsa.pSetLayouts = l.sets.data();
	VkDescriptorSet set;
	VK_OK(vkAllocateDescriptorSets(c.device, &dsa, &set));
	std::vector<VkDescriptorBufferInfo> infos(bufs.size());
	std::vector<VkWriteDescriptorSet>   writes(bufs.size());
	for (size_t i = 0; i < bufs.size(); i++) {
		infos[i] = {bufs[i].buf, 0, bufs[i].range};
		writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, bufs[i].binding, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &infos[i], nullptr};
	}
	vkUpdateDescriptorSets(c.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
	VkCommandPoolCreateInfo cpi {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	cpi.queueFamilyIndex = c.queue_family;
	VkCommandPool cp;
	VK_OK(vkCreateCommandPool(c.device, &cpi, nullptr, &cp));
	VkCommandBufferAllocateInfo cba {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cba.commandPool = cp; cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cba.commandBufferCount = 1;
	VkCommandBuffer cmd;
	VK_OK(vkAllocateCommandBuffers(c.device, &cba, &cmd));
	VkCommandBufferBeginInfo bi {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	VK_OK(vkBeginCommandBuffer(cmd, &bi));
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, l.pipeline, 0, 1, &set, 0, nullptr);
	vkCmdDispatch(cmd, 1, 1, 1);
	VK_OK(vkEndCommandBuffer(cmd));
	VkSubmitInfo si {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
	VK_OK(vkQueueSubmit(c.queue, 1, &si, VK_NULL_HANDLE));
	VK_OK(vkQueueWaitIdle(c.queue));
	for (auto& b : bufs) {
		std::printf("{\"kind\":\"exec\",\"file\":\"%s\",\"binding\":%u,\"range\":%" PRIu64 ",\"data\":\"", Escape(path).c_str(), b.binding, static_cast<uint64_t>(b.range));
		for (uint32_t i = 0; i < kSize / 4; i++) std::printf("%08x%s", b.map[i], i + 1 < kSize / 4 ? " " : "");
		std::printf("\"}\n");
	}
	std::fflush(stdout);
}

// ---- main -----------------------------------------------------------------------------------------------
static void Usage() {
	std::fputs(
	    "usage: spirv-compile-bench [options] file.spv...\n"
	    "  --mode pipeline|shader-object|both   what to time (default pipeline)\n"
	    "  --reps N / --warmup N                timed / discarded compiles per shader (default 5 / 1)\n"
	    "  --parallel K [--distinct]            K concurrent compiles per rep (same shader, or the files in turn)\n"
	    "  --robustness emu|none|rb1|rb2        device robustness features (emu = what the emulator enables)\n"
	    "  --subgroup N                         requiredSubgroupSize (default 32, 0 = off)\n"
	    "  --no-push                            plain descriptor set layout instead of push descriptors\n"
	    "  --no-uniq                            do not make each compile's SPIR-V binary unique\n"
	    "  --keep-nv-cache                      leave the NVIDIA disk cache enabled (default: disabled)\n"
	    "  --gfx                                files are vs.spv ps.spv pairs\n"
	    "  --exec [--range BINDING=BYTES]       run once (1 group), dump all storage buffers as JSON\n"
	    "  --gpu N / --list-gpus\n", stderr);
}

int main(int argc, char** argv) {
	Options o;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&]() -> const char* { if (i + 1 >= argc) die("%s needs a value", a.c_str()); return argv[++i]; };
		if (a == "--mode") o.mode = next();
		else if (a == "--reps") o.reps = std::atoi(next());
		else if (a == "--warmup") o.warmup = std::atoi(next());
		else if (a == "--parallel") o.parallel = std::atoi(next());
		else if (a == "--distinct") o.distinct = true;
		else if (a == "--robustness") o.robustness = next();
		else if (a == "--subgroup") o.subgroup = std::atoi(next());
		else if (a == "--no-push") o.push_desc = false;
		else if (a == "--no-uniq") o.uniq = false;
		else if (a == "--keep-nv-cache") o.keep_nv_cache = true;
		else if (a == "--gfx") o.gfx = true;
		else if (a == "--gfx-seq") o.gfx_seq = next();
		else if (a == "--exec") o.exec = true;
		else if (a == "--range") { unsigned b, n; if (std::sscanf(next(), "%u=%u", &b, &n) != 2) die("--range BINDING=BYTES"); o.ranges[b] = n; }
		else if (a == "--gpu") o.gpu = std::atoi(next());
		else if (a == "--list-gpus") o.list_only = true;
		else if (a == "-h" || a == "--help") { Usage(); return 0; }
		else if (!a.empty() && a[0] == '-') { Usage(); die("unknown option %s", a.c_str()); }
		else o.files.push_back(a);
	}
	if (o.files.empty() && !o.list_only) { Usage(); return 2; }
	if (!o.keep_nv_cache) {
		// The NVIDIA driver keeps a disk cache of compiled shaders keyed by the SPIR-V; it would turn
		// every repeat into a lookup.  Must be set before the driver is loaded.
		setenv("__GL_SHADER_DISK_CACHE", "0", 1);
		setenv("__GL_SHADER_DISK_CACHE_SKIP_CLEANUP", "1", 1);
	}
	Ctx c = MakeContext(o);
	std::fprintf(stderr, "device: %s driver 0x%x subgroup %u..%u push_desc=%d shader_object=%d robustness2=%d robustStorageBufferAccessSizeAlignment=%" PRIu64 " robustness=%s\n",
	             c.name, c.driver_version, c.min_subgroup, c.max_subgroup, c.push_desc, c.shader_object, c.robustness2, static_cast<uint64_t>(c.robust_storage_align), o.robustness.c_str());
	if (o.parallel > 0) {
		BenchParallel(c, o);
	} else if (!o.gfx_seq.empty()) {
		BenchGraphicsStates(c, o);
	} else if (o.gfx) {
		if (o.files.size() % 2) die("--gfx needs vs/ps pairs");
		for (size_t i = 0; i < o.files.size(); i += 2) BenchGraphics(c, o, o.files[i], o.files[i + 1]);
	} else if (o.exec) {
		for (auto& f : o.files) ExecOne(c, o, f);
	} else {
		for (auto& f : o.files) BenchOne(c, o, f);
	}
	vkDestroyDevice(c.device, nullptr);
	vkDestroyInstance(c.instance, nullptr);
	return 0;
}
