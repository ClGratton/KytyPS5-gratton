#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

#include "common/assert.h"
#include "common/benchInstrument.h"
#include "common/speculation.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/pipeline/blendMapping.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/shaderCompiler.h"
#include "kernel/memory.h"
#include "kytyGitVersion.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fmt/format.h>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <spirv-tools/libspirv.hpp>
#include <string_view>
#include <utility>
#include <vector>
#include <xxhash.h>

namespace Libs::Graphics {

namespace {

vk::PolygonMode ResolvePolygonMode(const HW::ModeControl& mode, bool cull_front, bool cull_back) {
	// CxPrimitiveSetup::PolygonMode disables both per-face modes when it is zero.
	if (mode.poly_mode == 0) {
		return vk::PolygonMode::eFill;
	}
	EXIT_NOT_IMPLEMENTED(mode.poly_mode != 1);
	if (cull_front && cull_back) {
		return vk::PolygonMode::eFill;
	}
	if (!cull_front && !cull_back && mode.polymode_front_ptype != mode.polymode_back_ptype) {
		EXIT("Pipeline: different polygon modes for two visible faces are unsupported\n");
	}
	// Vulkan has one polygon mode. A culled face does not constrain that mode.
	const auto polygon_mode = cull_front ? mode.polymode_back_ptype : mode.polymode_front_ptype;
	switch (polygon_mode) {
		case 0: return vk::PolygonMode::ePoint;
		case 1: return vk::PolygonMode::eLine;
		case 2: return vk::PolygonMode::eFill;
		default: EXIT("Pipeline: invalid polygon mode %u\n", polygon_mode);
	}
}

// Bump when the cache file layout or the meaning of the signature changes. Driver pipeline-cache
// blobs are content-addressed and validated by the driver itself, so changes to the emulator's
// pipeline creation do not need a bump: stale entries simply never match.
constexpr uint32_t kPipelineCacheSchemaVersion = 1;

std::string DriverCacheSignature(const vk::PhysicalDeviceProperties& properties) {
	constexpr char hex[] = "0123456789abcdef";
	std::string    uuid(VK_UUID_SIZE * 2, '0');
	for (size_t i = 0; i < VK_UUID_SIZE; i++) {
		uuid[i * 2]     = hex[properties.pipelineCacheUUID[i] >> 4u];
		uuid[i * 2 + 1] = hex[properties.pipelineCacheUUID[i] & 0xfu];
	}
	if (Config::GetPipelineCacheKey() == Config::PipelineCacheKey::Schema) {
		return fmt::format("KytyPCS:{}:{:08x}:{:08x}:{:08x}:{}\n", kPipelineCacheSchemaVersion,
		                   properties.vendorID, properties.deviceID, properties.driverVersion, uuid);
	}
	return fmt::format("KytyPC1:{}:{:08x}:{:08x}:{:08x}:{}\n", KYTY_GIT_REVISION,
	                   properties.vendorID, properties.deviceID, properties.driverVersion, uuid);
}

std::string PipelineCacheTitleId() {
	std::string title_id;
	if ((!Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) || title_id.empty()) &&
	    (!Loader::SystemContentParamSfoGetString("CONTENT_ID", &title_id) || title_id.empty())) {
		return {};
	}
	if (!std::ranges::all_of(title_id, [](unsigned char c) {
		    return std::isalnum(c) != 0 || c == '-' || c == '_';
	    })) {
		return {};
	}
	return title_id;
}

template <typename... Args>
void PipelineCacheLog(fmt::format_string<Args...> format, Args&&... args) {
	auto message = fmt::format(format, std::forward<Args>(args)...);
	message += '\n';
	Log::WriteToConsoleAndLog(message);
}

// --shader-timing-log helpers. Everything is a no-op (one relaxed atomic load) when the flag is
// absent: Stopwatch records nothing and callers skip all formatting behind On().
class Stopwatch {
public:
	Stopwatch(): m_on(Bench::TimingEnabled()), m_start(m_on ? Bench::NowNs() : 0), m_last(m_start) {}
	[[nodiscard]] bool    On() const noexcept { return m_on; }
	[[nodiscard]] int64_t Start() const noexcept { return m_start; }
	// Nanoseconds since construction or the previous Lap().
	int64_t Lap() noexcept {
		if (!m_on) {
			return 0;
		}
		const auto now = Bench::NowNs();
		const auto dt  = now - m_last;
		m_last         = now;
		return dt;
	}
	[[nodiscard]] int64_t Elapsed() const noexcept { return m_on ? Bench::NowNs() - m_start : 0; }

private:
	bool    m_on;
	int64_t m_start;
	int64_t m_last;
};

// Duration of a hit-path step (MaterializeResources) inside one program Get().
struct StepTime {
	int64_t t_start = 0;
	int64_t dur     = 0;
	void    Add(const Stopwatch& sw, int64_t lap) noexcept {
		if (dur == 0) {
			t_start = sw.Start();
		}
		dur += lap;
	}
};

const char* StageShortName(ShaderType stage) {
	switch (stage) {
		case ShaderType::Vertex: return "vs";
		case ShaderType::Mesh: return "ms";
		case ShaderType::Local: return "ls";
		case ShaderType::TessellationControl: return "hs";
		case ShaderType::TessellationEvaluation: return "ds";
		case ShaderType::Pixel: return "ps";
		case ShaderType::Compute: return "cs";
		default: return "unknown";
	}
}

// Hash of a resource specialization, computed once at capture (on the GPU thread) so job keys
// never re-walk the vectors. Equality is still ResourceSpecialization::operator==.
std::size_t HashSpecialization(const ShaderRecompiler::IR::ResourceSpecialization& spec) {
	auto mix = [](std::size_t& hash, std::size_t value) {
		hash ^= value + static_cast<std::size_t>(0x9e3779b97f4a7c15ull) + (hash << 6u) + (hash >> 2u);
	};
	std::size_t hash = spec.buffers.size() * 31u + spec.images.size();
	for (const auto& buffer: spec.buffers) {
		mix(hash, buffer.packed_stride);
		mix(hash, static_cast<std::size_t>(buffer.descriptor_format));
		mix(hash, buffer.descriptor_swizzle);
		mix(hash, buffer.zero_stride_oob ? 1u : 0u);
	}
	for (const auto& image: spec.images) {
		mix(hash, static_cast<std::size_t>(image.numeric_class));
		mix(hash, static_cast<std::size_t>(image.dimension));
		mix(hash, image.mip_count);
		mix(hash, static_cast<std::size_t>(image.conversion_format));
		mix(hash, image.shader_swizzle);
		mix(hash, image.indirect_root);
		mix(hash, image.indirect_mapping_offset);
		mix(hash, image.indirect_search_iterations);
		mix(hash, (image.cube ? 1u : 0u) | (image.fmask ? 2u : 0u));
	}
	return hash;
}

bool ReadShaderGuestMemory(void*, uint64_t address, std::span<uint32_t> values) {
	return !values.empty() &&
	       Libs::LibKernel::Memory::TryReadGpuCleanBacking(address, values.data(), values.size_bytes());
}

// Marker for ShaderStageRuntime::resources in prefetched input infos: the stage must look valid
// (operator bool), but workers never read the snapshot (docs/phase2-notes.md), so a shared empty
// one is enough. The GPU thread never sees an input info made by the prefetch.
const ShaderRecompiler::IR::ResourceSnapshot& PrefetchResources() {
	static const ShaderRecompiler::IR::ResourceSnapshot empty;
	return empty;
}

// Lookahead thread (--pipeline-lookahead): plain backing read. ReadShaderGuestMemory refuses every
// guest address off the GPU thread because it needs the GPU thread's view of GPU-dirty ranges;
// the lookahead accepts a possibly stale answer (a wrong prediction only wastes worker time).
bool ReadShaderGuestMemorySpeculative(void*, uint64_t address, std::span<uint32_t> values) {
	return !values.empty() &&
	       Libs::LibKernel::Memory::TryReadBacking(address, values.data(), values.size_bytes());
}

void DumpShaderSpirv(const char* stage_name, uint64_t shader_hash,
                     const std::vector<uint32_t>& spirv) {
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	static std::atomic_int id = 0;
	const auto path = Config::GetShaderLogFolder() / fmt::format("{:04d}_new_shader_{}_{:016x}.spv",
	                                                             id++, stage_name, shader_hash);
	Common::File::CreateDirectories(path.parent_path());
	Common::File file(path);
	if (file.IsInvalid()) {
		const auto path_text = Common::PathToString(path);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", path_text.c_str());
		return;
	}
	file.Write(spirv.data(), spirv.size() * sizeof(uint32_t));
}

void DumpShaderOriginal(const char* stage_name, uint64_t shader_hash,
                        std::span<const uint32_t> code) {
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	EXIT_IF(code.empty());
	static std::atomic_int id = 0;
	const auto path = Config::GetShaderLogFolder() / "original" /
	                  fmt::format("{:04d}_new_shader_{}_{:016x}.bin", id++, stage_name, shader_hash);
	Common::File::CreateDirectories(path.parent_path());
	Common::File file(path);
	if (file.IsInvalid()) {
		const auto path_text = Common::PathToString(path);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", path_text.c_str());
		return;
	}
	file.Write(code.data(), code.size_bytes());
}

bool ValidateShaderSpirv(const char* label, uint64_t shader_hash,
                         const std::vector<uint32_t>& spirv) {
	if (!Config::ShaderValidationEnabled()) {
		return true;
	}
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
	std::string          messages;
	tools.SetMessageConsumer([&messages](spv_message_level_t, const char*,
	                                     const spv_position_t& position, const char* message) {
		messages += fmt::format("{}: {} ({}) {}\n", static_cast<int>(position.line),
		                        static_cast<int>(position.column), static_cast<int>(position.index),
		                        message);
	});
	if (tools.Validate(spirv)) {
		return true;
	}
	spvtools::SpirvTools disassembler(SPV_ENV_VULKAN_1_2);
	std::string          text;
	disassembler.Disassemble(spirv, &text,
	                         static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_NO_HEADER) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COMMENT) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_INDENT) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COLOR));
	LOGF_COLOR(Log::Color::BrightRed, "%s SPIR-V validation failed hash=0x%016" PRIx64 ":\n%s",
	           label, shader_hash, messages.c_str());
	LOGF("%s\n", text.c_str());
	return false;
}

} // namespace

std::size_t PipelineCache::GraphicsPipelineKeyHash::operator()(const GraphicsPipelineKey& key) const {
	std::size_t hash = 0;
	PipelineKeyHash::Mix(hash, key.rendering.color_count);
	for (uint32_t i = 0; i < key.rendering.color_count; i++) {
		PipelineKeyHash::Mix(hash, static_cast<uint32_t>(key.rendering.color_formats[i]));
	}
	PipelineKeyHash::Mix(hash, static_cast<uint32_t>(key.rendering.depth_format));
	PipelineKeyHash::Mix(hash, static_cast<uint32_t>(key.rendering.stencil_format));
	for (const auto id: key.vertex_shader_ids) {
		PipelineKeyHash::Mix(hash, id);
	}
	PipelineKeyHash::Mix(hash, key.ps_shader_id);
	PipelineKeyHash::Mix(hash, key.vertex_input.binding_count);
	for (uint32_t i = 0; i < key.vertex_input.binding_count; i++) {
		PipelineKeyHash::Mix(hash, key.vertex_input.bindings[i].stride);
		PipelineKeyHash::Mix(hash, key.vertex_input.bindings[i].instance);
	}
	PipelineKeyHash::Mix(hash, key.vertex_input.attribute_count);
	for (uint32_t i = 0; i < key.vertex_input.attribute_count; i++) {
		PipelineKeyHash::Mix(hash, key.vertex_input.attributes[i].offset);
		PipelineKeyHash::Mix(hash, key.vertex_input.attributes[i].binding);
	}
	PipelineKeyHash::Mix(hash, XXH3_64bits(&key.static_params, sizeof(key.static_params)));
	return hash;
}

// Translated-program cache. Thread-safety (phase 1 of shaders.md, "Thread-safe ProgramCache"):
//
//  * `programs_mutex` guards the map structure (find / try_emplace / rehash). Mapped SourceEntry
//    objects are node-stable, so a SourceEntry* stays valid after the lock is dropped; entries are
//    never erased while the cache lives.
//  * Every SourceEntry has its own `mutex` guarding all of its mutable state: `resource_plan`,
//    `resources`, `specialization` and the permutation list. Long work (translation, SPIR-V,
//    module creation) runs outside any lock on copies taken under it.
//  * `permutations` is a deque and published Permutations are immutable, so a Permutation* handed
//    out earlier stays valid while other threads append.
//  * The lookup key is per call (thread_local scratch), and shader ids come from an atomic counter.
//  * `skip_dispatch` is fixed when the entry is created, so it is read without the entry lock.
//
// Port note (581dad7): this base has no S_SWAP_PC call-source linking (ShaderSource/Linked,
// RefreshShaderSource), so none of that snapshot machinery exists here.
//
// Remaining single-owner aliasing (NOT solved here, it needs the captured-job split of phase 2):
// Get() stores `&entry.resources` in input_info.stage.resources. That object is overwritten by the
// next Get() of the same program, so two threads must not use the results of concurrent Get()
// calls for the *same* ProgramKey at the same time. Today there is exactly one caller thread (the
// GPU thread), which is why default behaviour is unchanged.
struct PipelineCache::ProgramCache {
	struct ProgramKey {
		ShaderType            stage           = ShaderType::Unknown;
		uint64_t              hash            = 0;
		uint32_t              user_data_count = 0;
		uint32_t              code_size       = 0;
		std::vector<uint32_t> static_state;

		bool operator==(const ProgramKey&) const = default;
	};

	struct Permutation {
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		ShaderRecompiler::IR::CompiledShaderInfo     program;
		ShaderProgram                                handle;
	};

	struct SourceEntry {
		SourceEntry(ShaderRecompiler::IR::ResourcePlan plan, bool skip)
		    : skip_dispatch(skip), resource_plan(std::move(plan)) {}

		// Re-reads guest memory and refreshes `resources`/`specialization`. Returns a copy of the
		// specialization so callers can compile against exactly what was materialized.
		ShaderRecompiler::IR::ResourceSpecialization
		Materialize(const ShaderRecompiler::IR::SrtRuntime& runtime, StepTime* materialize_time) {
			std::unique_lock lock(mutex);
			MaterializeLocked(runtime, materialize_time);
			return specialization;
		}

		// Materializes, then looks for an already-compiled permutation. `specialization_out`
		// receives the materialized specialization when nothing matches (it is only copied then).
		Permutation* FindPermutation(const ShaderRecompiler::IR::SrtRuntime& runtime,
		                             uint32_t push_data_cursor, StepTime* materialize_time,
		                             ShaderRecompiler::IR::ResourceSpecialization* specialization_out) {
			std::unique_lock lock(mutex);
			MaterializeLocked(runtime, materialize_time);
			Permutation* found = FindExistingLocked(push_data_cursor, specialization);
			// Copy only on a miss: the hit path (every draw) stays allocation-free.
			if (found == nullptr && specialization_out != nullptr) {
				*specialization_out = specialization;
			}
			return found;
		}

		// Lookahead thread: materializes into private storage (never into `resources` /
		// `specialization`, which the GPU thread reads without the lock) and looks for a published
		// permutation. The exclusive lock is still needed: MaterializeResources keeps mutable
		// scratch inside the ResourcePlan (evaluation contexts, read capture), which the GPU
		// thread's FindPermutation uses under the same lock. The lock is registered with the
		// speculation guard so a speculative fatal check releases it before parking the thread.
		Permutation* PeekPermutation(const ShaderRecompiler::IR::SrtRuntime& runtime,
		                             uint32_t                                push_data_cursor,
		                             ShaderRecompiler::IR::ResourceSpecialization& specialization_out,
		                             bool&                                   materialized) {
			std::unique_lock lock(mutex);
			Common::Speculation::HeldLock held(&mutex);
			ShaderRecompiler::IR::ResourceSnapshot snapshot;
			materialized = ShaderRecompiler::IR::MaterializeResources(resource_plan, runtime, snapshot,
			                                                       specialization_out);
			if (!materialized) {
				return nullptr;
			}
			return FindExistingLocked(push_data_cursor, specialization_out);
		}

		bool MaterializePrivate(const ShaderRecompiler::IR::SrtRuntime& runtime,
		                        ShaderRecompiler::IR::ResourceSpecialization& specialization_out) {
			std::unique_lock lock(mutex);
			Common::Speculation::HeldLock held(&mutex);
			ShaderRecompiler::IR::ResourceSnapshot snapshot;
			return ShaderRecompiler::IR::MaterializeResources(resource_plan, runtime, snapshot,
			                                                  specialization_out);
		}

		// Publishes a freshly compiled permutation. If another thread already published an equal
		// one (two threads missing the same program), keeps theirs and sets `duplicate`; in that
		// case `compiled` is left untouched so the caller can release its shader module.
		Permutation* Publish(Permutation& compiled, uint32_t push_data_cursor, bool& duplicate) {
			std::unique_lock lock(mutex);
			Permutation*     existing = FindExistingLocked(push_data_cursor, compiled.specialization);
			duplicate = existing != nullptr;
			if (duplicate) {
				return existing;
			}
			return &permutations.emplace_back(std::move(compiled));
		}

		// Looks for an already-published permutation without touching guest memory. Used by the
		// async path to close the window between "not in flight" and "published".
		Permutation* FindPublished(uint32_t push_data_cursor,
		                           const ShaderRecompiler::IR::ResourceSpecialization& wanted) {
			std::shared_lock lock(mutex);
			return FindExistingLocked(push_data_cursor, wanted);
		}

		const bool                                   skip_dispatch;
		ShaderRecompiler::IR::ResourcePlan           resource_plan;
		ShaderRecompiler::IR::ResourceSnapshot       resources;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		std::deque<Permutation>                      permutations;
		mutable std::shared_mutex                    mutex;

	private:
		void MaterializeLocked(const ShaderRecompiler::IR::SrtRuntime& runtime,
		                       StepTime*                                materialize_time) {
			Stopwatch  materialize_sw;
			const bool materialized = ShaderRecompiler::IR::MaterializeResources(
			    resource_plan, runtime, resources, specialization);
			if (materialize_time != nullptr) materialize_time->Add(materialize_sw, materialize_sw.Lap());
			EXIT_IF(!materialized);
		}

		Permutation* FindExistingLocked(uint32_t push_data_cursor,
		                                const ShaderRecompiler::IR::ResourceSpecialization& wanted) {
			for (auto& candidate: permutations) {
				const auto& layout = candidate.program.bindings;
				if (layout.push_data_start_dword == ShaderRecompiler::IR::PushData::StartFor(
				        push_data_cursor, layout.ShaderDataDwords()) &&
				    candidate.specialization == wanted) {
					return &candidate;
				}
			}
			return nullptr;
		}
	};

	struct ProgramKeyHash {
		std::size_t operator()(const ProgramKey& key) const {
			std::size_t hash = static_cast<std::size_t>(key.stage);
			PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.hash));
			if constexpr (sizeof(std::size_t) < sizeof(uint64_t)) {
				PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.hash >> 32u));
			}
			PipelineKeyHash::Mix(hash, key.user_data_count);
			PipelineKeyHash::Mix(hash, key.code_size);
			PipelineKeyHash::Mix(hash, key.static_state.size());
			// Bucket same-shape static variants by source. ProgramKey equality performs the one
			// exact state comparison needed on a stable hit without hashing the full state first.
			return hash;
		}
	};

	static constexpr std::size_t MaxStaticKeyWords = 32 + ShaderVertexInputInfo::RES_MAX * 6;

	Permutation CompilePermutation(const char*                                  stage_name,
	                               const ShaderRecompiler::CompileOptions&      options,
	                               ShaderRecompiler::TranslateResult            translated,
	                               ShaderRecompiler::IR::ResourceSpecialization specialization,
	                               uint32_t push_data_start_dword,
	                               int64_t* backend_ns = nullptr, int64_t* module_ns = nullptr) {
		Stopwatch backend_sw;
		auto result = ShaderRecompiler::CompileProgram(std::move(translated), options,
		                                               specialization, push_data_start_dword);
		if (!ValidateShaderSpirv(options.dump_label, options.shader_hash, result.spirv)) {
			DumpShaderSpirv(stage_name, options.shader_hash, result.spirv);
			EXIT("%s failed hash=0x%016" PRIx64 ": SPIR-V validation failed\n", options.dump_label,
			     options.shader_hash);
		}
		DumpShaderSpirv(stage_name, options.shader_hash, result.spirv);
		if (backend_ns != nullptr) *backend_ns = backend_sw.Lap();

		Stopwatch module_sw;
		const auto module = CompileSPV(result.spirv, device);
		if (module_ns != nullptr) *module_ns = module_sw.Lap();
		EXIT_IF(module == nullptr);
		if (options.dump_ir) {
			LOGF("%s SPIR-V words=%" PRIu64 " wave_size=%u\n", options.dump_label,
			     static_cast<uint64_t>(result.spirv.size()), options.wave_size);
		}
		return {
		    .specialization = std::move(specialization),
		    .program        = std::move(result.program).TakeCompiledInfo(),
		    .handle         = {.id = next_shader_id.fetch_add(1, std::memory_order_relaxed) + 1,
	                       .module = module},
		};
	}

	// ---- --async-pipelines: program jobs -----------------------------------------------------
	//
	// A program job translates (when the GPU thread has not already) and compiles one shader
	// permutation on a worker and publishes it into its SourceEntry. Everything it needs is
	// captured by value on the GPU thread: guest code bytes, user_data, back_code, wave_size,
	// the stage input info, the resolved ResourceSpecialization (hashed at capture) and the push
	// data cursor. MaterializeResources (guest memory reads) and the ResourcePlan scratch stay on
	// the GPU thread; the worker never touches `entry->resource_plan` or `entry->resources`.

	// Identity of a permutation request: the program, the materialized specialization and the
	// push-data cursor the layout is allocated at.
	struct ProgramJobKey {
		ProgramKey                                   program;
		std::size_t                                  spec_hash = 0;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		uint32_t                                     push_data_cursor = 0;

		bool operator==(const ProgramJobKey& other) const {
			return spec_hash == other.spec_hash && push_data_cursor == other.push_data_cursor &&
			       program == other.program && specialization == other.specialization;
		}
	};

	struct ProgramJobKeyHash {
		std::size_t operator()(const ProgramJobKey& key) const {
			std::size_t hash = ProgramKeyHash {}(key.program);
			PipelineKeyHash::Mix(hash, key.spec_hash);
			PipelineKeyHash::Mix(hash, key.push_data_cursor);
			return hash;
		}
	};

	struct ProgramJob final: AsyncJob {
		ProgramJob(ProgramCache& owner, SourceEntry* source, ProgramJobKey job_key,
		           uint64_t key_hash)
		    : AsyncJob(Kind::Program, key_hash), cache(owner), entry(source),
		      key(std::move(job_key)) {}

		void Execute() override {
			const bool timing = Bench::TimingEnabled();
			Stopwatch  sw;
			ShaderRecompiler::CompileOptions options;
			options.stage          = stage;
			options.shader_hash    = shader_hash;
			options.user_data      = user_data;
			options.back_code      = back_code;
			options.dump_ir        = dump_ir;
			options.early_dump     = dump_ir;
			options.dump_label     = label;
			options.wave_size      = wave_size;
			options.user_data_base = user_data_base;
			if (vertex_info) {
				options.input_info.vertex = &*vertex_info;
			} else if (pixel_info) {
				options.input_info.pixel = &*pixel_info;
			} else {
				options.input_info.compute = &*compute_info;
			}
			int64_t                           frontend = frontend_ns;
			int64_t                           backend_ns = 0, module_ns = 0;
			ShaderRecompiler::TranslateResult translated;
			if (pre_translated) {
				translated = std::move(*pre_translated);
				pre_translated.reset();
			} else {
				translated = ShaderRecompiler::TranslateProgram(code, options);
				frontend   = sw.Lap();
				EXIT_IF(translated.skip_dispatch);
			}
			auto compiled = cache.CompilePermutation(
			    stage_name, options, std::move(translated), key.specialization,
			    key.push_data_cursor, timing ? &backend_ns : nullptr, timing ? &module_ns : nullptr);
			bool dup = false;
			auto* const published = entry->Publish(compiled, key.push_data_cursor, dup);
			if (dup) {
				cache.device.destroyShaderModule(compiled.handle.module, nullptr);
			} else {
				cache.stage_permutation_counts[static_cast<size_t>(stage)].fetch_add(
				    1, std::memory_order_relaxed);
			}
			result    = published;
			duplicate = dup;
			if (timing) {
				const auto translate_ns = frontend + plan_ns + backend_ns + module_ns;
				AddTimingRow(
				    "program_translate", program_key_hash, t_capture_ns, translate_ns,
				    fmt::format("stage={};shader_hash={:016x};shader_id={};new_source={};"
				                "reused_permutation={};prepare_ns=0;frontend_ns={};plan_ns={};"
				                "materialize_ns={};backend_ns={};module_ns={};code_dwords={}",
				                StageShortName(stage), shader_hash, published->handle.id,
				                new_source ? 1 : 0, dup ? 1 : 0, frontend, plan_ns, materialize_ns,
				                backend_ns, module_ns, code_dwords));
			}
			cache.PrintShaderCounts();
			// Publish first, unregister last: "not registered" must imply "published".
			cache.program_jobs.Erase(key);
		}

		void Cancel() override { cache.program_jobs.Erase(key); }

		ProgramCache& cache;
		SourceEntry*  entry;
		ProgramJobKey key;

		// Captured inputs.
		ShaderType            stage          = ShaderType::Unknown;
		const char*           label          = nullptr;
		const char*           stage_name     = nullptr;
		uint64_t              shader_hash    = 0;
		uint32_t              wave_size      = 64;
		uint32_t              user_data_base = 0;
		bool                  dump_ir        = false;
		std::vector<uint32_t> code;      // empty when the GPU thread already translated
		std::vector<uint32_t> back_code;
		std::vector<uint32_t> user_data;
		std::optional<ShaderVertexInputInfo>             vertex_info;
		std::optional<ShaderPixelInputInfo>              pixel_info;
		std::optional<ShaderComputeInputInfo>            compute_info;
		std::optional<ShaderRecompiler::TranslateResult> pre_translated;

		// Capture-side numbers for the program_translate timing row.
		bool     new_source        = false;
		int64_t  frontend_ns       = 0;
		int64_t  plan_ns           = 0;
		int64_t  materialize_ns    = 0;
		int64_t  t_capture_ns      = 0;
		uint64_t code_dwords       = 0;
		uint64_t program_key_hash  = 0;

		// Result, valid after a successful Wait().
		Permutation* result    = nullptr;
		bool         duplicate = false;
	};

	struct AsyncContext {
		AsyncPipelineCompiler* pool       = nullptr;
		bool                   allow_skip = false; // graphics only; compute always waits
		bool                   pending    = false; // out: skipped, program not ready
		// Lookahead thread (--pipeline-lookahead): materialize privately, never skip, wait for
		// jobs without accounting GPU-thread waits, never fall back to synchronous compilation.
		bool prefetch = false;
	};

	void PrintShaderCounts() {
		// Guest geometry shaders are compiled through the host mesh stage.
		const auto count = [this](ShaderType type) {
			return stage_permutation_counts[static_cast<size_t>(type)].load(std::memory_order_relaxed);
		};
		std::printf("Shaders: VS %zu | PS %zu | CS %zu | GS %zu | LS %zu | HS %zu | TES %zu\n",
		            count(ShaderType::Vertex), count(ShaderType::Pixel), count(ShaderType::Compute),
		            count(ShaderType::Mesh), count(ShaderType::Local),
		            count(ShaderType::TessellationControl), count(ShaderType::TessellationEvaluation));
	}

	template <typename InputInfo>
	ShaderProgram Get(const ShaderParams& params, InputInfo& input_info,
	                  uint32_t& push_data_cursor, AsyncContext* async = nullptr) {
		ShaderType stage;
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			stage = input_info.logical_stage;
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			stage = ShaderType::Pixel;
		} else {
			static_assert(std::is_same_v<InputInfo, ShaderComputeInputInfo>);
			stage = ShaderType::Compute;
		}

		const auto user_data = std::span(params.user_data).first(params.user_data_count);
		// Per-call key. A thread_local scratch object avoids a heap allocation per draw (the
		// static_state vector keeps its capacity) while never being shared between threads or
		// between calls: Get() is not re-entrant on one thread.
		thread_local ProgramKey lookup_key = [] {
			ProgramKey key;
			key.static_state.reserve(MaxStaticKeyWords);
			return key;
		}();
		lookup_key.stage           = stage;
		lookup_key.hash            = params.hash;
		lookup_key.user_data_count = params.user_data_count;
		lookup_key.code_size       = static_cast<uint32_t>(params.code.size());
		BuildStageStaticKey(input_info, lookup_key.static_state);
		const bool timing = Bench::TimingEnabled();
		StepTime   materialize_time;
		const auto program_key_hash = timing ? static_cast<uint64_t>(ProgramKeyHash {}(lookup_key)) : 0u;
		// Hit-path steps are aggregated; on a miss (or when slow) they are logged individually.
		const auto report_steps = [&](bool miss) {
			if (!timing) return;
			const bool gpu_thread = GuestGpu::IsGpuThread();
			const auto report = [&](const char* kind, Bench::HitSlot slot, const StepTime& step) {
				if (step.t_start == 0) return;
				if (miss || step.dur >= Bench::kSlowHitNs) {
					Bench::LogTiming(kind, program_key_hash, step.t_start, step.dur, gpu_thread,
					                 fmt::format("miss={};stage={};shader_hash={:016x}", miss ? 1 : 0,
					                             StageShortName(stage), params.hash));
				}
				if (!miss) Bench::AccumulateHit(slot, step.dur);
			};
			report("materialize", Bench::HitSlot::Materialize, materialize_time);
		};
		SourceEntry* entry = FindEntry(lookup_key);
		if (entry != nullptr && entry->skip_dispatch) {
			return {};
		}
		const bool prefetch = async != nullptr && async->prefetch;
		const ShaderRecompiler::IR::SrtRuntime       runtime {
		    .user_data                  = user_data,
		    .shader_base                = params.Base(),
		    .read_specialization_memory =
		        prefetch ? ReadShaderGuestMemorySpeculative : ReadShaderGuestMemory,
		};
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		if (entry != nullptr) {
			if (prefetch) {
				bool materialized = false;
				if (auto* permutation = entry->PeekPermutation(runtime, push_data_cursor,
				                                               specialization, materialized)) {
					input_info.stage = {.program = &permutation->program, .resources = &PrefetchResources()};
					permutation->program.bindings.AdvancePushData(push_data_cursor);
					return permutation->handle;
				}
				if (!materialized) {
					return {};
				}
			} else if (auto* permutation = entry->FindPermutation(runtime, push_data_cursor,
			                                                      &materialize_time,
			                                                      &specialization)) {
				input_info.stage = {.program = &permutation->program, .resources = &entry->resources};
				permutation->program.bindings.AdvancePushData(push_data_cursor);
				report_steps(false);
				return permutation->handle;
			}
		}
		if (prefetch && (async->pool == nullptr || !async->pool->Running())) {
			return {};
		}

		// --async-pipelines. `pool` is null in Off mode (and once the pool has been shut down), in
		// which case everything below is exactly the stock synchronous path.
		AsyncPipelineCompiler* const pool =
		    (async != nullptr && async->pool != nullptr && async->pool->Running()) ? async->pool
		                                                                           : nullptr;
		const bool may_skip = pool != nullptr && async->allow_skip && stage != ShaderType::Compute;
		// Uses a permutation another thread (or an earlier request) published for this draw.
		const auto use_published = [&](Permutation& permutation, bool miss_report) {
			input_info.stage = {.program = &permutation.program,
			                    .resources = prefetch ? &PrefetchResources() : &entry->resources};
			permutation.program.bindings.AdvancePushData(push_data_cursor);
			report_steps(miss_report);
			return permutation.handle;
		};
		const auto wait_job = [&](AsyncJob& job) {
			return prefetch ? job.Wait() : pool->WaitFor(job);
		};
		const auto note_skip = [&] {
			pool->Stats().skipped_program_draws.fetch_add(1, std::memory_order_relaxed);
			async->pending = true;
			report_steps(false);
		};
		ProgramJobKey job_key;
		if (pool != nullptr && entry != nullptr) {
			// Known source, new permutation: is it already being built?
			job_key.program          = lookup_key;
			job_key.specialization   = specialization;
			job_key.spec_hash        = HashSpecialization(specialization);
			job_key.push_data_cursor = push_data_cursor;
			if (auto in_flight = program_jobs.Find(job_key, may_skip)) {
				if (may_skip) {
					note_skip();
					return {};
				}
				if (wait_job(*in_flight)) {
					if (auto* result = std::static_pointer_cast<ProgramJob>(in_flight)->result) {
						return use_published(*result, true);
					}
				}
				if (prefetch) {
					return {};
				}
				// Cancelled by a shutdown: fall back to the synchronous path below.
			} else if (auto* published = entry->FindPublished(push_data_cursor, specialization)) {
				return use_published(*published, true);
			}
		}

		Stopwatch miss_sw;
		int64_t   prepare_ns = 0, frontend_ns = 0, plan_ns = 0, backend_ns = 0, module_ns = 0;
		const bool new_source = entry == nullptr;

		ShaderStageInputInfo stage_input {};
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			stage_input.vertex = &input_info;
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			stage_input.pixel = &input_info;
		} else {
			stage_input.compute = &input_info;
		}
		const char* label = nullptr;
		const char* stage_name = nullptr;
		switch (stage) {
			case ShaderType::Vertex: label = "ShaderRecompiler VS"; stage_name = "vs"; break;
			case ShaderType::Mesh: label = "ShaderRecompiler MS"; stage_name = "ms"; break;
			case ShaderType::Local: label = "ShaderRecompiler LS"; stage_name = "ls"; break;
			case ShaderType::TessellationControl: label = "ShaderRecompiler HS"; stage_name = "hs"; break;
			case ShaderType::TessellationEvaluation: label = "ShaderRecompiler DS"; stage_name = "ds"; break;
			case ShaderType::Pixel: label = "ShaderRecompiler PS"; stage_name = "ps"; break;
			case ShaderType::Compute: label = "ShaderRecompiler CS"; stage_name = "cs"; break;
			default: EXIT("invalid pipeline shader stage\n");
		}
		ShaderRecompiler::CompileOptions options;
		options.stage       = stage;
		options.shader_hash = params.hash;
		options.user_data   = user_data;
		options.back_code      = params.back_code;
		options.dump_ir     = Config::GetShaderLogDirection() != Config::LogDirection::Silent;
		options.early_dump  = options.dump_ir;
		options.dump_label  = label;
		options.input_info  = stage_input;

		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			options.user_data_base = 8;
			options.wave_size = input_info.wave_size;
			if (stage == ShaderType::Mesh || stage == ShaderType::TessellationControl) {
				options.user_data_base = 0;
				options.wave_size = stage == ShaderType::Mesh ? input_info.mesh.wave_size : 64u;
			}
		} else {
			options.wave_size = input_info.wave_size;
		}
		DumpShaderOriginal(stage_name, options.shader_hash, params.code);

		// Captures a job: every input the worker needs, by value.
		const auto make_job = [&](bool pre_translated) {
			auto job = std::make_shared<ProgramJob>(*this, entry, job_key,
			                                        static_cast<uint64_t>(ProgramKeyHash {}(lookup_key)));
			job->stage          = stage;
			job->label          = label;
			job->stage_name     = stage_name;
			job->shader_hash    = params.hash;
			job->wave_size      = options.wave_size;
			job->user_data_base = options.user_data_base;
			job->dump_ir        = options.dump_ir;
			job->user_data.assign(user_data.begin(), user_data.end());
			job->back_code.assign(params.back_code.begin(), params.back_code.end());
			if (!pre_translated) {
				job->code.assign(params.code.begin(), params.code.end());
			}
			if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
				job->vertex_info = input_info;
			} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
				job->pixel_info = input_info;
			} else {
				job->compute_info = input_info;
			}
			job->new_source        = new_source;
			job->code_dwords       = params.code.size();
			job->program_key_hash  = program_key_hash;
			job->t_capture_ns      = miss_sw.Start();
			return job;
		};
		// Registers and queues `job`, then skips (graphics, skip mode) or waits for it. Returns
		// the published permutation, or nullptr when the caller must continue synchronously;
		// `skipped` tells the caller to give up on the draw.
		const auto run_job = [&](const std::shared_ptr<ProgramJob>& job, bool& skipped) -> Permutation* {
			skipped = false;
			bool inserted = false;
			auto registered = program_jobs.Insert(job_key, job, inserted);
			if (inserted) {
				if (!pool->Submit(job)) {
					program_jobs.Erase(job_key);
					return nullptr;
				}
				if (may_skip) job->CountSkippedDraw();
			} else if (may_skip) {
				registered->CountSkippedDraw();
			}
			if (may_skip) {
				skipped = true;
				return nullptr;
			}
			if (wait_job(*registered)) {
				return std::static_pointer_cast<ProgramJob>(registered)->result;
			}
			return nullptr;
		};

		ShaderRecompiler::TranslateResult translated;
		if (pool != nullptr && entry != nullptr) {
			// Known source: the worker translates from the copied code bytes and compiles.
			const auto job = make_job(false);
			bool skipped = false;
			if (auto* result = run_job(job, skipped)) {
				return use_published(*result, true);
			}
			if (skipped) {
				note_skip();
				return {};
			}
			if (prefetch) {
				return {};
			}
			// Pool stopped or job cancelled: stock path below translates and compiles here.
		}
		translated = ShaderRecompiler::TranslateProgram(params.code, options);
		prepare_ns = 0; // no separate source-preparation step at this base (no S_SWAP_PC linking)
		frontend_ns = miss_sw.Lap();
		if (translated.skip_dispatch) {
			InsertEntry(lookup_key, ShaderRecompiler::IR::ResourcePlan {}, true);
			return {};
		}
		// One row per program translation (a miss). dur_ns = translate_ns = prepare + frontend +
		// plan + backend + module; MaterializeResources is reported on
		// its own row (miss=1), and the detail field carries materialize_ns for convenience.
		const auto log_translate = [&](bool reused, uint64_t shader_id) {
			if (!timing) return;
			const auto translate_ns = prepare_ns + frontend_ns + plan_ns + backend_ns + module_ns;
			Bench::LogTiming(
			    "program_translate", program_key_hash, miss_sw.Start(), translate_ns,
			    GuestGpu::IsGpuThread(),
			    fmt::format("stage={};shader_hash={:016x};shader_id={};new_source={};reused_permutation={};"
			                "prepare_ns={};frontend_ns={};plan_ns={};materialize_ns={};backend_ns={};"
			                "module_ns={};code_dwords={}",
			                StageShortName(stage), params.hash, shader_id, new_source ? 1 : 0,
			                reused ? 1 : 0, prepare_ns, frontend_ns, plan_ns, materialize_time.dur,
			                backend_ns, module_ns, params.code.size()));
		};
		if (entry == nullptr) {
			entry = InsertEntry(lookup_key, ShaderRecompiler::IR::ExtractResourcePlan(translated.program),
			                    false);
			plan_ns = miss_sw.Lap();
			if (prefetch) {
				if (!entry->MaterializePrivate(runtime, specialization)) {
					return {};
				}
			} else {
				specialization = entry->Materialize(runtime, &materialize_time);
			}
			miss_sw.Lap();
			if (pool != nullptr) {
				// New source: the GPU thread has translated (the resource plan comes out of the
				// translation) and materialized; the worker compiles the translated program.
				job_key.program          = lookup_key;
				job_key.specialization   = specialization;
				job_key.spec_hash        = HashSpecialization(specialization);
				job_key.push_data_cursor = push_data_cursor;
				const auto job = make_job(true);
				job->frontend_ns    = frontend_ns;
				job->plan_ns        = plan_ns;
				job->materialize_ns = materialize_time.dur;
				job->pre_translated = std::move(translated);
				bool skipped = false;
				if (auto* result = run_job(job, skipped)) {
					return use_published(*result, true);
				}
				if (skipped) {
					note_skip();
					return {};
				}
				if (prefetch) {
					return {};
				}
				// Pool stopped or job cancelled: take the translated program back and continue
				// synchronously.
				if (job->pre_translated) {
					translated = std::move(*job->pre_translated);
					job->pre_translated.reset();
				} else {
					EXIT("async program job lost its translated program\n");
				}
			}
		}
		auto compiled = CompilePermutation(
		    stage_name, options, std::move(translated), std::move(specialization), push_data_cursor,
		    timing ? &backend_ns : nullptr, timing ? &module_ns : nullptr);
		bool        duplicate = false;
		auto* const published = entry->Publish(compiled, push_data_cursor, duplicate);
		if (duplicate) {
			// Another thread published an equal permutation first: drop our module, use theirs.
			device.destroyShaderModule(compiled.handle.module, nullptr);
		} else {
			stage_permutation_counts[static_cast<size_t>(stage)].fetch_add(1, std::memory_order_relaxed);
		}
		const auto& permutation = *published;
		input_info.stage = {.program = &permutation.program, .resources = &entry->resources};
		permutation.program.bindings.AdvancePushData(push_data_cursor);
		log_translate(duplicate, permutation.handle.id);
		report_steps(true);

		PrintShaderCounts();
		return permutation.handle;
	}

	explicit ProgramCache(vk::Device device): device(device) {}
	~ProgramCache() {
		for (const auto& [key, entry]: programs) {
			(void)key;
			for (const auto& permutation: entry.permutations) {
				device.destroyShaderModule(permutation.handle.module, nullptr);
			}
		}
	}

	[[nodiscard]] SourceEntry* FindEntry(const ProgramKey& key) {
		std::lock_guard lock(programs_mutex);
		const auto      iter = programs.find(key);
		return iter != programs.end() ? &iter->second : nullptr;
	}

	// Inserts a new entry, or returns the one another thread inserted first (the caller then
	// treats it like any existing entry; its own translation result is simply discarded).
	SourceEntry* InsertEntry(const ProgramKey& key, ShaderRecompiler::IR::ResourcePlan plan,
	                         bool skip_dispatch) {
		std::lock_guard lock(programs_mutex);
		return &programs.try_emplace(key, std::move(plan), skip_dispatch).first->second;
	}

	std::mutex                                                  programs_mutex;
	std::unordered_map<ProgramKey, SourceEntry, ProgramKeyHash> programs;
	// In-flight --async-pipelines program jobs, keyed by (program, specialization, push cursor).
	InFlightRegistry<ProgramJobKey, ProgramJobKeyHash>          program_jobs;
	vk::Device                                                  device;
	std::atomic<uint64_t>                                       next_shader_id {0};
	// Number of published permutations per logical stage, kept incrementally so the "Shaders:"
	// progress line does not have to walk (and lock) the whole map on every miss.
	std::array<std::atomic<size_t>, static_cast<size_t>(ShaderType::TessellationEvaluation) + 1>
	    stage_permutation_counts {};
};

PipelineCache::PipelineCache(GraphicContext& graphics)
    : m_graphics(graphics), m_program_cache(std::make_unique<ProgramCache>(graphics.device)),
      m_async_mode(Config::GetAsyncPipelineMode()) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	InitializeDriverCache();
	if (m_async_mode != Config::AsyncPipelineMode::Off) {
		const auto workers = Config::GetPipelineWorkerCount();
		m_async            = std::make_unique<AsyncPipelineCompiler>(m_async_mode, workers);
		PipelineCacheLog("Async pipelines: mode={} workers={}", AsyncModeName(m_async_mode),
		                 m_async->WorkerCount());
	}
}

AsyncPipelineCompiler* PipelineCache::AsyncPool() {
	return m_async != nullptr && m_async->Running() ? m_async.get() : nullptr;
}

void PipelineCache::ShutdownAsync() {
	if (m_async == nullptr || !m_async->Running()) {
		return; // off, or already stopped
	}
	{
		// Wait for a running prefetch call to finish and keep new ones out.
		std::unique_lock gate(m_prefetch_gate);
		m_prefetch_stopped = true;
	}
	// Workers must be gone before the driver VkPipelineCache is read and destroyed (Save()),
	// before the Vulkan device is destroyed and before Config::Shutdown(). Queued jobs are
	// cancelled, running ones finish (and publish). Callers holding the render context lock
	// (Save() from WindowRun) cannot race with a draw waiting on a job.
	m_async->Shutdown();
	m_program_cache->program_jobs.Clear();
	m_graphics_jobs.Clear();
	m_compute_jobs.Clear();
	PipelineCacheLog("Async pipelines: stopped ({})", m_async->Summary());
	m_async->LogSummary();
	if (Config::GetPipelineLookahead()) {
		const auto& l = m_lookahead;
		const auto  summary = fmt::format(
		    "cs_seen={};cs_skipped={};cs_requested={};cs_already={};cs_hit_ready={};"
		    "cs_hit_inflight={};cs_miss={};gfx_seen={};gfx_skipped={};gfx_requested={};"
		    "gfx_already={};gfx_hit_ready={};gfx_hit_inflight={};gfx_miss={};programs_requested={}",
		    l.cs_seen.load(), l.cs_skipped.load(), l.cs_requested.load(), l.cs_already.load(),
		    l.cs_hit_ready.load(), l.cs_hit_inflight.load(), l.cs_miss.load(), l.gfx_seen.load(),
		    l.gfx_skipped.load(), l.gfx_requested.load(), l.gfx_already.load(),
		    l.gfx_hit_ready.load(), l.gfx_hit_inflight.load(), l.gfx_miss.load(),
		    l.programs_requested.load());
		PipelineCacheLog("Pipeline lookahead: {}", summary);
		if (Bench::TimingEnabled()) {
			Bench::LogTiming("summary_lookahead", 0, Bench::NowNs(), 0, false, summary);
		}
	}
}

PipelineCache::Pipeline* PipelineCache::FindGraphicsPipeline(const GraphicsPipelineKey& key) {
	std::shared_lock<std::shared_mutex> lock(m_pipelines_mutex, std::defer_lock);
	if (m_async_mode != Config::AsyncPipelineMode::Off) {
		lock.lock();
	}
	const auto iter = m_graphics_pipelines.find(key);
	return iter != m_graphics_pipelines.end() ? iter->second.get() : nullptr;
}

PipelineCache::Pipeline* PipelineCache::FindComputePipeline(uint64_t program_id) {
	std::shared_lock<std::shared_mutex> lock(m_pipelines_mutex, std::defer_lock);
	if (m_async_mode != Config::AsyncPipelineMode::Off) {
		lock.lock();
	}
	const auto iter = m_compute_pipelines.find(program_id);
	return iter != m_compute_pipelines.end() ? iter->second.get() : nullptr;
}

PipelineCache::Pipeline* PipelineCache::PublishGraphicsPipeline(const GraphicsPipelineKey& key,
                                                                std::unique_ptr<Pipeline>& pipeline,
                                                                bool& inserted) {
	std::unique_lock<std::shared_mutex> lock(m_pipelines_mutex, std::defer_lock);
	if (m_async_mode != Config::AsyncPipelineMode::Off) {
		lock.lock();
	}
	auto [iter, was_inserted] = m_graphics_pipelines.try_emplace(key, nullptr);
	inserted                  = was_inserted;
	if (was_inserted) {
		iter->second = std::move(pipeline);
	}
	return iter->second.get();
}

PipelineCache::Pipeline* PipelineCache::PublishComputePipeline(uint64_t program_id,
                                                               std::unique_ptr<Pipeline>& pipeline,
                                                               bool& inserted) {
	std::unique_lock<std::shared_mutex> lock(m_pipelines_mutex, std::defer_lock);
	if (m_async_mode != Config::AsyncPipelineMode::Off) {
		lock.lock();
	}
	auto [iter, was_inserted] = m_compute_pipelines.try_emplace(program_id, nullptr);
	inserted                  = was_inserted;
	if (was_inserted) {
		iter->second = std::move(pipeline);
	}
	return iter->second.get();
}

void PipelineCache::DestroyPipeline(Pipeline& pipeline) {
	m_graphics.device.destroyPipeline(pipeline.pipeline, nullptr);
	m_graphics.device.destroyPipelineLayout(pipeline.pipeline_layout, nullptr);
	m_graphics.device.destroyDescriptorSetLayout(pipeline.descriptor_set_layout, nullptr);
}

PipelineCache::~PipelineCache() {
	Save(); // also stops the async workers
	auto destroy = [this](const auto& pipelines) {
		for (const auto& [key, pipeline]: pipelines) {
			(void)key;
			m_graphics.device.destroyPipeline(pipeline->pipeline, nullptr);
			m_graphics.device.destroyPipelineLayout(pipeline->pipeline_layout, nullptr);
			m_graphics.device.destroyDescriptorSetLayout(pipeline->descriptor_set_layout, nullptr);
		}
	};
	destroy(m_graphics_pipelines);
	destroy(m_compute_pipelines);
	if (m_driver_cache != nullptr) {
		m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
	}
}

void PipelineCache::InitializeDriverCache() {
	// --shader-timing-log: one row so analysis can tell cold (empty cache) from warm sessions.
	Stopwatch   init_sw;
	size_t      loaded_bytes = 0;
	const char* init_result  = "disabled";
	struct InitReport {
		const Stopwatch& sw;
		const size_t&    loaded_bytes;
		const char* const& result;
		const PipelineCache& cache;
		~InitReport() {
			if (!sw.On()) return;
			Bench::LogTiming("driver_cache_init", 0, sw.Start(), sw.Elapsed(), false,
			                 fmt::format("result={};loaded_bytes={};active={}", result, loaded_bytes,
			                             cache.m_driver_cache != nullptr ? 1 : 0));
		}
	} init_report {init_sw, loaded_bytes, init_result, *this};
	const auto title_id = PipelineCacheTitleId();
	if (title_id.empty()) {
		return;
	}
	if (Config::GetPipelineCacheKey() == Config::PipelineCacheKey::Schema) {
		// The signature does not involve the git revision, so none of the build-identity gates
		// below apply: dev, dirty and unknown-revision builds may use (and share) the cache.
		PipelineCacheLog("Vulkan pipeline cache: key=schema (version {})", kPipelineCacheSchemaVersion);
	} else {
		if (KYTY_BUILD != KYTY_BUILD_RELEASE) {
			PipelineCacheLog("Vulkan pipeline cache: disabled (non-Release build)");
			return;
		}
		const std::string_view git_hash     = KYTY_GIT_HASH;
		const std::string_view git_revision = KYTY_GIT_REVISION;
		if (git_hash == "unknown" || git_revision == "unknown") {
			PipelineCacheLog("Vulkan pipeline cache: disabled (unknown git revision)");
			return;
		}
		if (git_hash.ends_with("-dirty")) {
			PipelineCacheLog("Vulkan pipeline cache: disabled (dirty build)");
			return;
		}
	}

	m_driver_cache_path     = std::filesystem::path("_PipelineCache") / (title_id + ".bin");
	const auto path         = Common::PathToString(m_driver_cache_path);
	const bool cache_exists = Common::File::IsFileExisting(m_driver_cache_path);
	if (cache_exists) {
		PipelineCacheLog("Vulkan pipeline cache: loading {}", path);
	} else {
		PipelineCacheLog("Vulkan pipeline cache: initializing {}", path);
	}
	std::vector<uint8_t> initial_data;
	if (cache_exists) {
		Common::File file(m_driver_cache_path, Common::File::Mode::Read);
		const auto   file_size = file.IsInvalid() ? 0 : file.Size();
		const auto   signature = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
		if (file_size >= signature.size() + sizeof(uint64_t) &&
		    file_size <= std::numeric_limits<uint32_t>::max()) {
			std::string cached_signature(signature.size(), '\0');
			uint64_t    payload_hash = 0;
			initial_data.resize(file_size - signature.size() - sizeof(payload_hash));
			uint32_t signature_read = 0;
			uint32_t hash_read      = 0;
			uint32_t payload_read   = 0;
			file.Read(cached_signature.data(), static_cast<uint32_t>(cached_signature.size()),
			          &signature_read);
			file.Read(&payload_hash, sizeof(payload_hash), &hash_read);
			file.Read(initial_data.data(), static_cast<uint32_t>(initial_data.size()),
			          &payload_read);
			file.Close();
			if (signature_read != cached_signature.size() || hash_read != sizeof(payload_hash) ||
			    payload_read != initial_data.size() || cached_signature != signature ||
			    XXH3_64bits(initial_data.data(), initial_data.size()) != payload_hash) {
				initial_data.clear();
				PipelineCacheLog(
				    "Vulkan pipeline cache: invalidating {} (driver, emulator, or data mismatch)",
				    path);
			}
		} else {
			file.Close();
			PipelineCacheLog("Vulkan pipeline cache: invalidating {} (invalid file size)", path);
		}
	}

	vk::PipelineCacheCreateInfo create {};
	create.initialDataSize = initial_data.size();
	create.pInitialData    = initial_data.empty() ? nullptr : initial_data.data();
	auto result = m_graphics.device.createPipelineCache(&create, nullptr, &m_driver_cache);
	if (result != vk::Result::eSuccess && !initial_data.empty()) {
		PipelineCacheLog("Vulkan pipeline cache: driver rejected {} ({}); starting empty", path,
		                 vk::to_string(result));
		initial_data.clear();
		create.initialDataSize = 0;
		create.pInitialData    = nullptr;
		result = m_graphics.device.createPipelineCache(&create, nullptr, &m_driver_cache);
	}
	if (result != vk::Result::eSuccess) {
		PipelineCacheLog("Vulkan pipeline cache: disabled ({})", vk::to_string(result));
		m_driver_cache = nullptr;
		return;
	}
	if (!initial_data.empty()) {
		PipelineCacheLog("Vulkan pipeline cache: loaded {} bytes from {}", initial_data.size(),
		                 path);
		loaded_bytes = initial_data.size();
		init_result  = "loaded";
	} else {
		PipelineCacheLog("Vulkan pipeline cache: initialized empty");
		init_result = "empty";
	}
}

void PipelineCache::Save() {
	// Join the workers first: they use m_driver_cache, and it is destroyed below.
	ShutdownAsync();
	if (m_driver_cache == nullptr) {
		return;
	}

	size_t               size = 0;
	vk::Result           result;
	std::vector<uint8_t> payload;
	for (uint32_t attempt = 0; attempt < 3; attempt++) {
		size   = 0;
		result = m_graphics.device.getPipelineCacheData(m_driver_cache, &size, nullptr);
		if (result != vk::Result::eSuccess || size == 0 ||
		    size > std::numeric_limits<uint32_t>::max()) {
			break;
		}
		payload.resize(size);
		result = m_graphics.device.getPipelineCacheData(m_driver_cache, &size, payload.data());
		if (result != vk::Result::eIncomplete) {
			break;
		}
	}
	if (result != vk::Result::eSuccess || size == 0 ||
	    size > std::numeric_limits<uint32_t>::max()) {
		PipelineCacheLog("Vulkan pipeline cache: save failed ({}, {} bytes)",
		                 vk::to_string(result), size);
		return;
	}
	payload.resize(size);
	auto       prefix       = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
	const auto payload_hash = XXH3_64bits(payload.data(), payload.size());
	prefix.append(reinterpret_cast<const char*>(&payload_hash), sizeof(payload_hash));
	if (!Common::File::CreateDirectories(m_driver_cache_path.parent_path())) {
		PipelineCacheLog("Vulkan pipeline cache: failed to create cache directory");
		return;
	}
	auto temp_path = m_driver_cache_path;
	temp_path += ".tmp";
	Common::File file;
	uint32_t     prefix_written  = 0;
	uint32_t     payload_written = 0;
	if (file.Create(temp_path)) {
		file.Write(prefix.data(), static_cast<uint32_t>(prefix.size()), &prefix_written);
		file.Write(payload.data(), static_cast<uint32_t>(payload.size()), &payload_written);
	}
	const bool flushed = !file.IsInvalid() && file.Flush();
	file.Close();
	if (prefix_written != prefix.size() || payload_written != payload.size() || !flushed ||
	    !Common::File::RenameFile(temp_path, m_driver_cache_path)) {
		PipelineCacheLog("Vulkan pipeline cache: failed to write {}",
		                 Common::PathToString(m_driver_cache_path));
		return;
	}
	PipelineCacheLog("Vulkan pipeline cache: saved {} bytes to {}", payload.size(),
	                 Common::PathToString(m_driver_cache_path));
	m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
	m_driver_cache = nullptr;
}

// ---- --async-pipelines: pipeline jobs ----------------------------------------------------------
//
// A pipeline job runs CreatePipelineInternal (descriptor/pipeline layouts + vkCreate*Pipelines on
// the shared driver VkPipelineCache, which is created without EXTERNALLY_SYNCHRONIZED) on a worker
// and publishes the result into the pipeline map. All inputs are copies taken on the GPU thread.

struct PipelineCache::GraphicsPipelineJob final: AsyncJob {
	GraphicsPipelineJob(PipelineCache& owner, GraphicsPipelineKey job_key, uint64_t key_hash)
	    : AsyncJob(Kind::Graphics, key_hash), cache(owner), key(std::move(job_key)) {}

	void Execute() override {
		auto cached = std::make_unique<Pipeline>();
		cached->prefetched = IsPrefetched();
		LogPipelineTrace("CreatePipelineInternal begin", vs_id, ps_id);
		Stopwatch create_sw;
		CreatePipelineInternal(cache.m_graphics, *cached, key.rendering, key.vertex_input,
		                       vertex_info, ps_input_info ? &*ps_input_info : nullptr, programs,
		                       key.static_params, driver_cache);
		if (create_sw.On()) {
			const auto create_ns = create_sw.Elapsed();
			AddTimingRow("pipeline_create_gfx", KeyHash(), create_sw.Start(), create_ns,
			             fmt::format("vs_id={};ps_id={};tess={};driver_cache={};driver_ns={}", vs_id,
			                         ps_id, programs.VertexStageCount() == 3 ? 1 : 0,
			                         driver_cache != nullptr ? 1 : 0, Bench::TakeDriverCreateNs()));
		}
		LogPipelineTrace("CreatePipelineInternal done", vs_id, ps_id);
		EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
		EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);
		bool inserted = false;
		cache.PublishGraphicsPipeline(key, cached, inserted);
		if (!inserted) {
			cache.DestroyPipeline(*cached); // an equal pipeline was published first
		}
		// Publish first, unregister last: "not registered" must imply "published".
		cache.m_graphics_jobs.Erase(key);
	}

	void Cancel() override { cache.m_graphics_jobs.Erase(key); }

	PipelineCache&                     cache;
	GraphicsPipelineKey                key;
	std::vector<ShaderVertexInputInfo> vertex_info;
	std::optional<ShaderPixelInputInfo> ps_input_info;
	GraphicsPrograms                   programs;
	vk::PipelineCache                  driver_cache = nullptr;
	uint64_t                           vs_id        = 0;
	uint64_t                           ps_id        = 0;
};

struct PipelineCache::ComputePipelineJob final: AsyncJob {
	ComputePipelineJob(PipelineCache& owner, uint64_t id)
	    : AsyncJob(Kind::Compute, id), cache(owner), program_id(id) {}

	void Execute() override {
		auto      cached = std::make_unique<Pipeline>();
		cached->prefetched = IsPrefetched();
		Stopwatch create_sw;
		CreatePipelineInternal(cache.m_graphics, *cached, input_info, module, driver_cache);
		if (create_sw.On()) {
			const auto create_ns = create_sw.Elapsed();
			AddTimingRow("pipeline_create_cs", program_id, create_sw.Start(), create_ns,
			             fmt::format("cs_id={};driver_cache={};driver_ns={}", program_id,
			                         driver_cache != nullptr ? 1 : 0, Bench::TakeDriverCreateNs()));
		}
		EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
		EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);
		bool inserted = false;
		cache.PublishComputePipeline(program_id, cached, inserted);
		if (!inserted) {
			cache.DestroyPipeline(*cached);
		}
		cache.m_compute_jobs.Erase(program_id);
	}

	void Cancel() override { cache.m_compute_jobs.Erase(program_id); }

	PipelineCache&         cache;
	uint64_t               program_id;
	ShaderComputeInputInfo input_info;
	vk::ShaderModule       module       = nullptr;
	vk::PipelineCache      driver_cache = nullptr;
};

PipelineCache::GraphicsPrograms PipelineCache::GetGraphicsPrograms(
    const HW::VertexShaderInfo& vertex_regs, const HW::PixelShaderInfo& pixel_regs,
    const HW::ShaderRegisters& sh, const HW::Context& context, const HW::UserConfig& user_config,
    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping, bool pixel_active,
    std::array<ShaderVertexInputInfo, 3>& vertex_info, ShaderPixelInputInfo& pixel_info) {
	return GetGraphicsProgramsImpl(vertex_regs, pixel_regs, sh, context, user_config,
	                               target_export_mapping, pixel_active, vertex_info, pixel_info,
	                               false);
}

PipelineCache::GraphicsPrograms PipelineCache::PrefetchGraphicsPrograms(
    const HW::VertexShaderInfo& vertex_regs, const HW::PixelShaderInfo& pixel_regs,
    const HW::ShaderRegisters& sh, const HW::Context& context, const HW::UserConfig& user_config,
    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping, bool pixel_active,
    std::array<ShaderVertexInputInfo, 3>& vertex_info, ShaderPixelInputInfo& pixel_info) {
	return GetGraphicsProgramsImpl(vertex_regs, pixel_regs, sh, context, user_config,
	                               target_export_mapping, pixel_active, vertex_info, pixel_info,
	                               true);
}

PipelineCache::GraphicsPrograms PipelineCache::GetGraphicsProgramsImpl(
    const HW::VertexShaderInfo& vertex_regs, const HW::PixelShaderInfo& pixel_regs,
    const HW::ShaderRegisters& sh, const HW::Context& context, const HW::UserConfig& user_config,
    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping, bool pixel_active,
    std::array<ShaderVertexInputInfo, 3>& vertex_info, ShaderPixelInputInfo& pixel_info,
    bool prefetch) {
	const bool tess_active = user_config.GetPrimType() == Prospero::PrimitiveType::kPatch;
	if (prefetch && tess_active) {
		return {}; // not prefetched
	}
	std::array<ShaderParams, 3> vertex_params;
	if (tess_active) {
		vertex_params = PrepareTessellationPrograms(vertex_regs, context, vertex_info);
	} else {
		vertex_params[0] = PrepareProgram(vertex_regs, context, user_config, vertex_info[0]);
		if (prefetch && Common::Speculation::TakeFailure()) {
			return {};
		}
	}
	const bool mesh_active = vertex_info[0].logical_stage == ShaderType::Mesh;
	if (mesh_active) {
		EXIT_NOT_IMPLEMENTED(!m_graphics.mesh_shader_enabled);
		auto& mesh              = vertex_info[0].mesh;
		mesh.host_subgroup_size = m_graphics.subgroup_size;
		const auto& limits      = m_graphics.mesh_shader_properties;
		const auto  logical_threads =
		    mesh.threads_num[0] * mesh.threads_num[1] * mesh.threads_num[2];
		const auto host_threads = ((logical_threads + mesh.wave_size - 1u) / mesh.wave_size) *
		                          std::min(mesh.host_subgroup_size, mesh.wave_size);
		if (host_threads > limits.maxMeshWorkGroupInvocations ||
		    host_threads > limits.maxMeshWorkGroupSize[0] ||
		    mesh.max_vertices > limits.maxMeshOutputVertices ||
		    mesh.max_primitives > limits.maxMeshOutputPrimitives ||
		    mesh.lds_size_dwords * sizeof(uint32_t) > limits.maxMeshSharedMemorySize) {
			EXIT("mesh shader exceeds host limits: threads=%u vertices=%u primitives=%u LDS=%u\n",
			     host_threads, mesh.max_vertices, mesh.max_primitives, mesh.lds_size_dwords);
		}
	}
	ShaderParams pixel_params;
	if (pixel_active) {
		pixel_params      = PrepareProgram(pixel_regs, sh, target_export_mapping, pixel_info);
		const auto& blend = context.GetBlendControl(0);
		pixel_info.dual_source_blending =
		    blend.enable && !context.GetRenderTarget(0).info.blend_bypass &&
		    (BlendFactorIsDualSource(blend.color_srcblend) ||
		     BlendFactorIsDualSource(blend.color_destblend) ||
		     (blend.separate_alpha_blend && (BlendFactorIsDualSource(blend.alpha_srcblend) ||
		                                     BlendFactorIsDualSource(blend.alpha_destblend))));
		if (pixel_info.dual_source_blending) {
			// MRT1 supplies the second blend source for target 0.
			pixel_info.target_output_mode[1]    = pixel_info.target_output_mode[0];
			pixel_info.target_export_mapping[1] = pixel_info.target_export_mapping[0];
		} else if (blend.enable && !context.GetRenderTarget(0).info.blend_bypass &&
		           pixel_info.target_output_mode[0] != 0 && pixel_info.target_output_mode[0] != 7 &&
		           std::all_of(std::begin(pixel_info.target_output_mode) + 1,
		                       std::end(pixel_info.target_output_mode),
		                       [](uint8_t mode) { return mode == 0; }) &&
		           ClassifyBlendMapping(blend, pixel_info.target_export_mapping[0]) ==
		               BlendMappingSupport::SourceAlpha) {
			// Preserve logical alpha when the export mapping moves it.
			pixel_info.alpha_blend_source_remap = true;
			pixel_info.dual_source_blending     = true;
			pixel_info.target_output_mode[1]    = pixel_info.target_output_mode[0];
			pixel_info.target_export_mapping[1] = {};
		}
	}
	if (context.GetClipControl().clip_disable) {
		const auto& viewport = context.GetScreenViewport().viewports[0];
		const auto& limits   = m_graphics.GetPhysicalDeviceProperties().limits;
		auto&       clip     = vertex_info[tess_active ? 2u : 0u].clip_space;
		clip.scale[0]        = viewport.xscale;
		clip.scale[1]        = viewport.yscale;
		clip.offset[0]       = viewport.xoffset;
		clip.offset[1]       = viewport.yoffset;
		clip.half_extent[0] =
		    static_cast<float>(std::min(limits.maxViewportDimensions[0], 16384u)) * 0.5f;
		clip.half_extent[1] =
		    static_cast<float>(std::min(limits.maxViewportDimensions[1], 16384u)) * 0.5f;
		clip.enabled = true;
	}
	uint32_t          push_data_cursor =
	    mesh_active ? ShaderRecompiler::IR::PushData::MeshDrawDwordCount : 0;
	GraphicsPrograms  result;
	// Off mode: `async.pool` is null and every Get() below is the stock synchronous call. In skip
	// mode a stage whose program is not ready stops the whole lookup: the stages after it cannot
	// be captured because their push-data start depends on the earlier stages' compiled layouts.
	ProgramCache::AsyncContext async {
	    .pool       = AsyncPool(),
	    .allow_skip = !prefetch && m_async_mode == Config::AsyncPipelineMode::Skip,
	    .prefetch   = prefetch};
	if (pixel_active) {
		result.pixel = m_program_cache->Get(pixel_params, pixel_info, push_data_cursor, &async);
		if (async.pending) {
			result.pending = true;
			return result;
		}
		if (prefetch && !result.pixel) {
			return {};
		}
	}
	for (uint32_t i = 0; i < (tess_active ? 3u : 1u); i++) {
		result.vertex[i] =
		    m_program_cache->Get(vertex_params[i], vertex_info[i], push_data_cursor, &async);
		if (async.pending) {
			result.pending = true;
			return result;
		}
		if (prefetch && !result.vertex[i]) {
			return {};
		}
	}
	return result;
}

ShaderProgram PipelineCache::GetComputeProgram(const HW::ComputeShaderInfo& regs,
                                               const HW::ShaderRegisters&   sh,
                                               ShaderComputeInputInfo&      input_info) {
	input_info.host_subgroup_size = m_graphics.SupportsComputeWave64() ? 64u : 32u;
	const auto        params      = PrepareProgram(regs, sh, input_info);
	uint32_t          push_data_cursor = 0;
	// Compute dispatches are never skipped: the dispatch waits for the worker (stall and skip).
	ProgramCache::AsyncContext async {.pool = AsyncPool(), .allow_skip = false};
	return m_program_cache->Get(params, input_info, push_data_cursor, &async);
}

void PipelineCache::PrefetchCompute(const HW::ComputeShaderInfo& regs,
                                    const HW::ShaderRegisters& sh, uint32_t mode) {
	std::shared_lock gate(m_prefetch_gate);
	Common::Speculation::HeldLock held(&m_prefetch_gate, true);
	if (m_prefetch_stopped) {
		return;
	}
	auto* pool = AsyncPool();
	if (pool == nullptr) {
		return;
	}
	m_lookahead.cs_seen.fetch_add(1, std::memory_order_relaxed);
	if (regs.cs_regs.data_addr == 0) {
		m_lookahead.cs_skipped.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	ShaderComputeInputInfo input_info {};
	input_info.host_subgroup_size         = m_graphics.SupportsComputeWave64() ? 64u : 32u;
	input_info.dispatch_thread_dimensions = (mode & (1u << 5u)) != 0;
	const auto params                     = PrepareProgram(regs, sh, input_info);
	uint32_t   push_data_cursor           = 0;
	ProgramCache::AsyncContext async {.pool = pool, .allow_skip = false, .prefetch = true};
	const auto program = m_program_cache->Get(params, input_info, push_data_cursor, &async);
	if (!program) {
		m_lookahead.cs_skipped.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	if (FindComputePipeline(program.id) != nullptr || m_compute_jobs.Find(program.id) != nullptr) {
		m_lookahead.cs_already.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	auto fresh = std::make_shared<ComputePipelineJob>(*this, program.id);
	fresh->input_info   = input_info;
	fresh->module       = program.module;
	fresh->driver_cache = m_driver_cache;
	fresh->SetPrefetched();
	bool inserted = false;
	m_compute_jobs.Insert(program.id, fresh, inserted);
	if (!inserted) {
		m_lookahead.cs_already.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	if (FindComputePipeline(program.id) != nullptr) { // published between the checks above
		m_compute_jobs.Erase(program.id);
		m_lookahead.cs_already.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	if (!pool->Submit(fresh)) {
		m_compute_jobs.Erase(program.id);
		return;
	}
	m_lookahead.cs_requested.fetch_add(1, std::memory_order_relaxed);
}

bool PipelineStaticParameters::operator==(const PipelineStaticParameters& other) const noexcept {
	return std::memcmp(this, &other, sizeof(*this)) == 0;
}

PipelineCache::GraphicsPipelineKey PipelineCache::BuildGraphicsKey(
    std::span<const RenderColorInfo> colors, const RenderDepthInfo& depth,
    std::span<const ShaderVertexInputInfo> vertex_info, const HW::Context& ctx,
    const HW::UserConfig& user_config, const ShaderPixelInputInfo* ps_input_info,
    vk::PrimitiveTopology topology, bool primitive_restart_enable,
    const GraphicsPrograms& programs) {
	const auto& vs_input_info  = vertex_info.front();
	const auto& vertex_program = programs.vertex[0];
	const auto& pixel_program  = programs.pixel;

	EXIT_IF(colors.size() > RENDER_COLOR_ATTACHMENTS_MAX);
	EXIT_IF(!vertex_program);
	const bool ps_active = ps_input_info != nullptr;
	EXIT_IF(ps_active && !pixel_program);
	const auto color_count = static_cast<uint32_t>(colors.size());

	const HW::ModeControl& mc = ctx.GetModeControl();

	const auto ps_id = ps_active ? pixel_program.id : 0;

	GraphicsPipelineKey key {};
	for (uint32_t i = 0; i < programs.vertex.size(); i++) {
		key.vertex_shader_ids[i] = programs.vertex[i].id;
	}
	key.ps_shader_id            = ps_id;
	auto& static_params         = key.static_params;
	auto& rendering             = key.rendering;
	rendering.color_count       = 0;
	uint32_t attachment_samples = 0;
	for (uint32_t i = 0; i < color_count; i++) {
		const auto slot = colors[i].target_slot;
		EXIT_IF(slot >= RENDER_COLOR_ATTACHMENTS_MAX);
		rendering.color_count = std::max(rendering.color_count, slot + 1);
		EXIT_IF(!colors[i].image_id || colors[i].desc.view_info.format == vk::Format::eUndefined);
		static_params.color_mask[slot] = colors[i].export_mapping.ApplyMask(
		    render_target_mask_slot(ctx.GetRenderTargetMask(), colors[i].target_slot));
		rendering.color_formats[slot] = colors[i].desc.view_info.format;
		if (attachment_samples == 0) {
			attachment_samples = colors[i].desc.info.samples;
		} else if (attachment_samples != colors[i].desc.info.samples) {
			EXIT("mixed color attachment sample counts are unsupported: %u and %u\n",
			     attachment_samples, colors[i].desc.info.samples);
		}
		const auto& rt                        = ctx.GetRenderTarget(colors[i].target_slot);
		const auto& bc                        = ctx.GetBlendControl(colors[i].target_slot);
		static_params.color_srcblend[slot]       = bc.color_srcblend;
		static_params.color_comb_fcn[slot]       = bc.color_comb_fcn;
		static_params.color_destblend[slot]      = bc.color_destblend;
		static_params.alpha_srcblend[slot]       = bc.alpha_srcblend;
		static_params.alpha_comb_fcn[slot]       = bc.alpha_comb_fcn;
		static_params.alpha_destblend[slot]      = bc.alpha_destblend;
		static_params.separate_alpha_blend[slot] = bc.separate_alpha_blend;
		const bool alpha_remap =
		    slot == 0 && ps_input_info != nullptr && ps_input_info->alpha_blend_source_remap;
		static_params.blend_enable[slot] = bc.enable && !rt.info.blend_bypass;
		if (static_params.blend_enable[slot] && !alpha_remap &&
		    ClassifyBlendMapping(bc, colors[i].export_mapping) != BlendMappingSupport::Direct) {
			static_params.blend_enable[slot] = false;
			static std::atomic_bool warned = false;
			if (!warned.exchange(true, std::memory_order_relaxed)) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "Warning: blending disabled for unsupported color mapping "
				    "(slot={} mapping=0x{:02x} color={}/{} alpha={}/{} separate={}).\n",
				    slot, colors[i].export_mapping.packed, bc.color_srcblend, bc.color_destblend,
				    bc.alpha_srcblend, bc.alpha_destblend, bc.separate_alpha_blend ? 1 : 0));
			}
		}
		if (alpha_remap) {
			static_params.blend_alpha_source_remap = true;
		}
	}
	const bool with_depth =
	    depth.desc.view_info.format != vk::Format::eUndefined && static_cast<bool>(depth.image_id);
	if (with_depth) {
		const auto aspects       = ImageViewOps::DepthAspectMask(depth.desc.view_info.format);
		rendering.depth_format   = aspects & vk::ImageAspectFlagBits::eDepth
		                               ? depth.desc.view_info.format
		                               : vk::Format::eUndefined;
		rendering.stencil_format = aspects & vk::ImageAspectFlagBits::eStencil
		                               ? depth.desc.view_info.format
		                               : vk::Format::eUndefined;
		if (attachment_samples == 0) {
			attachment_samples = depth.desc.info.samples;
		} else if (attachment_samples != depth.desc.info.samples) {
			EXIT("mixed color/depth sample counts are unsupported: %u and %u\n", attachment_samples,
			     depth.desc.info.samples);
		}
	}
	if (color_count == 0 && !with_depth) {
		attachment_samples = render_sample_count(ctx.GetAaConfig().msaa_num_samples);
		EXIT_IF(!static_cast<bool>(
		    m_graphics.GetPhysicalDeviceProperties().limits.framebufferNoAttachmentsSampleCounts &
		    vulkan_sample_count(attachment_samples)));
	}
	EXIT_IF(attachment_samples == 0 ||
	        vulkan_sample_count(attachment_samples) == vk::SampleCountFlagBits {});

	if (ps_active && depth.depth_test_enable && ps_input_info->ps_execute_on_noop) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
			LOGF("Pipeline: temporary: accepting EXEC_ON_NOOP with depth test enabled\n");
		}
	}

	const auto& clip_control               = ctx.GetClipControl();
	static_params.negative_one_to_one      = !clip_control.dx_clip_space;
	static_params.depth_clip_enable        = clip_control.IsZClipEnabled();
	static_params.topology                 = topology;
	static_params.primitive_restart_enable = primitive_restart_enable;
	static_params.samples                  = attachment_samples;
	static_params.sample_shading_enable =
	    ps_active && attachment_samples > 1 && ps_input_info->ps_sample_shading;
	if (static_params.sample_shading_enable && !m_graphics.sample_rate_shading_enabled) {
		EXIT("Pipeline: sample-rate shading is required but unsupported by the host\n");
	}
	static_params.depth_bounds_test_enable = depth.depth_bounds_test_enable;
	static_params.depth_min_bounds         = depth.depth_min_bounds;
	static_params.depth_max_bounds         = depth.depth_max_bounds;
	const bool rect_list = Prospero::IsRectList(user_config.GetPrimType());
	static_params.cull_back  = !rect_list && mc.cull_back;
	static_params.cull_front = !rect_list && mc.cull_front;
	static_params.face       = mc.face;
	static_params.provoking_vtx_last = mc.provoking_vtx_last;
	static_params.polygon_mode =
	    ResolvePolygonMode(mc, static_params.cull_front, static_params.cull_back);

	if (vs_input_info.stage.program->stage != ShaderType::Mesh) {
		EXIT_IF(vs_input_info.buffers_num < 0 ||
		        vs_input_info.buffers_num > ShaderVertexInputInfo::RES_MAX ||
		        vs_input_info.resources_num < 0 ||
		        vs_input_info.resources_num > ShaderVertexInputInfo::RES_MAX);
		key.vertex_input.binding_count   = static_cast<uint8_t>(vs_input_info.buffers_num);
		key.vertex_input.attribute_count = static_cast<uint8_t>(vs_input_info.resources_num);
		for (int binding = 0; binding < vs_input_info.buffers_num; binding++) {
			const auto& buffer = vs_input_info.buffers[binding];
			key.vertex_input.bindings[binding] = {.stride   = buffer.stride,
			                                      .instance = buffer.fetch_index != 0};
		}
		for (int attribute = 0; attribute < vs_input_info.resources_num; attribute++) {
			const auto binding = vs_input_info.resources_dst[attribute].buffer_index;
			EXIT_IF(binding < 0 || binding >= vs_input_info.buffers_num);
			key.vertex_input.attributes[attribute] = {
			    .offset = static_cast<uint32_t>(vs_input_info.resources[attribute].Base48() -
			                                    vs_input_info.buffers[binding].addr),
			    .binding = static_cast<uint8_t>(binding),
			};
		}
	}

	return key;
}

PipelineCache::Pipeline* PipelineCache::GetGraphicsPipeline(
    std::span<const RenderColorInfo> colors, const RenderDepthInfo& depth,
    std::span<const ShaderVertexInputInfo> vertex_info, CommandBuffer& command,
    const ShaderPixelInputInfo* ps_input_info, vk::PrimitiveTopology topology,
    bool primitive_restart_enable, const GraphicsPrograms& programs) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Gfx)", profiler::colors::DeepOrangeA200);

	const auto  key = BuildGraphicsKey(colors, depth, vertex_info, command.GetRegisters(),
	                                   command.GetUserConfig(), ps_input_info, topology,
	                                   primitive_restart_enable, programs);
	const bool  ps_active      = ps_input_info != nullptr;
	const auto  vs_id          = programs.vertex[0].id;
	const auto  ps_id          = ps_active ? programs.pixel.id : 0;
	const auto& vertex_program = programs.vertex[0];
	const auto& pixel_program  = programs.pixel;
	const auto& vs_input_info  = vertex_info.front();
	const auto& rendering      = key.rendering;
	const auto& static_params  = key.static_params;

	if (auto* found = FindGraphicsPipeline(key)) {
		if (found->prefetched && !found->claimed) {
			found->claimed = true;
			m_lookahead.gfx_hit_ready.fetch_add(1, std::memory_order_relaxed);
		}
		return found;
	}

	// --async-pipelines: build the pipeline on a worker. A job captures the key, the vertex/pixel
	// input info (by value), the compiled program handles and the driver cache handle; the
	// stages' CompiledShaderInfo pointers it carries point at published, immutable permutations.
	if (auto* pool = AsyncPool()) {
		const bool                may_skip = m_async_mode == Config::AsyncPipelineMode::Skip;
		std::shared_ptr<AsyncJob> job      = m_graphics_jobs.Find(key, may_skip);
		if (job != nullptr && job->ClaimByDraw()) {
			m_lookahead.gfx_hit_inflight.fetch_add(1, std::memory_order_relaxed);
		}
		if (job == nullptr) {
			// Not in flight. It may have been published since the lookup above: a worker
			// publishes before it unregisters.
			if (auto* published = FindGraphicsPipeline(key)) {
				if (published->prefetched && !published->claimed) {
					published->claimed = true;
					m_lookahead.gfx_hit_ready.fetch_add(1, std::memory_order_relaxed);
				}
				return published;
			}
			auto fresh = std::make_shared<GraphicsPipelineJob>(*this, key,
			                                                   GraphicsPipelineKeyHash {}(key));
			fresh->vertex_info.assign(vertex_info.begin(), vertex_info.end());
			if (ps_input_info != nullptr) {
				fresh->ps_input_info = *ps_input_info;
			}
			fresh->programs     = programs;
			fresh->driver_cache = m_driver_cache;
			fresh->vs_id        = vs_id;
			fresh->ps_id        = ps_id;
			bool inserted       = false;
			job                 = m_graphics_jobs.Insert(key, fresh, inserted);
			if (inserted) {
				m_lookahead.gfx_miss.fetch_add(1, std::memory_order_relaxed);
				if (!pool->Submit(fresh, true)) {
					m_graphics_jobs.Erase(key);
					job = nullptr;
				} else if (may_skip) {
					fresh->CountSkippedDraw();
				}
			} else {
				if (job->ClaimByDraw()) {
					m_lookahead.gfx_hit_inflight.fetch_add(1, std::memory_order_relaxed);
				}
				if (may_skip) {
					job->CountSkippedDraw();
				}
			}
		}
		if (job != nullptr) {
			if (may_skip) {
				pool->Stats().skipped_pipeline_draws.fetch_add(1, std::memory_order_relaxed);
				return nullptr; // the caller skips this draw
			}
			if (pool->WaitFor(*job)) {
				if (auto* published = FindGraphicsPipeline(key)) {
					return published;
				}
			}
			// Cancelled by a shutdown: build it here.
		}
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(vs_input_info);
		if (ps_active) {
			ShaderDbgDumpInputInfo(*ps_input_info);
		}
		LOGF("PipelineTrace: shader modules VS=%" PRIu64 " module=%p PS=%" PRIu64 " module=%p\n",
		     vs_id, static_cast<void*>(vertex_program.module), ps_id,
		     static_cast<void*>(pixel_program.module));
	}

	auto cached = std::make_unique<Pipeline>();
	LogPipelineTrace("CreatePipelineInternal begin", vs_id, ps_id);
	Stopwatch create_sw;
	CreatePipelineInternal(m_graphics, *cached, rendering, key.vertex_input, vertex_info,
	                       ps_input_info, programs, static_params, m_driver_cache);
	if (create_sw.On()) {
		// One row per pipeline creation (a miss). dur_ns = create_ns (layouts + vkCreate*), of
		// which driver_ns is the vkCreateGraphicsPipelines call itself.
		const auto create_ns = create_sw.Elapsed();
		Bench::LogTiming("pipeline_create_gfx", GraphicsPipelineKeyHash {}(key), create_sw.Start(),
		                 create_ns, GuestGpu::IsGpuThread(),
		                 fmt::format("vs_id={};ps_id={};tess={};driver_cache={};driver_ns={}", vs_id,
		                             ps_id, programs.VertexStageCount() == 3 ? 1 : 0,
		                             m_driver_cache != nullptr ? 1 : 0, Bench::TakeDriverCreateNs()));
	}
	LogPipelineTrace("CreatePipelineInternal done", vs_id, ps_id);

	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	bool inserted = false;
	auto* const published = PublishGraphicsPipeline(key, cached, inserted);
	EXIT_IF(!inserted);

	return published;
}

void PipelineCache::PrefetchGraphicsPipeline(std::span<const RenderColorInfo> colors,
                                             const RenderDepthInfo&           depth,
                                             std::span<const ShaderVertexInputInfo> vertex_info,
                                             const HW::Context&               ctx,
                                             const HW::UserConfig&            user_config,
                                             const ShaderPixelInputInfo*      ps_input_info,
                                             vk::PrimitiveTopology            topology,
                                             bool primitive_restart_enable,
                                             const GraphicsPrograms&          programs) {
	std::shared_lock gate(m_prefetch_gate);
	Common::Speculation::HeldLock held(&m_prefetch_gate, true);
	if (m_prefetch_stopped) {
		return;
	}
	auto* pool = AsyncPool();
	if (pool == nullptr) {
		return;
	}
	const auto key = BuildGraphicsKey(colors, depth, vertex_info, ctx, user_config, ps_input_info,
	                                  topology, primitive_restart_enable, programs);
	if (FindGraphicsPipeline(key) != nullptr || m_graphics_jobs.Find(key) != nullptr) {
		m_lookahead.gfx_already.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	auto fresh = std::make_shared<GraphicsPipelineJob>(*this, key, GraphicsPipelineKeyHash {}(key));
	fresh->vertex_info.assign(vertex_info.begin(), vertex_info.end());
	if (ps_input_info != nullptr) {
		fresh->ps_input_info = *ps_input_info;
	}
	fresh->programs     = programs;
	fresh->driver_cache = m_driver_cache;
	fresh->vs_id        = programs.vertex[0].id;
	fresh->ps_id        = ps_input_info != nullptr ? programs.pixel.id : 0;
	fresh->SetPrefetched();
	bool inserted = false;
	m_graphics_jobs.Insert(key, fresh, inserted);
	if (!inserted) {
		m_lookahead.gfx_already.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	if (FindGraphicsPipeline(key) != nullptr) { // published between the checks above
		m_graphics_jobs.Erase(key);
		m_lookahead.gfx_already.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	if (!pool->Submit(fresh)) {
		m_graphics_jobs.Erase(key);
		return;
	}
	m_lookahead.gfx_requested.fetch_add(1, std::memory_order_relaxed);
}

PipelineCache::Pipeline&
PipelineCache::GetComputePipeline(const ShaderComputeInputInfo& input_info,
                                  const ShaderProgram&          compute_program) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Compute)", profiler::colors::RedA100);

	EXIT_IF(!compute_program);

	if (auto* found = FindComputePipeline(compute_program.id)) {
		if (found->prefetched && !found->claimed) {
			found->claimed = true;
			m_lookahead.cs_hit_ready.fetch_add(1, std::memory_order_relaxed);
		}
		return *found;
	}

	// --async-pipelines: compute dispatches are never skipped, they wait for the worker.
	if (auto* pool = AsyncPool()) {
		std::shared_ptr<AsyncJob> job = m_compute_jobs.Find(compute_program.id);
		if (job != nullptr && job->ClaimByDraw()) {
			m_lookahead.cs_hit_inflight.fetch_add(1, std::memory_order_relaxed);
		}
		if (job == nullptr) {
			if (auto* published = FindComputePipeline(compute_program.id)) {
				if (published->prefetched && !published->claimed) {
					published->claimed = true;
					m_lookahead.cs_hit_ready.fetch_add(1, std::memory_order_relaxed);
				}
				return *published;
			}
			auto fresh = std::make_shared<ComputePipelineJob>(*this, compute_program.id);
			fresh->input_info   = input_info;
			fresh->module       = compute_program.module;
			fresh->driver_cache = m_driver_cache;
			bool inserted       = false;
			job                 = m_compute_jobs.Insert(compute_program.id, fresh, inserted);
			if (inserted) {
				m_lookahead.cs_miss.fetch_add(1, std::memory_order_relaxed);
			} else if (job->ClaimByDraw()) {
				m_lookahead.cs_hit_inflight.fetch_add(1, std::memory_order_relaxed);
			}
			if (inserted && !pool->Submit(fresh, true)) {
				m_compute_jobs.Erase(compute_program.id);
				job = nullptr;
			}
		}
		if (job != nullptr && pool->WaitFor(*job)) {
			if (auto* published = FindComputePipeline(compute_program.id)) {
				return *published;
			}
		}
		// Cancelled by a shutdown: build it here.
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(input_info);
	}

	auto cached = std::make_unique<Pipeline>();
	Stopwatch create_sw;
	CreatePipelineInternal(m_graphics, *cached, input_info, compute_program.module, m_driver_cache);
	if (create_sw.On()) {
		const auto create_ns = create_sw.Elapsed();
		Bench::LogTiming("pipeline_create_cs", compute_program.id, create_sw.Start(), create_ns,
		                 GuestGpu::IsGpuThread(),
		                 fmt::format("cs_id={};driver_cache={};driver_ns={}", compute_program.id,
		                             m_driver_cache != nullptr ? 1 : 0, Bench::TakeDriverCreateNs()));
	}

	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	bool inserted = false;
	auto* const published = PublishComputePipeline(compute_program.id, cached, inserted);
	EXIT_IF(!inserted);

	return *published;
}
} // namespace Libs::Graphics
