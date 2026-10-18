#include "graphics/host_gpu/renderer/pipeline/nullPageRead.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/skipDepsHooks.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCacheFile.h"
#include "graphics/host_gpu/renderer/pipeline/srtPlan.h"

#include "common/assert.h"
#include "common/benchInstrument.h"
#include "common/gpuPhaseStats.h"
#include "common/hangWatchdog.h"
#include "common/speculation.h"
#include "common/emulatorConfig.h"
#include "common/loopGuard.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/guest_gpu/csRegisters.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "common/fairSharedMutex.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/pipeline/blendMapping.h"
#include "graphics/host_gpu/renderer/pipeline/graphicsVariantKeys.h"
#include "graphics/host_gpu/renderer/pipeline/resolvedDrawStats.h"
#include "graphics/host_gpu/renderer/pipeline/scalarReadHazard.h"
#include "graphics/host_gpu/renderer/pipeline/variantKeys.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/frontend/decode/DrawSkipSafety.h"
#include "graphics/shader/shaderCompiler.h"
#include "kernel/memory.h"
#include "kytyGitVersion.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <functional>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fmt/format.h>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <optional>
#include <optional>
#include <shared_mutex>
#include <span>
#include <spirv-tools/libspirv.hpp>
#include <string_view>
#include <thread>
#include <condition_variable>
#include <unordered_set>
#include <utility>
#include <vector>
#include <xxhash.h>

namespace Libs::Graphics {

namespace {

uint8_t RemapSourceAlphaFactor(uint8_t factor) {
	switch (static_cast<Prospero::BlendFactor>(factor)) {
		case Prospero::BlendFactor::kSrcAlpha:
			return static_cast<uint8_t>(Prospero::BlendFactor::kSrc1Alpha);
		case Prospero::BlendFactor::kOneMinusSrcAlpha:
			return static_cast<uint8_t>(Prospero::BlendFactor::kOneMinusSrc1Alpha);
		default: return factor;
	}
}

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

constexpr uint32_t kPipelineCacheSchemaVersion = PipelineCacheFile::kSchemaVersion;

PipelineCacheFile::KeyMode DriverCacheKeyMode() {
	return Config::GetPipelineCacheKey() == Config::PipelineCacheKey::Schema
	           ? PipelineCacheFile::KeyMode::Schema
	           : PipelineCacheFile::KeyMode::Revision;
}

std::string DriverCacheSignature(const vk::PhysicalDeviceProperties& properties) {
	PipelineCacheFile::DeviceIdentity device;
	device.vendor_id      = properties.vendorID;
	device.device_id      = properties.deviceID;
	device.driver_version = properties.driverVersion;
	for (size_t i = 0; i < device.uuid.size(); i++) {
		device.uuid[i] = properties.pipelineCacheUUID[i];
	}
	return PipelineCacheFile::Signature(DriverCacheKeyMode(), KYTY_GIT_REVISION, device);
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

// Duration of a hit-path step (MaterializeResources / S_SWAP_PC refresh) inside one program Get().
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

// A stored specialization can be stale (the translator changed since it was recorded). Applying an
// incompatible one is fatal in ApplyResourceSpecialization, so check what it asserts.
bool SpecializationFits(const ShaderRecompiler::IR::ResourcePlan&           plan,
                        const ShaderRecompiler::IR::ResourceSpecialization& spec) {
	using ShaderRecompiler::IR::BufferResource;
	using ShaderRecompiler::IR::ImageResource;
	if (spec.buffers.size() < plan.info.buffers.size() || spec.images.size() < plan.info.images.size()) {
		return false;
	}
	for (size_t i = 0; i < spec.buffers.size(); i++) {
		const auto root = spec.buffers[i].indirect_root;
		if (root != BufferResource::NoIndirectBuffer && root >= spec.buffers.size()) return false;
		if (i >= plan.info.buffers.size() &&
		    (root == BufferResource::NoIndirectBuffer || root >= plan.info.buffers.size())) {
			return false;
		}
	}
	for (size_t i = 0; i < spec.images.size(); i++) {
		const auto root = spec.images[i].indirect_root;
		if (root != ImageResource::NoIndirectImage && root >= spec.images.size()) return false;
		if (i >= plan.info.images.size() &&
		    (root == ImageResource::NoIndirectImage || root >= plan.info.images.size())) {
			return false;
		}
	}
	return true;
}

// One-line text of a variant for the compute_variant timing row: static state words, then the
// buffers (stride:format:swizzle:oob:indirect) and images (class:dimension:mips:conversion:swizzle:
// flags). ',' is reserved by the timing log, so '|' and ':' separate fields.
std::string DescribeVariant(const Variants::ComputeVariant& v) {
	std::string text = fmt::format(
	    "user={};wg_reg={};wave={};float={};host_sg={};tid={};lds={};scratch={};threads={}x{}x{};flags={:x}",
	    v.state.user_data_count, v.state.workgroup_register, v.state.wave_size, v.state.float_mode,
	    v.state.host_subgroup_size, v.state.thread_ids_num, v.state.lds_size_dwords,
	    v.state.scratch_size_dwords, v.state.threads_num[0], v.state.threads_num[1],
	    v.state.threads_num[2], v.state.flags);
	text += ";buffers=";
	for (const auto& b: v.spec.buffers) {
		text += fmt::format("{}:{}:{:x}:{}:{}|", b.packed_stride, b.descriptor_format,
		                    b.descriptor_swizzle, b.zero_stride_oob,
		                    b.indirect_root == 0xffffffffu ? 0 : 1);
	}
	text += ";images=";
	for (const auto& i: v.spec.images) {
		text += fmt::format("{}:{}:{}:{}:{:x}:{}|", i.numeric_class, i.dimension, i.mip_count,
		                    i.conversion_format, i.shader_swizzle, i.flags);
	}
	return text;
}

bool ReadShaderGuestMemory(void*, uint64_t address, std::span<uint32_t> values) {
	Common::GpuPhase::Scope gpu_phase_read(Common::GpuPhase::Phase::PipeSrtRead);
	SkipDeps::NoteScalarRead(address, values.size_bytes());
	// Scalar and unformatted buffer dependencies use the same backing as native raw loads.
	// Image synchronization belongs to formatted buffer bindings, not these reads. A table pointer
	// the game left null reads zeros, as on the GPU.
	if (ReadGuestNullPage(address, values)) {
		return true;
	}
	return !values.empty() &&
	       Libs::LibKernel::Memory::TryReadBufferBacking(address, values.data(), values.size_bytes());
}

bool ReadShaderScalarMemory(void*, uint64_t address, std::span<uint32_t> values) {
	Common::GpuPhase::Scope gpu_phase_read(Common::GpuPhase::Phase::PipeSrtRead);
	SkipDeps::NoteScalarRead(address, values.size_bytes());
	// The walk evaluates loads the shader may never execute, e.g. through a null SRT pointer, so
	// unmapped guest memory reads as zeros instead of faulting the host.
	if (!Libs::LibKernel::Memory::TryReadBacking(address, values.data(), values.size_bytes())) {
		std::ranges::fill(values, 0u);
	}
	return true;
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
	SkipDeps::NoteScalarRead(address, values.size_bytes());
	if (ReadGuestNullPage(address, values)) {
		return true;
	}
	return !values.empty() &&
	       Libs::LibKernel::Memory::TryReadBacking(address, values.data(), values.size_bytes());
}

// --srt-plan-shadow, lookahead thread: the same two readers, remembering what they returned.
bool RecordingScalarRead(void* userdata, uint64_t address, std::span<uint32_t> values) {
	if (!ReadShaderScalarMemory(nullptr, address, values)) return false;
	static_cast<SrtPlan::ReadRecorder*>(userdata)->Record(SrtPlan::ReadKind::Scalar, address, values);
	return true;
}

bool RecordingSpecializationRead(void* userdata, uint64_t address, std::span<uint32_t> values) {
	if (!ReadShaderGuestMemorySpeculative(nullptr, address, values)) return false;
	static_cast<SrtPlan::ReadRecorder*>(userdata)->Record(SrtPlan::ReadKind::Clean, address, values);
	return true;
}

// --srt-plan-shadow, kyGpu: re-reads for the validity check. No phase scopes (the check is timed
// on its own) and no synchronization: GPU-dirty memory is reported by ShadowDirty instead.
bool ShadowRead(SrtPlan::ReadKind kind, uint64_t address, std::span<uint32_t> values, bool exact) {
	if (values.empty()) return true;
	if (exact) {
		if (!Libs::LibKernel::Memory::TryReadBacking(address, values.data(), values.size_bytes())) return false;
		if (kind == SrtPlan::ReadKind::Clean) {
			Libs::LibKernel::Memory::OverlayKnownGpuBytes(address, values.data(), values.size_bytes());
		}
		return true;
	}
	if (kind == SrtPlan::ReadKind::Scalar) {
		if (!Libs::LibKernel::Memory::TryReadBacking(address, values.data(), values.size_bytes())) {
			std::ranges::fill(values, 0u);
		}
		return true;
	}
	if (ReadGuestNullPage(address, values)) return true;
	if (!Libs::LibKernel::Memory::TryReadBacking(address, values.data(), values.size_bytes())) return false;
	// --known-stores: a GPU-dirty dword a recorded dispatch is proven to have stored holds that value
	// (ShadowDirty does not count it as dirty).
	Libs::LibKernel::Memory::OverlayKnownGpuBytes(address, values.data(), values.size_bytes());
	return true;
}

bool ShadowDirty(uint64_t address, uint64_t bytes) {
	return Libs::LibKernel::Memory::HasUnknownGpuDirtyBytes(address, bytes);
}

// --srt-plan-shadow, kyGpu thread: kyGpu has materialized the draw's resources itself; look up the
// plan the lookahead made for the same draw and record how it compares. Nothing from the plan is
// used.
void ShadowCompare(const SrtPlan::Key& key, bool nested, bool compute, uint32_t stage,
                   uint64_t shader_hash, const ShaderRecompiler::IR::SrtRuntime& runtime,
                   const ShaderRecompiler::IR::ResourceSnapshot&       resources,
                   const ShaderRecompiler::IR::ResourceSpecialization& specialization,
                   int64_t materialize_ns) {
	Common::GpuPhase::Scope gpu_phase_shadow(Common::GpuPhase::Phase::SrtPlanShadow);
	SrtPlan::Sample sample;
	sample.compute        = compute;
	sample.materialize_ns = materialize_ns;
	if (nested) {
		sample.outcome = SrtPlan::Outcome::SubDraw;
		SrtPlan::Record(sample);
		return;
	}
	std::unique_ptr<SrtPlan::Plan> plan;
	const auto t_lookup = SrtPlan::NowNs();
	const auto lookup   = SrtPlan::GetStore().Take(key, plan);
	sample.lookup_ns    = SrtPlan::NowNs() - t_lookup;
	switch (lookup) {
		case SrtPlan::Lookup::NotReady: sample.outcome = SrtPlan::Outcome::NotReady; break;
		case SrtPlan::Lookup::Missing: sample.outcome = SrtPlan::Outcome::Missing; break;
		case SrtPlan::Lookup::Late: sample.outcome = SrtPlan::Outcome::Late; break;
		case SrtPlan::Lookup::Evicted: sample.outcome = SrtPlan::Outcome::Evicted; break;
		case SrtPlan::Lookup::Ready: sample.outcome = SrtPlan::Outcome::Ready; break;
	}
	if (lookup != SrtPlan::Lookup::Ready) {
		SrtPlan::Record(sample);
		return;
	}
	if (plan->stage != stage || plan->shader_hash != shader_hash) {
		sample.outcome = SrtPlan::Outcome::Desync;
		SrtPlan::Record(sample);
		return;
	}
	const auto t_check = SrtPlan::NowNs();
	sample.lead_ns     = t_check - plan->produced_ns;
	const auto events  = SrtPlan::ProducerEvents();
	sample.lead_draws  = events > plan->produced_serial ? events - plan->produced_serial : 0;
	// The validity check: the inputs, then the guest memory the walk read.
	const SrtPlan::Inputs inputs {runtime.user_data, runtime.shader_base, runtime.workgroup_counts};
	if (!SrtPlan::InputsMatch(*plan, inputs)) {
		sample.invalid = SrtPlan::Invalid::Inputs;
	} else {
		switch (plan->reads.Validate(ShadowRead, ShadowDirty)) {
			case SrtPlan::ReadSet::Result::Valid: break;
			case SrtPlan::ReadSet::Result::Changed: sample.invalid = SrtPlan::Invalid::Changed; break;
			case SrtPlan::ReadSet::Result::Unreadable: sample.invalid = SrtPlan::Invalid::Unreadable; break;
			case SrtPlan::ReadSet::Result::GpuDirty: sample.invalid = SrtPlan::Invalid::GpuDirty; break;
		}
	}
	const auto t_compare = SrtPlan::NowNs();
	sample.check_ns      = t_compare - t_check;
	sample.diff          = SrtPlan::Diff(plan->snapshot, plan->specialization, resources, specialization);
	sample.equal         = sample.diff == 0;
	sample.compare_ns    = SrtPlan::NowNs() - t_compare;
	sample.windows       = static_cast<uint32_t>(plan->reads.windows.size());
	sample.runs          = static_cast<uint32_t>(plan->reads.runs.size());
	sample.dwords        = static_cast<uint32_t>(plan->reads.values.size());
	for (const auto& window: plan->reads.windows) {
		sample.clean_windows += window.kind == SrtPlan::ReadKind::Clean;
	}
	SrtPlan::Record(sample);
}

// --srt-plans, kyGpu thread: look the draw's plan up and check it (SrtPlan::Decide, the same
// validity check as the shadow). Returns the plan when it can stand in for the draw's own
// materialization, nullptr otherwise; counts the outcome, no clocks and no samples.
std::unique_ptr<SrtPlan::Plan> TakeAdoptablePlan(const SrtPlan::Key& key, bool nested, uint32_t stage,
                                                 uint64_t                                    shader_hash,
                                                 const ShaderRecompiler::IR::SrtRuntime& runtime) {
	Common::GpuPhase::Scope gpu_phase_adopt(Common::GpuPhase::Phase::SrtPlanAdopt);
	std::unique_ptr<SrtPlan::Plan> plan;
	const SrtPlan::Inputs          inputs {runtime.user_data, runtime.shader_base, runtime.workgroup_counts};
	const auto decision = SrtPlan::Decide(SrtPlan::GetStore(), key, nested, stage, shader_hash, inputs,
	                                      ShadowRead, ShadowDirty, plan);
	SrtPlan::Adopting().Note(decision);
	if (decision.Adopted()) return plan;
	if (plan) SrtPlan::RecyclePlan(std::move(plan));
	return nullptr;
}

void DumpShaderSpirv(const char* stage_name, uint64_t shader_hash,
                     const std::vector<uint32_t>& spirv) {
	if (!Config::GraphicsDebugDumpEnabled() && !Config::DebugDumpShaderMatches(shader_hash)) {
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
	if (!Config::GraphicsDebugDumpEnabled() && !Config::DebugDumpShaderMatches(shader_hash)) {
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


// Contention and popularity figures of the lookahead, printed with its summary (observation only).
// Only rare events are counted in shared atomics; the popularity table is per thread (merged for the
// summary) so that several prefetch workers do not meet on one counter or one lock per materialization.
struct PopularityShard {
	std::mutex                             mutex; // taken by its own thread per call, by the summary once
	std::unordered_map<uint64_t, uint64_t> counts;
};

struct LookaheadLockStats {
	std::atomic<uint64_t> lookahead_waits {0};     // entry locks the lookahead was blocked on
	std::atomic<int64_t>  lookahead_wait_ns {0};
	std::atomic<uint64_t> gpu_waits {0};
	std::atomic<int64_t>  gpu_wait_ns {0};
	std::atomic<uint64_t> compile_waits {0};       // the lookahead blocked on a program/pipeline compile
	std::atomic<int64_t>  compile_wait_ns {0};
	std::mutex                                    shards_mutex;
	std::vector<std::shared_ptr<PopularityShard>> shards;
};

LookaheadLockStats& LockStats() {
	static LookaheadLockStats stats;
	return stats;
}

void NotePopularity(uint64_t shader_hash) {
	thread_local std::shared_ptr<PopularityShard> shard = [] {
		auto created = std::make_shared<PopularityShard>();
		auto& stats  = LockStats();
		std::lock_guard lock(stats.shards_mutex);
		stats.shards.push_back(created);
		return created;
	}();
	std::lock_guard lock(shard->mutex);
	++shard->counts[shader_hash];
}

std::string LookaheadLockSummary() {
	auto& stats = LockStats();
	std::vector<std::pair<uint64_t, uint64_t>> top;
	uint64_t total = 0;
	{
		std::unordered_map<uint64_t, uint64_t> merged;
		std::lock_guard registry_lock(stats.shards_mutex);
		for (const auto& shard: stats.shards) {
			std::lock_guard lock(shard->mutex);
			for (const auto& [hash, count]: shard->counts) merged[hash] += count;
		}
		for (const auto& [hash, count]: merged) {
			top.emplace_back(count, hash);
			total += count;
		}
	}
	std::sort(top.begin(), top.end(), std::greater<>());
	std::string text = fmt::format(
	    "entry_lock_waits lookahead={} wait_ms={:.1f}; gpu={} wait_ms={:.1f}; "
	    "compile_waits={} compile_wait_ms={:.1f}; materializations={} distinct_shaders={} top:",
	    stats.lookahead_waits.load(), static_cast<double>(stats.lookahead_wait_ns.load()) / 1e6,
	    stats.gpu_waits.load(), static_cast<double>(stats.gpu_wait_ns.load()) / 1e6,
	    stats.compile_waits.load(), static_cast<double>(stats.compile_wait_ns.load()) / 1e6, total,
	    top.size());
	double cumulative = 0.0;
	for (size_t i = 0; i < std::min<size_t>(top.size(), 12); ++i) {
		cumulative += total != 0 ? 100.0 * static_cast<double>(top[i].first) / static_cast<double>(total) : 0.0;
		text += fmt::format(" {:016x}:{:.1f}%(cum {:.1f}%)", top[i].second,
		                    total != 0 ? 100.0 * static_cast<double>(top[i].first) / static_cast<double>(total) : 0.0,
		                    cumulative);
	}
	return text;
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
//  * Every SourceEntry has its own `mutex` guarding all of its mutable state: the call source
//    (`linked` snapshot pointer, `reads`, `revision`), `resource_plan`, `resources`,
//    `specialization` and the permutation list. Long work (translation, SPIR-V, module creation)
//    runs outside any lock on snapshots/copies taken under it.
//  * `permutations` is a deque and published Permutations are immutable, so a Permutation* handed
//    out earlier stays valid while other threads append.
//  * The lookup key is per call (thread_local scratch), and shader ids come from an atomic counter.
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
		std::vector<uint32_t>                       function_code;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		ShaderRecompiler::IR::CompiledShaderInfo     program;
		ShaderProgram                                handle;
		// --debug-lookahead-verify only.
		uint64_t                                     spirv_hash = 0;
		bool                                         prefetched = false;
		bool                                         verified   = false;
		// Built by a shader-load job (set before publication, never changed after).
		bool                                         from_load  = false;
		int64_t                                      load_register_ns = 0;
		int64_t                                      load_ready_ns = 0;
		// Set by the GPU thread at the first dispatch that uses the permutation (variant
		// recording and hit accounting); copyable so the deque can take it by move.
		struct UseFlag {
			std::atomic<bool> value {false};
			UseFlag() = default;
			UseFlag(const UseFlag& other): value(other.value.load(std::memory_order_relaxed)) {}
			UseFlag& operator=(const UseFlag& other) {
				value.store(other.value.load(std::memory_order_relaxed), std::memory_order_relaxed);
				return *this;
			}
			bool Exchange() { return value.exchange(true, std::memory_order_relaxed); }
		} used;
	};

	using LinkedPtr = std::shared_ptr<const ShaderRecompiler::ShaderSource::Linked>;

	// Consistent copy of the mutable parts of a call source.
	struct CallSnapshot {
		LinkedPtr                                   linked;
		std::vector<std::pair<uint64_t, uint64_t>> reads;
	};

	struct SourceEntry {
		SourceEntry(ShaderRecompiler::IR::ResourcePlan plan,
		            std::unique_ptr<ShaderRecompiler::ShaderSource> call_source)
		    : call_source(std::move(call_source)), resource_plan(std::move(plan)) {}

		// `call_source` itself (the pointer and its immutable `code`) never changes after
		// construction; only the members listed in ShaderSource's contract are mutated, under `mutex`.
		static std::span<const uint32_t> FunctionCode(const LinkedPtr& linked, size_t base_code_size) {
			return linked ? std::span<const uint32_t>(linked->code).subspan(base_code_size)
			              : std::span<const uint32_t> {};
		}

		[[nodiscard]] CallSnapshot SnapshotCall() const {
			std::shared_lock lock(mutex);
			return {call_source->linked, call_source->reads};
		}

		bool RefreshCallSource(const ShaderRecompiler::IR::SrtRuntime& runtime) {
			std::unique_lock lock(mutex);
			const auto revision = call_source->revision;
			ShaderRecompiler::RefreshShaderSource(*call_source, runtime);
			resource_plan.source_reads = call_source->reads;
			return revision != call_source->revision;
		}

		void SetResourcePlan(ShaderRecompiler::IR::ResourcePlan plan) {
			std::unique_lock lock(mutex);
			resource_plan = std::move(plan);
		}

		// Re-reads guest memory and refreshes `resources`/`specialization`. Returns a copy of the
		// specialization so callers can compile against exactly what was materialized.
		ShaderRecompiler::IR::ResourceSpecialization
		Materialize(const ShaderRecompiler::IR::SrtRuntime& runtime, StepTime* materialize_time) {
			std::unique_lock lock(mutex, std::defer_lock);
			LockEntry(lock, "entry_lock_gpu");
			SkipDeps::ReaderScope reader(resource_plan.shader_hash, "walk_gpu");
			MaterializeLocked(runtime, materialize_time);
			return specialization;
		}

		// Materializes, then looks for an already-compiled permutation. `specialization_out`
		// receives the materialized specialization when nothing matches (it is only copied then).
		template <bool HasCalls>
		Permutation* FindPermutation(const ShaderRecompiler::IR::SrtRuntime& runtime,
		                             uint32_t push_data_cursor, StepTime* materialize_time,
		                             ShaderRecompiler::IR::ResourceSpecialization* specialization_out) {
			std::unique_lock lock(mutex, std::defer_lock);
			LockEntry(lock, "entry_lock_gpu");
			SkipDeps::ReaderScope reader(resource_plan.shader_hash, "walk_gpu");
			MaterializeLocked(runtime, materialize_time);
			Permutation* found = nullptr;
			if constexpr (HasCalls) {
				const auto& linked = call_source->linked;
				found = FindExistingLocked<true>(push_data_cursor, specialization,
				                                 FunctionCode(linked, call_source->code.size()));
			} else {
				found = FindExistingLocked<false>(push_data_cursor, specialization, {});
			}
			// Copy only on a miss: the hit path (every draw) stays allocation-free.
			if (found == nullptr && specialization_out != nullptr) {
				*specialization_out = specialization;
			}
			return found;
		}

		// --srt-plans: the draw's materialization is `plan` (checked by the caller) instead of a walk
		// of the guest memory: the entry takes over the plan's snapshot and specialization, then the
		// same permutation search runs. The plan keeps the entry's old buffers for recycling.
		Permutation* FindPermutationAdopting(SrtPlan::Plan& plan, uint32_t push_data_cursor,
		                                     ShaderRecompiler::IR::ResourceSpecialization* specialization_out) {
			std::shared_lock lock(mutex, std::defer_lock);
			LockEntry(lock, "entry_lock_gpu");
			SrtPlan::AdoptInto(resources, specialization, plan);
			Permutation* found = nullptr;
			auto& perm_stats = ResolvedDraw::GetStats().permutation;
			if (Config::ResolvedDrawEnabled() &&
			    (plan.permutation == nullptr || plan.entry != this || plan.permutation_cursor != push_data_cursor)) {
				if (plan.permutation == nullptr) perm_stats.Absent(); else perm_stats.Fallback();
			}
			if (plan.permutation != nullptr && plan.entry == this &&
			    plan.permutation_cursor == push_data_cursor && Config::ResolvedDrawEnabled()) {
				perm_stats.Used();
				// Resolved draw: the producer's search, over this entry's append-only permutation
				// list, with this cursor and a specialization equal to the adopted one, found this
				// permutation; the first match of any later search is the same one.
				found = const_cast<Permutation*>(static_cast<const Permutation*>(plan.permutation));
				if (Config::ResolvedDrawVerify()) {
					if (found != FindExistingLocked<false>(push_data_cursor, specialization, {})) {
						EXIT("resolved draw: the planned permutation is not the one a search finds\n");
					}
					perm_stats.Verified();
				}
			} else {
				found = FindExistingLocked<false>(push_data_cursor, specialization, {});
			}
			if (found == nullptr && specialization_out != nullptr) {
				*specialization_out = specialization;
			}
			return found;
		}

		// --srt-plans with --srt-plan-shadow: materializes the draw the ordinary way into scratch
		// storage and returns which parts differ from what was adopted (0 = byte-equal).
		uint32_t DiffAdoptedAgainstOwn(const ShaderRecompiler::IR::SrtRuntime& runtime) {
			std::unique_lock lock(mutex, std::defer_lock);
			LockEntry(lock, "entry_lock_gpu");
			SkipDeps::ReaderScope reader(resource_plan.shader_hash, "walk_shadow");
			thread_local ShaderRecompiler::IR::ResourceSnapshot       own;
			thread_local ShaderRecompiler::IR::ResourceSpecialization own_specialization;
			own                = {};
			own_specialization = {};
			EXIT_IF(!ShaderRecompiler::IR::MaterializeResources(resource_plan, runtime, own,
			                                                    own_specialization));
			return SrtPlan::Diff(resources, specialization, own, own_specialization);
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
		                             bool&                                   materialized,
		                             ShaderRecompiler::IR::ResourceSnapshot* keep = nullptr) {
			std::shared_lock lock(mutex, std::defer_lock);
			LockEntry(lock, "entry_lock_lookahead");
			Common::Speculation::HeldLock held(mutex.Raw(), true);
			Common::Hang::Phase phase("lookahead_materialize");
			SkipDeps::ReaderScope reader(resource_plan.shader_hash, "walk_lookahead");
			NotePopularity(resource_plan.shader_hash);
			ShaderRecompiler::IR::ResourceSnapshot discarded;
			auto& snapshot = keep != nullptr ? *keep : discarded; // --srt-plan-shadow keeps it
			materialized = ShaderRecompiler::IR::MaterializeResources(resource_plan, runtime, snapshot,
			                                                       specialization_out);
			if (!materialized) {
				return nullptr;
			}
			return FindExistingLocked<false>(push_data_cursor, specialization_out, {});
		}

		bool MaterializePrivate(const ShaderRecompiler::IR::SrtRuntime& runtime,
		                        ShaderRecompiler::IR::ResourceSpecialization& specialization_out) {
			std::shared_lock lock(mutex, std::defer_lock);
			LockEntry(lock, "entry_lock_lookahead");
			Common::Speculation::HeldLock held(mutex.Raw(), true);
			Common::Hang::Phase phase("lookahead_materialize");
			SkipDeps::ReaderScope reader(resource_plan.shader_hash, "walk_lookahead_private");
			ShaderRecompiler::IR::ResourceSnapshot snapshot;
			return ShaderRecompiler::IR::MaterializeResources(resource_plan, runtime, snapshot,
			                                                  specialization_out);
		}

		// Publishes a freshly compiled permutation. If another thread already published an equal
		// one (two threads missing the same program), keeps theirs and sets `duplicate`; in that
		// case `compiled` is left untouched so the caller can release its shader module.
		Permutation* Publish(Permutation& compiled, uint32_t push_data_cursor, bool has_calls,
		                     bool& duplicate) {
			std::unique_lock lock(mutex);
			Permutation*     existing = has_calls
			    ? FindExistingLocked<true>(push_data_cursor, compiled.specialization,
			                               compiled.function_code)
			    : FindExistingLocked<false>(push_data_cursor, compiled.specialization, {});
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
			return FindExistingLocked<false>(push_data_cursor, wanted, {});
		}

		std::unique_ptr<ShaderRecompiler::ShaderSource> call_source;
		ShaderRecompiler::IR::ResourcePlan           resource_plan;
		ShaderRecompiler::IR::ResourceSnapshot       resources;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		std::deque<Permutation>                      permutations;
		// Shared by the lookahead's materializations (private output, per-thread scratch) and by
		// kyGpu's adoption; exclusive for whoever changes the entry itself (publishing a permutation,
		// refreshing a call source, kyGpu's own materialization into `resources`).
		mutable Common::FairSharedMutex              mutex;

	private:
		// Takes the entry lock, publishing "waiting for the entry lock" for the hang watchdog.
		template <class Lock>
		void LockEntry(Lock& lock, const char* what) {
			Common::GpuPhase::Scope gpu_phase_lock(Common::GpuPhase::Phase::PipeEntryLock);
			Common::Hang::Phase phase(what);
			if (lock.try_lock()) {
				return;
			}
			auto&      stats = LockStats();
			const bool la    = Common::Speculation::Active();
			const auto t0    = Bench::NowNs();
			lock.lock();
			(la ? stats.lookahead_waits : stats.gpu_waits).fetch_add(1, std::memory_order_relaxed);
			(la ? stats.lookahead_wait_ns : stats.gpu_wait_ns)
			    .fetch_add(Bench::NowNs() - t0, std::memory_order_relaxed);
		}

		void MaterializeLocked(const ShaderRecompiler::IR::SrtRuntime& runtime,
		                       StepTime*                                materialize_time) {
			Stopwatch  materialize_sw;
			const bool materialized = ShaderRecompiler::IR::MaterializeResources(
			    resource_plan, runtime, resources, specialization);
			if (materialize_time != nullptr) materialize_time->Add(materialize_sw, materialize_sw.Lap());
			EXIT_IF(!materialized);
		}

		template <bool HasCalls>
		Permutation* FindExistingLocked(uint32_t push_data_cursor,
		                                const ShaderRecompiler::IR::ResourceSpecialization& wanted,
		                                std::span<const uint32_t> function_code) {
			Common::GpuPhase::Scope gpu_phase_search(Common::GpuPhase::Phase::PipePermSearch);
			for (auto& candidate: permutations) {
				const auto& layout = candidate.program.bindings;
				if (layout.push_data_start_dword != ShaderRecompiler::IR::PushData::StartFor(
				        push_data_cursor, layout.ShaderDataDwords()) ||
				    candidate.specialization != wanted) {
					continue;
				}
				if constexpr (HasCalls) {
					if (!std::ranges::equal(candidate.function_code, function_code)) continue;
				}
				return &candidate;
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
	                               std::span<const uint32_t> function_code,
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
		const uint64_t spirv_hash =
		    Config::GetLookaheadVerify() ? XXH3_64bits(result.spirv.data(), result.spirv.size() * 4u) : 0u;
		return {
		    .function_code  = {function_code.begin(), function_code.end()},
		    .specialization = std::move(specialization),
		    .program        = std::move(result.program).TakeCompiledInfo(),
		    .handle         = {.id = next_shader_id.fetch_add(1, std::memory_order_relaxed) + 1,
		                       .module = module},
		    .spirv_hash     = spirv_hash,
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
			options.loop_limit     = Config::GetShaderLoopLimit();
			options.shader_hash    = shader_hash;
			Common::LoopGuard::GetMonitor().Register(shader_hash, stage_name);
			options.user_data      = user_data;
			options.back_code      = back_code;
			options.dump_ir        = dump_ir;
			options.early_dump     = dump_ir;
			options.dump_label     = label;
			options.wave_size      = wave_size;
			options.user_data_base = user_data_base;
			options.robust_buffers = cache.robust_buffers;
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
			}
			auto compiled = cache.CompilePermutation(
			    stage_name, options, std::move(translated), key.specialization,
			    key.push_data_cursor, {}, timing ? &backend_ns : nullptr,
			    timing ? &module_ns : nullptr);
			// Jobs only exist for plain (non call-source) programs: no function code.
			compiled.prefetched = IsPrefetched();
			bool dup = false;
			auto* const published = entry->Publish(compiled, key.push_data_cursor, false, dup);
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

	// ---- shader-load jobs (--precompile-on-load / --pipeline-variants-replay) -----------------
	//
	// The guest registered a compute binary (AgcCreateShader). A load job translates it on a worker
	// (low priority, nobody waits), compiles one specialization of it - a stored variant, or the
	// guess - and queues its pipeline. It publishes exactly like a program job, so a dispatch whose
	// specialization is equal finds the permutation and the pipeline ready or in flight. A dispatch
	// that needs an equal specialization while the job runs joins it (WaitFor promotes it), so
	// nothing is compiled twice; a dispatch with another specialization never waits for it.
	struct LoadJob final: AsyncJob {
		LoadJob(PipelineCache& owner_cache, ProgramKey program_key, uint64_t key_hash)
		    : AsyncJob(Kind::Program, key_hash), owner(owner_cache), key(std::move(program_key)) {}

		[[nodiscard]] bool SpecIs(const ShaderRecompiler::IR::ResourceSpecialization& wanted) {
			std::lock_guard lock(spec_mutex);
			return spec.has_value() && *spec == wanted;
		}
		void Execute() override;
		void Cancel() override { Unregister(); }
		void Unregister();

		PipelineCache&        owner;
		ProgramKey            key;
		ShaderComputeInputInfo info;                    // statics of the variant (device values)
		std::vector<uint32_t> code;                     // copy of the binary
		uint32_t              user_data_count = 0;
		int64_t               register_ns     = 0;
		uint32_t              priority        = 0;      // background queue order (first use last time)
		bool                  joinable        = true;   // a dispatch may wait for it (pool jobs only)
		bool                  guess           = true;   // false: `explicit_spec` is a stored variant
		std::optional<ShaderRecompiler::IR::ResourceSpecialization> explicit_spec;

		std::mutex spec_mutex;
		std::optional<ShaderRecompiler::IR::ResourceSpecialization> spec; // what this job builds
	};

	std::mutex                            load_mutex;
	std::vector<std::shared_ptr<LoadJob>> load_active;
	std::unordered_set<uint64_t>          load_hashes; // code hashes registered with a load request

	// Registers a load job unless an equal one is active (same program, same stored variant or
	// both guesses). False: duplicate.
	bool AddLoad(const std::shared_ptr<LoadJob>& job) {
		std::lock_guard lock(load_mutex);
		for (const auto& other: load_active) {
			if (other->key == job->key && other->guess == job->guess &&
			    other->explicit_spec == job->explicit_spec) {
				return false;
			}
		}
		load_active.push_back(job);
		load_hashes.insert(job->key.hash);
		return true;
	}
	void RemoveLoad(const LoadJob* job) {
		std::shared_ptr<LoadJob> released;
		{
			std::lock_guard lock(load_mutex);
			const auto iter = std::ranges::find_if(load_active, [&](const auto& other) {
				return other.get() == job;
			});
			if (iter == load_active.end()) return;
			released = std::move(*iter);
			load_active.erase(iter);
		}
	}
	[[nodiscard]] std::shared_ptr<LoadJob>
	FindLoad(const ProgramKey& key, const ShaderRecompiler::IR::ResourceSpecialization& wanted) {
		std::lock_guard lock(load_mutex);
		for (const auto& job: load_active) {
			if (job->joinable && job->key == key && job->SpecIs(wanted)) return job;
		}
		return nullptr;
	}
	[[nodiscard]] bool LoadedBinary(uint64_t hash) {
		std::lock_guard lock(load_mutex);
		return load_hashes.contains(hash);
	}
	[[nodiscard]] size_t LoadActive() {
		std::lock_guard lock(load_mutex);
		return load_active.size();
	}

	// ---- graphics replay (--pipeline-variants-replay) ---------------------------------------
	// Builds (or finds) the permutation of a stored vertex / mesh / pixel stage program. Worker
	// thread. Returns nullptr when the stage cannot be rebuilt (an S_SWAP_PC program, a stored
	// specialization that no longer fits the translation); the translated source is kept.
	Permutation* BuildStoredStage(const Variants::ProgramVariant& stored, std::span<const uint32_t> code,
	                              std::span<const uint32_t> back_code,
	                              ShaderVertexInputInfo* vertex, ShaderPixelInputInfo* pixel,
	                              LoadCounters& counters, uint32_t& built_count) {
		const auto stage = static_cast<ShaderType>(stored.stage);
		const char* label = nullptr;
		const char* name  = nullptr;
		switch (stage) {
			case ShaderType::Vertex: label = "ShaderRecompiler VS"; name = "vs"; break;
			case ShaderType::Mesh: label = "ShaderRecompiler MS"; name = "ms"; break;
			case ShaderType::Pixel: label = "ShaderRecompiler PS"; name = "ps"; break;
			default: return nullptr;
		}
		ProgramKey key;
		key.stage           = stage;
		key.hash            = stored.code_hash;
		key.user_data_count = stored.user_data_count;
		key.code_size       = stored.code_size_dwords;
		key.static_state    = stored.static_state;
		const std::vector<uint32_t> user_data(stored.user_data_count, 0u);
		ShaderRecompiler::CompileOptions options;
		options.stage       = stage;
		options.shader_hash = stored.code_hash;
		options.user_data   = user_data;
		options.back_code   = back_code;
		options.dump_ir     = Config::GetShaderLogDirection() != Config::LogDirection::Silent;
		options.early_dump  = options.dump_ir;
		options.dump_label  = label;
		if (vertex != nullptr) {
			options.input_info.vertex = vertex;
			options.user_data_base    = 8;
			options.wave_size         = vertex->wave_size;
			if (stage == ShaderType::Mesh) {
				options.user_data_base = 0;
				options.wave_size      = vertex->mesh.wave_size;
			}
		} else {
			options.input_info.pixel = pixel;
			options.wave_size        = pixel->wave_size;
		}
		auto source = ShaderRecompiler::PrepareShaderSource(code, options);
		if (source.call) {
			counters.skipped_call.fetch_add(1, std::memory_order_relaxed);
			return nullptr;
		}
		auto translated = ShaderRecompiler::TranslateProgram(source.decoded, options);
		auto plan       = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
		auto spec       = FromVariantSpec(stored.spec);
		const bool fits = SpecializationFits(plan, spec);
		auto* entry     = InsertEntry(key, std::move(plan), nullptr);
		if (!fits) {
			counters.skipped_stale.fetch_add(1, std::memory_order_relaxed);
			return nullptr;
		}
		if (auto* existing = entry->FindPublished(stored.push_data_cursor, spec)) {
			return existing;
		}
		auto compiled = CompilePermutation(name, options, std::move(translated), spec,
		                                   stored.push_data_cursor, {});
		compiled.from_load     = true;
		compiled.load_ready_ns = Bench::NowNs();
		bool dup               = false;
		auto* const published  = entry->Publish(compiled, stored.push_data_cursor, false, dup);
		if (dup) {
			device.destroyShaderModule(compiled.handle.module, nullptr);
			counters.duplicates.fetch_add(1, std::memory_order_relaxed);
		} else {
			stage_permutation_counts[static_cast<size_t>(stage)].fetch_add(1, std::memory_order_relaxed);
			counters.built.fetch_add(1, std::memory_order_relaxed);
			built_count++;
		}
		return published;
	}

	// One stored graphics pipeline: builds its stage programs, then queues the pipeline.
	struct GfxLoadJob final: AsyncJob {
		GfxLoadJob(PipelineCache& owner_cache, uint64_t key_hash)
		    : AsyncJob(Kind::Program, key_hash), owner(owner_cache) {}
		~GfxLoadJob() override;
		void Execute() override;
		void Cancel() override {}

		PipelineCache&                               owner;
		Variants::GraphicsVariant                    variant;
		std::shared_ptr<const std::vector<uint32_t>> vs_code;
		std::shared_ptr<const std::vector<uint32_t>> ps_code;
		std::shared_ptr<const std::vector<uint32_t>> vs_back_code;
		uint32_t                                     priority = 0;
	};

	struct AsyncContext {
		AsyncPipelineCompiler* pool       = nullptr;
		bool                   allow_skip = false; // graphics only; compute always waits
		bool                   pending    = false; // out: skipped, program not ready
		bool                   waited     = false; // out: the GPU thread waited for a program job
		bool                   guard_waited = false; // out: ... because --skip-guard vetoed the skip
		// Lookahead thread (--pipeline-lookahead): materialize privately, never skip, wait for
		// jobs without accounting GPU-thread waits, never fall back to synchronous compilation.
		bool prefetch = false;
		LookaheadCounters* counters = nullptr; // prefetch only
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

	// Set by the PipelineCache before the first dispatch (null / false when the feature is off).
	Variants::VariantStore*   variants = nullptr;
	LoadCounters*             load     = nullptr;
	bool                      track_use = false;
	int64_t                   clock_start_ns = 0; // first_use_ms of recorded variants counts from here

	// Graphics stage programs as draws used them, by program id (for NoteGraphicsVariant). Only
	// programs a replay can rebuild are kept: vertex / mesh and pixel stages without a back half.
	std::mutex                                       gfx_programs_mutex;
	std::unordered_map<uint64_t, Variants::ProgramVariant> gfx_programs;

	template <typename InputInfo>
	void NoteGraphicsProgramUse(Permutation& permutation, const ShaderParams& params,
	                            const InputInfo& input_info, ShaderType stage, uint32_t cursor_in,
	                            const std::vector<uint32_t>& static_state) {
		if (variants == nullptr || permutation.used.Exchange() ||
		    (stage != ShaderType::Vertex && stage != ShaderType::Mesh && stage != ShaderType::Pixel)) {
			return;
		}
		Variants::ProgramVariant program;
		program.stage            = static_cast<uint8_t>(stage);
		program.code_hash        = params.hash;
		program.code_size_dwords = static_cast<uint32_t>(params.code.size());
		program.user_data_count  = params.user_data_count;
		program.push_data_cursor = cursor_in;
		program.static_state     = static_state;
		program.spec             = ToVariantSpec(permutation.specialization);
		if (!params.back_code.empty()) {
			program.back_hash        = XXH3_64bits(params.back_code.data(), params.back_code.size_bytes());
			program.back_size_dwords = static_cast<uint32_t>(params.back_code.size());
			// The program key's hash of a front/back pair combines both; the guest registers
			// the front binary under its own hash.
			program.front_hash       = XXH3_64bits(params.code.data(), params.code.size_bytes());
		}
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			program.input        = EncodeVertexInfo(input_info);
			program.input_layout = VertexInfoLayout();
		} else {
			program.input        = EncodePixelInfo(input_info);
			program.input_layout = PixelInfoLayout();
		}
		std::lock_guard lock(gfx_programs_mutex);
		gfx_programs[permutation.handle.id] = std::move(program);
	}

	[[nodiscard]] bool FindGraphicsProgram(uint64_t id, Variants::ProgramVariant& out) {
		std::lock_guard lock(gfx_programs_mutex);
		const auto      iter = gfx_programs.find(id);
		if (iter == gfx_programs.end()) return false;
		out = iter->second;
		return true;
	}

	// First use of a compute permutation by a dispatch (GPU thread): records the variant for the
	// next session and accounts a hit of the load-time build. The hot path is one relaxed
	// exchange, and nothing at all when no feature that needs it is on.
	void NoteComputeUse(Permutation& permutation, const ShaderParams& params,
	                    const ShaderComputeInputInfo& input_info) {
		const bool timing = Bench::TimingEnabled();
		if (!track_use && !timing) return;
		if (permutation.used.Exchange()) return;
		if (permutation.from_load && load != nullptr) {
			const auto now = Bench::NowNs();
			load->program_hit.fetch_add(1, std::memory_order_relaxed);
			if (permutation.load_register_ns != 0) load->NoteLead(now - permutation.load_register_ns);
			if (timing) {
				Bench::LogTiming("load_hit", params.hash, now, 0, true,
				                 fmt::format("register_ns={};ready_ns={};lead_ns={};lead_after_ready_ns={}",
				                             permutation.load_register_ns, permutation.load_ready_ns,
				                             now - permutation.load_register_ns,
				                             now - permutation.load_ready_ns));
			}
		}
		if (variants == nullptr && !timing) return;
		Variants::ComputeVariant variant;
		variant.code_hash        = params.hash;
		variant.code_size_dwords = static_cast<uint32_t>(params.code.size());
		variant.state            = ToVariantStatic(input_info, params.user_data_count);
		variant.spec             = ToVariantSpec(permutation.specialization);
		variant.first_use_ms     = static_cast<uint32_t>(std::min<int64_t>(
		    (Bench::NowNs() - clock_start_ns) / 1000000, 0xfffffffeLL));
		const bool added = variants != nullptr && variants->Add(variant);
		if (added && load != nullptr) load->recorded.fetch_add(1, std::memory_order_relaxed);
		if (timing) {
			Bench::LogTiming("compute_variant", params.hash, Bench::NowNs(), 0, true,
			                 fmt::format("code_dwords={};new={};from_load={};{}", params.code.size(),
			                             added ? 1 : 0, permutation.from_load ? 1 : 0,
			                             DescribeVariant(variant)));
		}
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
		// --srt-plan-shadow: which draw's Get() call this is (in the lookahead and on kyGpu alike).
		SrtPlan::Key plan_key;
		bool         plan_nested = false;
		const bool   plan_active = SrtPlan::Enabled() && SrtPlan::NextCall(plan_key, plan_nested);
		// Per-call key. A thread_local scratch object avoids a heap allocation per draw (the
		// static_state vector keeps its capacity) while never being shared between threads or
		// between calls: Get() is not re-entrant on one thread.
		thread_local ProgramKey lookup_key = [] {
			ProgramKey key;
			key.static_state.reserve(MaxStaticKeyWords);
			return key;
		}();
		Common::GpuPhase::Scope gpu_phase_key(Common::GpuPhase::Phase::PipeProgramKey);
		lookup_key.stage           = stage;
		lookup_key.hash            = params.hash;
		lookup_key.user_data_count = params.user_data_count;
		lookup_key.code_size       = static_cast<uint32_t>(params.code.size());
		BuildStageStaticKey(input_info, lookup_key.static_state);
		const bool timing = Bench::TimingEnabled();
		StepTime   refresh_time;
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
			report("swappc_refresh", Bench::HitSlot::SwapPcRefresh, refresh_time);
			report("materialize", Bench::HitSlot::Materialize, materialize_time);
		};
		SourceEntry* entry = FindEntry(lookup_key);
		gpu_phase_key.End();
		const bool prefetch = async != nullptr && async->prefetch;
		// kyGpu, --srt-plans: adopt the lookahead's plan of this draw (with --srt-plan-shadow also
		// verify it); --srt-plan-shadow alone only compares.
		const bool plan_adopt  = plan_active && !prefetch && SrtPlan::AdoptEnabled();
		const bool plan_shadow = plan_active && !prefetch && !plan_adopt && SrtPlan::ShadowEnabled();
		ShaderRecompiler::IR::SrtRuntime             runtime {
		    .user_data                  = user_data,
		    .shader_base                = params.Base(),
		    .read_memory                = ReadShaderScalarMemory,
		    .read_specialization_memory =
		        prefetch ? ReadShaderGuestMemorySpeculative : ReadShaderGuestMemory,
		};
		runtime.table_walk   = Config::SrtTableWalkEnabled();
		runtime.table_verify = runtime.table_walk && Config::DrawKeyVerifyEnabled();
		if constexpr (std::is_same_v<InputInfo, ShaderComputeInputInfo>) {
			runtime.workgroup_counts = input_info.workgroup_counts;
		}
		// --srt-plan-shadow, lookahead thread: the walk's reads go through recording readers.
		SrtPlan::ReadRecorder* plan_recorder = nullptr;
		std::optional<ShaderRecompiler::IR::ResourceSnapshot> plan_snapshot;
		// --plan-pool: the plan this draw's walk is written into, taken from the pool (its snapshot and
		// specialization are the walk's output, nothing is copied into it afterwards).
		std::unique_ptr<SrtPlan::Plan> plan_lease;
		struct LeaseReturn { // a plan this call does not hand over (call-source program, failed walk) goes back
			std::unique_ptr<SrtPlan::Plan>& lease;
			~LeaseReturn() {
				if (lease) SrtPlan::GetPlanPool().Return(std::move(lease));
			}
		} lease_return {plan_lease};
		if (prefetch && plan_active) {
			if (SrtPlan::PoolEnabled()) {
				plan_lease = SrtPlan::GetPlanPool().Acquire();
			} else {
				plan_snapshot.emplace();
			}
			thread_local SrtPlan::ReadRecorder recorder;
			recorder.Clear();
			plan_recorder                       = &recorder;
			runtime.userdata                    = plan_recorder;
			runtime.read_memory                 = RecordingScalarRead;
			runtime.read_specialization_memory  = RecordingSpecializationRead;
		}
		// The draw/dispatch (not a prefetch) is about to use `p`: variant recording and load hits.
		const uint32_t cursor_in = push_data_cursor;
		const auto note_use = [&](Permutation& p) {
			if (prefetch || (entry != nullptr && entry->call_source != nullptr)) return;
			if constexpr (std::is_same_v<InputInfo, ShaderComputeInputInfo>) {
				NoteComputeUse(p, params, input_info);
			} else {
				NoteGraphicsProgramUse(p, params, input_info, stage, cursor_in, lookup_key.static_state);
			}
		};
		bool source_changed = false;
		int64_t materialize_t0 = 0;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		Permutation* verify_target = nullptr;
		if (entry == nullptr && plan_adopt) {
			auto& counts = SrtPlan::Adopting();
			++counts.events;
			++counts.new_source;
		}
		if (entry == nullptr && plan_shadow) {
			// A shader kyGpu sees for the first time: the lookahead cannot have a plan for it.
			SrtPlan::Sample sample;
			sample.outcome = SrtPlan::Outcome::NewSource;
			sample.compute = std::is_same_v<InputInfo, ShaderComputeInputInfo>;
			SrtPlan::Record(sample);
		}
		if (entry != nullptr) {
			Permutation* permutation = nullptr;
			Common::GpuPhase::Scope gpu_phase_materialize(
			    Common::GpuPhase::Phase::PipeMaterialize);
			if (prefetch) {
				// Lookahead thread: call-source (S_SWAP_PC) programs are left to the GPU thread
				// (refreshing the linked callee reads live guest memory).
				if (entry->call_source) {
					return {};
				}
				bool materialized = false;
				permutation = entry->PeekPermutation(
				    runtime, push_data_cursor, plan_lease ? plan_lease->specialization : specialization, materialized,
				    plan_lease ? &plan_lease->snapshot : plan_recorder != nullptr ? &*plan_snapshot : nullptr);
				if (plan_recorder != nullptr) {
					if (materialized) {
						const auto t_plan = SrtPlan::NowNs();
						auto       plan   = plan_lease ? std::move(plan_lease) : std::make_unique<SrtPlan::Plan>();
						plan->key         = plan_key;
						plan->stage       = static_cast<uint32_t>(stage);
						plan->shader_hash = params.hash;
						plan->shader_base = runtime.shader_base;
						if (!plan_snapshot) {
							// Pooled: the walk already wrote the plan's own snapshot and specialization. A draw
							// that found no permutation goes on to compile with `specialization` after the plan
							// is handed over (and maybe reused), so it gets its own copy.
							if (permutation == nullptr) specialization = plan->specialization;
						} else {
							plan->snapshot       = std::move(*plan_snapshot);
							plan->specialization = specialization;
						}
						plan->workgroup_counts.assign(runtime.workgroup_counts.begin(),
						                              runtime.workgroup_counts.end());
						plan_recorder->FinishInto(plan->reads);
						plan->entry              = entry;
						plan->permutation        = permutation;
						plan->permutation_cursor = push_data_cursor;
						if (Config::ResolvedDrawEnabled()) {
							// Resolved draw: parts of the draw that depend only on the snapshot.
							BuildReadHull(plan->snapshot.specialization_reads, plan->snapshot.read_hull);
						}
						plan->produced_serial = SrtPlan::ProducerEvents();
						const auto bytes      = plan->Bytes();
						SrtPlan::NoteProduceCost(SrtPlan::NowNs() - t_plan);
						plan->produced_ns = SrtPlan::NowNs();
						SrtPlan::PutPlan(std::move(plan));
						SrtPlan::NoteProduced(bytes);
					} else {
						SrtPlan::NoteProduceFailed();
					}
				}
				if (permutation != nullptr) {
					input_info.stage = {.program = &permutation->program, .resources = &PrefetchResources()};
					permutation->program.bindings.AdvancePushData(push_data_cursor);
					return permutation->handle;
				}
				if (!materialized) {
					async->counters->skip[LookaheadCounters::SkipMaterializeFailed].fetch_add(
					    1, std::memory_order_relaxed);
					return {};
				}
			} else if (entry->call_source) {
				materialize_t0 = plan_shadow ? SrtPlan::NowNs() : 0;
				if (plan_adopt) {
					auto& counts = SrtPlan::Adopting();
					++counts.events; // call-source programs are left to kyGpu: the lookahead skips them
					++counts.call_source;
				}
				Stopwatch refresh_sw;
				source_changed = entry->RefreshCallSource(runtime);
				refresh_time.Add(refresh_sw, refresh_sw.Lap());
				if (!source_changed)
					permutation = entry->FindPermutation<true>(runtime, push_data_cursor, &materialize_time,
					                                           &specialization);
			} else {
				std::unique_ptr<SrtPlan::Plan> adopted;
				if (plan_adopt) {
					adopted = TakeAdoptablePlan(plan_key, plan_nested, static_cast<uint32_t>(stage),
					                            params.hash, runtime);
				}
				if (adopted) {
					permutation = entry->FindPermutationAdopting(*adopted, push_data_cursor, &specialization);
					SrtPlan::RecyclePlan(std::move(adopted));
					if (SrtPlan::ShadowEnabled()) {
						SrtPlan::Adopting().NoteVerified(entry->DiffAdoptedAgainstOwn(runtime));
					}
				} else {
					materialize_t0 = plan_shadow ? SrtPlan::NowNs() : 0;
					permutation = entry->FindPermutation<false>(runtime, push_data_cursor, &materialize_time,
					                                            &specialization);
				}
			}
			gpu_phase_materialize.End();
			if (plan_shadow) {
				// kyGpu has its own result in entry->resources / entry->specialization (sole writer).
				ShadowCompare(plan_key, plan_nested, std::is_same_v<InputInfo, ShaderComputeInputInfo>,
				              static_cast<uint32_t>(stage), params.hash, runtime, entry->resources,
				              entry->specialization, SrtPlan::NowNs() - materialize_t0);
			}
			if (permutation != nullptr && !prefetch && Config::GetLookaheadVerify() &&
			    permutation->prefetched && !permutation->verified) {
				// --debug-lookahead-verify: recompile it here with the real draw's inputs and
				// compare below before the first use.
				verify_target  = permutation;
				specialization = permutation->specialization;
				permutation    = nullptr;
			}
			if (permutation != nullptr) {
				input_info.stage = {.program = &permutation->program, .resources = &entry->resources};
				permutation->program.bindings.AdvancePushData(push_data_cursor);
				note_use(*permutation);
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
		    (async != nullptr && async->pool != nullptr && async->pool->Running() &&
		     verify_target == nullptr) ? async->pool : nullptr;
		const bool skip_candidate = pool != nullptr && async->allow_skip && stage != ShaderType::Compute;
		// --skip-guard: the draw writes memory a hard-class reader has read from a skipped draw
		// before: wait for the program like stall mode (asked only here, on a miss).
		const bool guard_veto = skip_candidate && SkipDeps::GuardVetoesSkip();
		const bool may_skip   = skip_candidate && !guard_veto;
		// Uses a permutation another thread (or an earlier request) published for this draw.
		const auto use_published = [&](Permutation& permutation, bool miss_report) {
			input_info.stage = {.program = &permutation.program,
			                    .resources = prefetch ? &PrefetchResources() : &entry->resources};
			permutation.program.bindings.AdvancePushData(push_data_cursor);
			note_use(permutation);
			report_steps(miss_report);
			return permutation.handle;
		};
		const auto wait_job = [&](AsyncJob& job) {
			if (!prefetch) async->waited = true;
			if (guard_veto && !prefetch) async->guard_waited = true;
			if (!prefetch) return pool->WaitFor(job);
			// The lookahead blocks here for as long as the compile takes. Under --async-pipelines
			// skip the GPU thread does not wait with it, so the submission being scanned can be
			// executed and its memory recycled meanwhile: what the lookahead reads after this wait
			// (descriptor tables, indirect register tables) is not the draw's any more.
			const auto t0   = Bench::NowNs();
			const bool done = job.Wait();
			LockStats().compile_waits.fetch_add(1, std::memory_order_relaxed);
			LockStats().compile_wait_ns.fetch_add(Bench::NowNs() - t0, std::memory_order_relaxed);
			return done && !Common::Speculation::ItemStale();
		};
		const auto note_skip = [&] {
			pool->Stats().skipped_program_draws.fetch_add(1, std::memory_order_relaxed);
			async->pending = true;
			report_steps(false);
		};
		ProgramJobKey job_key;
		if (pool != nullptr && entry != nullptr && entry->call_source == nullptr) {
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
			if constexpr (std::is_same_v<InputInfo, ShaderComputeInputInfo>) {
				// A shader-load job is already building exactly this specialization: join it
				// instead of compiling the same SPIR-V again.
				if (load != nullptr && track_use) {
					if (auto load_job = FindLoad(lookup_key, specialization)) {
						if (wait_job(*load_job)) {
							if (auto* published = entry->FindPublished(push_data_cursor, specialization)) {
								return use_published(*published, true);
							}
						}
						if (prefetch) {
							return {};
						}
					} else if (!prefetch && LoadedBinary(params.hash)) {
						load->program_other.fetch_add(1, std::memory_order_relaxed);
						if (Bench::TimingEnabled()) {
							Bench::LogTiming("load_miss", params.hash, Bench::NowNs(), 0, true,
							                 "reason=other_variant");
						}
					}
				}
			}
		}

		Stopwatch miss_sw;
		int64_t   prepare_ns = 0, frontend_ns = 0, plan_ns = 0, backend_ns = 0, module_ns = 0;
		const bool new_source = entry == nullptr;
		if constexpr (std::is_same_v<InputInfo, ShaderComputeInputInfo>) {
			// The binary was registered with a load request but this dispatch's static state
			// (wave size, dispatch mode, queue, ...) is another one than was built.
			if (new_source && !prefetch && load != nullptr && track_use && LoadedBinary(params.hash)) {
				load->program_other.fetch_add(1, std::memory_order_relaxed);
				if (timing) {
					Bench::LogTiming("load_miss", params.hash, Bench::NowNs(), 0, true,
					                 "reason=other_static_state");
				}
			}
		}

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
		options.loop_limit  = Config::GetShaderLoopLimit();
		options.shader_hash = params.hash;
		Common::LoopGuard::GetMonitor().Register(params.hash, stage_name);
		options.user_data   = user_data;
		options.back_code      = params.back_code;
		options.dump_ir     = Config::GetShaderLogDirection() != Config::LogDirection::Silent;
		options.early_dump  = options.dump_ir;
		options.dump_label  = label;
		options.input_info  = stage_input;
		options.robust_buffers = robust_buffers;

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
			// A request derived from guest memory the GPU thread may have released is dropped, not
			// compiled: it would build a program for a specialization nobody asked for.
			if (prefetch && Common::Speculation::ItemStale()) {
				return nullptr;
			}
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

		ShaderRecompiler::ShaderSource source;
		if (entry == nullptr) source = ShaderRecompiler::PrepareShaderSource(params.code, options);
		// call-source (S_SWAP_PC) programs keep the synchronous path: their translation depends on
		// the linked callee snapshot, which is refreshed under the entry lock.
		const bool call_shader = entry == nullptr ? static_cast<bool>(source.call)
		                                         : entry->call_source != nullptr;
		if (prefetch && call_shader) {
			return {}; // left to the GPU thread (see above)
		}
		if (pool != nullptr && entry != nullptr && !call_shader) {
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
		auto* current = entry == nullptr ? &source : entry->call_source.get();
		ShaderRecompiler::TranslateResult translated;
		prepare_ns = miss_sw.Lap();
		// The linked callee snapshot translation used, and where its function code starts; the
		// permutation records exactly this function code, even if another thread refreshes later.
		LinkedPtr translated_linked;
		size_t    base_code_size = 0;
		if (current != nullptr) {
			if (current->call) {
				base_code_size = current->code.size();
				if (entry == nullptr) {
					// `source` is still private to this call: no lock needed.
					Stopwatch refresh_sw;
					ShaderRecompiler::RefreshShaderSource(*current, runtime);
					refresh_time.Add(refresh_sw, refresh_sw.Lap());
					miss_sw.Lap(); // refresh is reported on its own row, not as translate time
					translated_linked = current->linked;
					translated        = ShaderRecompiler::TranslateProgram(translated_linked->decoded, options);
					translated.program.source_reads = current->reads;
				} else {
					auto snapshot     = entry->SnapshotCall();
					translated_linked = snapshot.linked;
					translated        = ShaderRecompiler::TranslateProgram(translated_linked->decoded, options);
					translated.program.source_reads = std::move(snapshot.reads);
				}
			} else {
				translated = ShaderRecompiler::TranslateProgram(current->decoded, options);
			}
		} else {
			translated = ShaderRecompiler::TranslateProgram(params.code, options);
		}
		frontend_ns = miss_sw.Lap();
		// One row per program translation (a miss). dur_ns = translate_ns = prepare + frontend +
		// plan + backend + module; MaterializeResources and the S_SWAP_PC refresh are reported on
		// their own rows (miss=1), and the detail field carries materialize_ns for convenience.
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
			auto retained = source.call ? std::make_unique<ShaderRecompiler::ShaderSource>(std::move(source)) : nullptr;
			entry = InsertEntry(lookup_key, ShaderRecompiler::IR::ExtractResourcePlan(translated.program),
			                    std::move(retained));
			plan_ns = miss_sw.Lap();
			if (prefetch) {
				if (!entry->MaterializePrivate(runtime, specialization)) {
					async->counters->skip[LookaheadCounters::SkipMaterializeFailed].fetch_add(
					    1, std::memory_order_relaxed);
					return {};
				}
			} else {
				specialization = entry->Materialize(runtime, &materialize_time);
			}
			miss_sw.Lap();
			if (pool != nullptr && !call_shader) {
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
		} else if (source_changed) {
			entry->SetResourcePlan(ShaderRecompiler::IR::ExtractResourcePlan(translated.program));
			plan_ns = miss_sw.Lap();
			if (const auto* permutation = entry->FindPermutation<true>(
			        runtime, push_data_cursor, &materialize_time, &specialization)) {
				miss_sw.Lap();
				input_info.stage = {.program = &permutation->program, .resources = &entry->resources};
				permutation->program.bindings.AdvancePushData(push_data_cursor);
				log_translate(true, permutation->handle.id);
				report_steps(true);
				return permutation->handle;
			}
			miss_sw.Lap();
		}
		auto compiled = CompilePermutation(
		    stage_name, options, std::move(translated), std::move(specialization), push_data_cursor,
		    SourceEntry::FunctionCode(translated_linked, base_code_size), timing ? &backend_ns : nullptr,
		    timing ? &module_ns : nullptr);
		if (verify_target != nullptr) {
			verify_target->verified = true;
			static std::atomic<uint64_t> verified_count {0}, mismatch_count {0};
			verified_count.fetch_add(1, std::memory_order_relaxed);
			if (compiled.spirv_hash != verify_target->spirv_hash) {
				const auto n = mismatch_count.fetch_add(1, std::memory_order_relaxed) + 1;
				std::fprintf(stderr,
				             "Lookahead verify: MISMATCH #%llu stage=%s shader_hash=%016llx "
				             "prefetched_spirv=%016llx real_spirv=%016llx (verified so far %llu)\n",
				             static_cast<unsigned long long>(n), stage_name,
				             static_cast<unsigned long long>(params.hash),
				             static_cast<unsigned long long>(verify_target->spirv_hash),
				             static_cast<unsigned long long>(compiled.spirv_hash),
				             static_cast<unsigned long long>(verified_count.load()));
			}
		}
		bool        duplicate = false;
		auto* const published = entry->Publish(compiled, push_data_cursor, translated_linked != nullptr, duplicate);
		if (duplicate) {
			// Another thread published an equal permutation first: drop our module, use theirs.
			device.destroyShaderModule(compiled.handle.module, nullptr);
		} else {
			stage_permutation_counts[static_cast<size_t>(stage)].fetch_add(1, std::memory_order_relaxed);
		}
		const auto& permutation = *published;
		input_info.stage = {.program = &permutation.program, .resources = &entry->resources};
		permutation.program.bindings.AdvancePushData(push_data_cursor);
		note_use(*published);
		log_translate(duplicate, permutation.handle.id);
		report_steps(true);

		PrintShaderCounts();
		return permutation.handle;
	}

	explicit ProgramCache(vk::Device device): device(device) {}

	// Storage-buffer accesses the emitter leaves to robustBufferAccess2 (--spirv-robust-bounds);
	// fixed after construction, read by the worker threads.
	ShaderRecompiler::RobustBufferPolicy robust_buffers;
	~ProgramCache() {
		for (const auto& [key, entry]: programs) {
			(void)key;
			for (const auto& permutation: entry.permutations) {
				device.destroyShaderModule(permutation.handle.module, nullptr);
			}
		}
	}

	[[nodiscard]] SourceEntry* FindEntry(const ProgramKey& key) {
		// --scalable-read-locks: no lock and no locked instruction on a hit; a miss repeats the lookup under the lock.
		if (Common::ScalableLocksEnabled()) {
			if (const auto* node = programs_index.Find(key)) {
				return const_cast<SourceEntry*>(&node->second);
			}
		}
		Common::ScalableSharedMutex::SharedGuard lock(programs_mutex);
		const auto                               iter = programs.find(key);
		return iter != programs.end() ? &iter->second : nullptr;
	}

	// Inserts a new entry, or returns the one another thread inserted first (the caller then
	// treats it like any existing entry; its own translation result is simply discarded).
	SourceEntry* InsertEntry(const ProgramKey& key, ShaderRecompiler::IR::ResourcePlan plan,
	                         std::unique_ptr<ShaderRecompiler::ShaderSource> call_source) {
		Common::ScalableSharedMutex::ExclusiveGuard lock(programs_mutex);
		auto [iter, inserted] = programs.try_emplace(key, std::move(plan), std::move(call_source));
		if (inserted) {
			programs_index.Insert(&*iter);
		}
		return &iter->second;
	}

	Common::ScalableSharedMutex                                 programs_mutex;
	std::unordered_map<ProgramKey, SourceEntry, ProgramKeyHash> programs;
	struct ProgramNodeKey {
		const ProgramKey& operator()(const std::pair<const ProgramKey, SourceEntry>& node) const noexcept { return node.first; }
	};
	// The lock-free read side of `programs` (common/publishedIndex.h); see FindEntry.
	Common::PublishedIndex<std::pair<const ProgramKey, SourceEntry>, ProgramKey, ProgramNodeKey, ProgramKeyHash, std::equal_to<ProgramKey>>
	    programs_index;
	// In-flight --async-pipelines program jobs, keyed by (program, specialization, push cursor).
	InFlightRegistry<ProgramJobKey, ProgramJobKeyHash>          program_jobs;
	vk::Device                                                  device;
	std::atomic<uint64_t>                                       next_shader_id {0};
	// Number of published permutations per logical stage, kept incrementally so the "Shaders:"
	// progress line does not have to walk (and lock) the whole map on every miss.
	std::array<std::atomic<size_t>, static_cast<size_t>(ShaderType::TessellationEvaluation) + 1>
	    stage_permutation_counts {};
};

// --precompile-on-load: translating and compiling the guessed variant of every registered compute
// binary touches binaries the game may never use, and a translator check that fires on one of them
// (an unimplemented instruction) must not end the emulator. This thread runs the guess jobs inside
// a speculation scope like the lookahead thread: such a check parks the thread, switches the
// speculative features off for the rest of the run and leaves the real path alone. It runs at a
// lower scheduling priority than the game's threads.
class PipelineCache::GuessThread {
public:
	GuessThread(): m_state(std::make_shared<State>()) { SpawnWorker(m_state); }
	~GuessThread() { Stop(); }
	GuessThread(const GuessThread&)            = delete;
	GuessThread& operator=(const GuessThread&) = delete;

	void Push(std::shared_ptr<ProgramCache::LoadJob> job) {
		{
			std::lock_guard lock(m_state->mutex);
			if (m_state->stopping || m_state->workers_lost > kMaxLostWorkers) return;
			m_state->queue.push_back(std::move(job));
		}
		m_state->cv.notify_one();
	}

	// Stops accepting work and waits for the running job to finish. A worker parked by a fatal
	// check is not waited for (it no longer touches the cache).
	void Stop() {
		{
			std::lock_guard lock(m_state->mutex);
			if (m_state->stopping) return;
			m_state->stopping = true;
			m_state->queue.clear();
		}
		m_state->cv.notify_all();
		for (int i = 0; i < 4000 && m_state->active.load(std::memory_order_acquire) != 0; i++) {
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}

private:
	static constexpr uint32_t kMaxLostWorkers = 64;

	struct State {
		std::mutex                                         mutex;
		std::condition_variable                            cv;
		std::deque<std::shared_ptr<ProgramCache::LoadJob>> queue;
		bool                                               stopping     = false;
		uint32_t                                           workers_lost = 0;
		std::atomic<uint32_t>                              active {0}; // workers that can touch the cache
	};
	struct Context {
		std::shared_ptr<State>                  state;
		std::shared_ptr<ProgramCache::LoadJob> job;
	};

	// Workers are detached and own a share of the state, so a worker parked by a fatal check
	// never refers to memory that is gone.
	static void SpawnWorker(const std::shared_ptr<State>& state) {
		state->active.fetch_add(1, std::memory_order_acq_rel);
		std::thread([state] { Run(state); }).detach();
	}

	// A fatal check on the worker: the job is dropped, a replacement starts, this thread parks.
	static void OnAbort(void* user) {
		auto* context = static_cast<Context*>(user);
		if (context->job != nullptr) {
			context->job->Unregister();
			context->job.reset();
		}
		bool respawn = false;
		{
			std::lock_guard lock(context->state->mutex);
			context->state->workers_lost++;
			respawn = !context->state->stopping && context->state->workers_lost <= kMaxLostWorkers;
		}
		if (respawn) SpawnWorker(context->state);
		context->state->active.fetch_sub(1, std::memory_order_acq_rel);
	}

	static void Run(std::shared_ptr<State> state) {
#if defined(__linux__)
		pthread_setname_np(pthread_self(), "kyty-precompile");
		(void)setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#endif
		Context context {state, nullptr};
		Common::Speculation::SetThreadAbortHandler(&OnAbort, &context);
		Common::Speculation::Scope speculation;
		for (;;) {
			{
				std::unique_lock lock(state->mutex);
				state->cv.wait(lock, [&] { return state->stopping || !state->queue.empty(); });
				if (state->stopping) break;
				context.job = std::move(state->queue.front());
				state->queue.pop_front();
			}
			context.job->Execute();
			context.job.reset();
		}
		state->active.fetch_sub(1, std::memory_order_acq_rel);
	}

	std::shared_ptr<State> m_state;
};

PipelineCache::PipelineCache(GraphicContext& graphics)
    : m_graphics(graphics), m_program_cache(std::make_unique<ProgramCache>(graphics.device)),
      m_async_mode(Config::GetAsyncPipelineMode()) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	m_program_cache->robust_buffers = graphics.SpirvRobustBufferPolicy();
	if (m_program_cache->robust_buffers.Any()) {
		PipelineCacheLog("SPIR-V robust bounds: storage-buffer accesses use robustBufferAccess2 "
		                 "(dword elements={} sub-dword={})",
		                 m_program_cache->robust_buffers.dword_elements,
		                 m_program_cache->robust_buffers.subdword_elements);
	}
	m_draw_key_cache  = Config::DrawKeyCacheEnabled();
	m_draw_key_verify = Config::DrawKeyVerifyEnabled();
	InitializeDriverCache();
	if (m_async_mode == Config::AsyncPipelineMode::Skip) {
		SkipDeps::ConfigureGuard(Config::SkipGuardEnabled(), Config::SkipGuardBdaEnabled());
	} else if (Config::SkipGuardRequested()) {
		PipelineCacheLog("SkipGuard: --skip-guard needs --async-pipelines skip, ignored");
	}
	if (m_async_mode != Config::AsyncPipelineMode::Off) {
		const auto workers = Config::GetPipelineWorkerCount();
		m_async            = std::make_unique<AsyncPipelineCompiler>(m_async_mode, workers);
		PipelineCacheLog("Async pipelines: mode={} workers={}", AsyncModeName(m_async_mode),
		                 m_async->WorkerCount());
		if (Common::Hang::Enabled()) {
			Common::Hang::RegisterProvider("pipeline_pool", [this] {
				return m_async->DebugState() + " | program_jobs: " +
				       m_program_cache->program_jobs.Describe() + " | graphics_jobs: " +
				       m_graphics_jobs.Describe() + " | compute_jobs: " +
				       m_compute_jobs.Describe();
			});
		}
	}
	DecideUnoptimizedFirst();
	InitializeVariants();
}

std::string PipelineCache::LoadCounters::Summary() const {
	const auto n = lead_n.load();
	return fmt::format(
	    "registered_cs={};registered_other={};stored_variants={};jobs_stored={};jobs_guess={};"
	    "skipped_unguessable={};skipped_call={};skipped_stale={};skipped_known={};skipped_registers={};built={};"
	    "duplicates={};pipelines={};program_hit={};program_other={};pipeline_hit_ready={};"
	    "pipeline_hit_inflight={};recorded={};recorded_graphics={};gfx_stored={};gfx_jobs={};gfx_pipelines={};gfx_skipped={};gfx_unrecorded={};gfx_hit_ready={};gfx_hit_inflight={};lead_n={};lead_avg_ms={:.1f};lead_min_ms={:.1f};"
	    "lead_max_ms={:.1f}",
	    registered_cs.load(), registered_other.load(), stored_variants.load(), jobs_stored.load(),
	    jobs_guess.load(), skipped_unguessable.load(), skipped_call.load(), skipped_stale.load(), skipped_known.load(),
	    skipped_registers.load(), built.load(), duplicates.load(), pipelines.load(),
	    program_hit.load(), program_other.load(), pipeline_hit_ready.load(),
	    pipeline_hit_inflight.load(), recorded.load(), recorded_graphics.load(), gfx_stored.load(), gfx_jobs.load(), gfx_pipelines.load(), gfx_skipped.load(), gfx_unrecorded.load(), gfx_hit_ready.load(), gfx_hit_inflight.load(), n,
	    n != 0 ? static_cast<double>(lead_sum_ns.load()) / static_cast<double>(n) / 1e6 : 0.0,
	    n != 0 ? static_cast<double>(lead_min_ns.load()) / 1e6 : 0.0,
	    static_cast<double>(lead_max_ns.load()) / 1e6);
}

// --pipeline-variants-replay: opens <TITLE>.variants next to the driver cache. The store records
// every compute variant a dispatch uses (ProgramCache::NoteComputeUse) and, from the next session
// on, answers NoteShaderRegistered(). It holds keys only: no code, no SPIR-V.
void PipelineCache::InitializeVariants() {
	m_program_cache->load = &m_load;
	m_program_cache->clock_start_ns = Bench::NowNs();
	if (!Config::GetPipelineVariantsReplay() && !Config::GetPrecompileOnLoad()) {
		return;
	}
	m_program_cache->track_use = true;
	if (Config::GetPrecompileOnLoad()) {
		m_guess_thread = std::make_unique<GuessThread>();
	}
	if (!Config::GetPipelineVariantsReplay()) {
		return;
	}
	const auto title_id = PipelineCacheTitleId();
	if (title_id.empty()) {
		PipelineCacheLog("Pipeline variants: disabled (no title id)");
		return;
	}
	UseVariantStore(std::filesystem::path("_PipelineCache") / (title_id + ".variants"));
}

bool PipelineCache::UseVariantStore(const std::filesystem::path& path) {
	auto       store  = std::make_unique<Variants::VariantStore>();
	const auto loaded = store->Load(path);
	if (!store->OpenAppend(path)) {
		PipelineCacheLog("Pipeline variants: disabled (cannot write {})", Common::PathToString(path));
		return false;
	}
	PipelineCacheLog("Pipeline variants: {} ({} stored, {} records, torn_tail={}, schema_ok={})",
	                 Common::PathToString(path), store->Size(), loaded.records, loaded.torn_tail,
	                 loaded.schema_ok);
	if (Bench::TimingEnabled()) {
		Bench::LogTiming("variants_init", 0, Bench::NowNs(), 0, false,
		                 fmt::format("stored={};records={};torn_tail={};file_found={}", store->Size(),
		                             loaded.records, loaded.torn_tail ? 1 : 0, loaded.file_found ? 1 : 0));
	}
	m_variants                = std::move(store);
	m_program_cache->variants = m_variants.get();
	m_program_cache->track_use = true;
	{
		std::lock_guard lock(m_gfx_mutex);
		m_gfx_pending.clear();
		m_gfx_by_hash.clear();
		for (auto& stored: m_variants->Graphics()) {
			const size_t index = m_gfx_pending.size();
			m_gfx_by_hash[stored.vs.RegistrationHash()].push_back(index);
			if (stored.vs.back_hash != 0) m_gfx_by_hash[stored.vs.back_hash].push_back(index);
			if (stored.has_ps) m_gfx_by_hash[stored.ps.code_hash].push_back(index);
			m_gfx_pending.push_back({.variant = std::move(stored)});
		}
		m_load.gfx_stored.store(m_gfx_pending.size(), std::memory_order_relaxed);
	}
	return true;
}

// --pipelines-unoptimized-first is active only where it is safe and effective:
//  * async pipelines are on (the optimized rebuilds run on the pool);
//  * NVIDIA, the only vendor it was measured on. An AMD RDNA2 driver crashed in the fork that
//    introduced it, so other vendors need KYTY_PIPELINE_FAST_BUILD=1 (and =0 turns it off);
//  * the driver cache can be asked without compiling (pipelineCreationCacheControl), or there is no
//    driver cache at all. Otherwise a warm cache would be ignored in favour of unoptimized code.
void PipelineCache::DecideUnoptimizedFirst() {
	m_unoptimized_first = false;
	if (!Config::GetPipelinesUnoptimizedFirst()) {
		return;
	}
	const char* env = std::getenv("KYTY_PIPELINE_FAST_BUILD");
	if (env != nullptr && std::string_view(env) == "0") {
		PipelineCacheLog("Unoptimized-first pipelines: off (KYTY_PIPELINE_FAST_BUILD=0)");
		return;
	}
	constexpr uint32_t NvidiaVendor = 0x10DE;
	const bool         forced       = env != nullptr && std::string_view(env) == "1";
	if (m_graphics.GetPhysicalDeviceProperties().vendorID != NvidiaVendor && !forced) {
		PipelineCacheLog("Unoptimized-first pipelines: off (only measured on NVIDIA; "
		                 "KYTY_PIPELINE_FAST_BUILD=1 forces it)");
		return;
	}
	if (m_driver_cache != nullptr && !m_graphics.pipeline_cache_control_enabled) {
		PipelineCacheLog("Unoptimized-first pipelines: off (the driver cannot probe its pipeline "
		                 "cache without compiling: pipelineCreationCacheControl)");
		return;
	}
	m_unoptimized_first = true;
	PipelineCacheLog("Unoptimized-first pipelines: on (driver cache {})",
	                 m_driver_cache != nullptr ? "probed first" : "disabled");
}

void PipelineCache::NoteFirstBuild(const Pipeline& created) {
	if (!m_unoptimized_first) {
		return;
	}
	(created.unoptimized ? m_unoptimized_counters.unoptimized_created
	                     : m_unoptimized_counters.served_from_cache)
	    .fetch_add(1, std::memory_order_relaxed);
}

// GPU thread. Swaps the optimized pipeline in once a worker has built it.
void PipelineCache::PromoteOptimized(Pipeline& pipeline) {
	if (pipeline.optimized == nullptr || !pipeline.optimized->done.load(std::memory_order_acquire)) {
		return;
	}
	const auto optimized = std::move(pipeline.optimized);
	pipeline.optimized.reset();
	pipeline.unoptimized = false;
	if (optimized->pipeline == nullptr) {
		return;
	}
	const auto replaced = pipeline.pipeline;
	pipeline.pipeline   = optimized->pipeline;
	m_unoptimized_counters.swapped.fetch_add(1, std::memory_order_relaxed);
	if (replaced == nullptr) {
		return;
	}
	if (m_retire_handler && m_retire_handler(replaced)) {
		m_unoptimized_counters.retired.fetch_add(1, std::memory_order_relaxed);
	} else {
		m_retired_pipelines.push_back(replaced);
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
	Common::Hang::UnregisterProvider("pipeline_pool");
	if (m_guess_thread != nullptr) {
		m_guess_thread->Stop();
	}
	m_async->Shutdown();
	m_program_cache->program_jobs.Clear();
	m_graphics_jobs.Clear();
	m_compute_jobs.Clear();
	PipelineCacheLog("Async pipelines: stopped ({})", m_async->Summary());
	// --skip-guard / --debug-skip-log: the final summary (an atexit handler never runs on this path).
	if (SkipDeps::Enabled()) SkipDeps::DumpOpenRoots("exit");
	m_async->LogSummary();
	if (m_unoptimized_first) {
		const auto& c = m_unoptimized_counters;
		const auto  summary = fmt::format(
		    "unoptimized_created={};served_from_cache={};optimized_done={};swapped={};retired={}",
		    c.unoptimized_created.load(), c.served_from_cache.load(), c.optimized_done.load(),
		    c.swapped.load(), c.retired.load());
		PipelineCacheLog("Unoptimized-first pipelines: {}", summary);
		if (Bench::TimingEnabled()) {
			Bench::LogTiming("summary_unoptimized_first", 0, Bench::NowNs(), 0, false, summary);
		}
	}
	if (Config::GetPrecompileOnLoad() || Config::GetPipelineVariantsReplay()) {
		const auto summary = m_load.Summary();
		PipelineCacheLog("Pipeline load: {}; variants_stored={}", summary,
		                 m_variants != nullptr ? m_variants->Size() : 0);
		if (Bench::TimingEnabled()) {
			Bench::LogTiming("summary_load", 0, Bench::NowNs(), 0, false, summary);
		}
	}
	if (Config::GetPipelineLookahead()) {
		const auto& l = m_lookahead;
		const auto  summary = fmt::format(
		    "cs_seen={};cs_skipped={};cs_requested={};cs_already={};cs_hit_ready={};"
		    "cs_hit_inflight={};cs_miss={};gfx_seen={};gfx_skipped={};gfx_requested={};"
		    "gfx_already={};gfx_hit_ready={};gfx_hit_inflight={};gfx_miss={}",
		    l.cs_seen.load(), l.cs_skipped.load(), l.cs_requested.load(), l.cs_already.load(),
		    l.cs_hit_ready.load(), l.cs_hit_inflight.load(), l.cs_miss.load(), l.gfx_seen.load(),
		    l.gfx_skipped.load(), l.gfx_requested.load(), l.gfx_already.load(),
		    l.gfx_hit_ready.load(), l.gfx_hit_inflight.load(), l.gfx_miss.load());
		std::string skips;
		for (uint32_t i = 0; i < LookaheadCounters::SkipCount; i++) {
			skips += fmt::format(";skip_{}={}", LookaheadCounters::SkipName(i), l.skip[i].load());
		}
		PipelineCacheLog("Pipeline lookahead: {}{}", summary, skips);
		if (SrtPlan::Enabled()) {
			::Log::WriteToConsoleAndLog(SrtPlan::SummaryText());
		}
		if (m_lookahead_thread_summary) {
			PipelineCacheLog("Pipeline lookahead thread: {}", m_lookahead_thread_summary());
		}
		PipelineCacheLog("Pipeline lookahead locks: {}", LookaheadLockSummary());
		PipelineCacheLog("{}", ResolvedDraw::SummaryText());
		if (Bench::TimingEnabled()) {
			Bench::LogTiming("summary_lookahead", 0, Bench::NowNs(), 0, false, summary + skips);
			if (m_lookahead_thread_summary) {
				Bench::LogTiming("summary_lookahead_thread", 0, Bench::NowNs(), 0, false,
				                 m_lookahead_thread_summary());
			}
		}
	}
}

PipelineCache::Pipeline* PipelineCache::FindGraphicsPipeline(const GraphicsPipelineKey& key) {
	// --scalable-read-locks: no lock and no locked instruction on a hit; a miss repeats the lookup under the lock.
	if (m_async_mode != Config::AsyncPipelineMode::Off && Common::ScalableLocksEnabled()) {
		if (const auto* node = m_graphics_index.Find(key)) {
			return node->second.get();
		}
	}
	std::optional<Common::ScalableSharedMutex::SharedGuard> lock;
	if (m_async_mode != Config::AsyncPipelineMode::Off) {
		lock.emplace(m_pipelines_mutex);
	}
	const auto iter = m_graphics_pipelines.find(key);
	return iter != m_graphics_pipelines.end() ? iter->second.get() : nullptr;
}

// GPU thread: FindGraphicsPipeline() behind the --draw-key-cache memo. A memo hit needs a key equal
// to a remembered one (the map's own equality), so it can only return what the map returned for
// that key. --debug-draw-key-verify runs the real lookup on every hit and stops on a disagreement.
PipelineCache::Pipeline* PipelineCache::FindGraphicsPipelineMemo(const GraphicsPipelineKey& key) {
	if (!m_draw_key_cache) {
		return FindGraphicsPipeline(key);
	}
	++m_draw_key_lookups;
	if ((m_draw_key_lookups & 0x3ffffu) == 0) {
		const auto& stats = m_draw_key_memo.GetStats();
		PipelineCacheLog("DrawKeyCache: lookups={} memo_hits={} ({:.1f}%) verified={}",
		                 m_draw_key_lookups, stats.hits,
		                 100.0 * static_cast<double>(stats.hits) /
		                     static_cast<double>(m_draw_key_lookups),
		                 m_draw_key_verified);
	}
	if (auto* hit = m_draw_key_memo.Find(key)) {
		if (m_draw_key_verify) {
			auto* const real = FindGraphicsPipeline(key);
			if (real != *hit) {
				EXIT("DrawKeyCache: memo returned pipeline %p but the pipeline map has %p\n",
				     static_cast<void*>(*hit), static_cast<void*>(real));
			}
			++m_draw_key_verified;
		}
		return *hit;
	}
	auto* const found = FindGraphicsPipeline(key);
	if (found != nullptr) {
		m_draw_key_memo.Remember(key, found);
	}
	return found;
}

PipelineCache::Pipeline* PipelineCache::FindComputePipeline(uint64_t program_id) {
	if (m_async_mode != Config::AsyncPipelineMode::Off && Common::ScalableLocksEnabled()) {
		if (const auto* node = m_compute_index.Find(program_id)) {
			return node->second.get();
		}
	}
	std::optional<Common::ScalableSharedMutex::SharedGuard> lock;
	if (m_async_mode != Config::AsyncPipelineMode::Off) {
		lock.emplace(m_pipelines_mutex);
	}
	const auto iter = m_compute_pipelines.find(program_id);
	return iter != m_compute_pipelines.end() ? iter->second.get() : nullptr;
}

PipelineCache::Pipeline* PipelineCache::PublishGraphicsPipeline(const GraphicsPipelineKey& key,
                                                                std::unique_ptr<Pipeline>& pipeline,
                                                                bool& inserted) {
	std::optional<Common::ScalableSharedMutex::ExclusiveGuard> lock;
	if (m_async_mode != Config::AsyncPipelineMode::Off) {
		lock.emplace(m_pipelines_mutex);
	}
	auto [iter, was_inserted] = m_graphics_pipelines.try_emplace(key, nullptr);
	inserted                  = was_inserted;
	if (was_inserted) {
		iter->second = std::move(pipeline);
		m_graphics_index.Insert(&*iter); // after the node is complete
	}
	return iter->second.get();
}

PipelineCache::Pipeline* PipelineCache::PublishComputePipeline(uint64_t program_id,
                                                               std::unique_ptr<Pipeline>& pipeline,
                                                               bool& inserted) {
	std::optional<Common::ScalableSharedMutex::ExclusiveGuard> lock;
	if (m_async_mode != Config::AsyncPipelineMode::Off) {
		lock.emplace(m_pipelines_mutex);
	}
	auto [iter, was_inserted] = m_compute_pipelines.try_emplace(program_id, nullptr);
	inserted                  = was_inserted;
	if (was_inserted) {
		iter->second = std::move(pipeline);
		m_compute_index.Insert(&*iter);
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
	// Optimized builds that finished after the pipeline's last lookup were never swapped in.
	auto destroy_unclaimed = [this](const auto& pipelines) {
		for (const auto& [key, pipeline]: pipelines) {
			(void)key;
			if (pipeline->optimized != nullptr && pipeline->optimized->pipeline != nullptr) {
				m_graphics.device.destroyPipeline(pipeline->optimized->pipeline, nullptr);
			}
		}
	};
	destroy_unclaimed(m_graphics_pipelines);
	destroy_unclaimed(m_compute_pipelines);
	for (const auto retired: m_retired_pipelines) {
		m_graphics.device.destroyPipeline(retired, nullptr);
	}
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
		// apply: dev, dirty and unknown-revision builds may use (and share) the cache.
		PipelineCacheLog("Vulkan pipeline cache: key=schema (version {})", kPipelineCacheSchemaVersion);
	}
	if (const auto reason = PipelineCacheFile::DisabledReason(
	        DriverCacheKeyMode(), KYTY_BUILD == KYTY_BUILD_RELEASE, KYTY_GIT_HASH,
	        KYTY_GIT_REVISION)) {
		PipelineCacheLog("Vulkan pipeline cache: disabled ({})", *reason);
		return;
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
		if (file_size > 0 && file_size <= std::numeric_limits<uint32_t>::max()) {
			std::vector<uint8_t> contents(file_size);
			uint32_t             read = 0;
			file.Read(contents.data(), static_cast<uint32_t>(contents.size()), &read);
			file.Close();
			const auto signature = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
			const auto loaded =
			    read == contents.size()
			        ? PipelineCacheFile::Load(contents, signature)
			        : PipelineCacheFile::LoadResult {PipelineCacheFile::LoadStatus::TooSmall, {}};
			if (loaded.status == PipelineCacheFile::LoadStatus::Valid) {
				initial_data.assign(loaded.payload.begin(), loaded.payload.end());
			} else {
				PipelineCacheLog("Vulkan pipeline cache: invalidating {} ({})", path,
				                 PipelineCacheFile::ToString(loaded.status));
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
	if (const auto seconds = Config::GetDriverCacheCheckpointSeconds(); seconds != 0) {
		// Pipelines the driver has created so far (the GPU thread and the pipeline workers both
		// count into these): a checkpoint is due only when that number moved.
		const auto created = [this] {
			return uint64_t {m_graphics.pipelines_graphics.load(std::memory_order_relaxed)} +
			       m_graphics.pipelines_compute.load(std::memory_order_relaxed);
		};
		const auto write = [this, created] {
			// t0/t1 are on the clock of --bench-frames-csv, so a bench run can line the write up with
			// the frames it overlapped.
			const auto t0 = Bench::NowNs();
			const bool ok = WriteDriverCache();
			const auto t1 = Bench::NowNs();
			PipelineCacheLog("Vulkan pipeline cache: checkpoint {} in {} ms t0_ns={} t1_ns={} "
			                 "({} pipelines created so far)",
			                 ok ? "written" : "FAILED (retried next period)", (t1 - t0) / 1000000,
			                 t0, t1, created());
			return ok;
		};
		m_driver_cache_checkpoint =
		    std::make_unique<DriverCacheCheckpointer>(std::chrono::seconds(seconds), created, write);
		PipelineCacheLog("Vulkan pipeline cache: checkpoint every {} s while pipelines are created",
		                 seconds);
	}
}

void PipelineCache::Save() {
	// Join the workers and the checkpoint thread first: they use m_driver_cache, and it is
	// destroyed below.
	ShutdownAsync();
	m_driver_cache_checkpoint.reset();
	if (m_driver_cache == nullptr) {
		return;
	}
	if (!WriteDriverCache()) {
		return;
	}
	m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
	m_driver_cache = nullptr;
}

bool PipelineCache::WriteDriverCache() {
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
		return false;
	}
	payload.resize(size);
	const auto contents = PipelineCacheFile::Store(
	    DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties()), payload);
	if (!Common::File::CreateDirectories(m_driver_cache_path.parent_path())) {
		PipelineCacheLog("Vulkan pipeline cache: failed to create cache directory");
		return false;
	}
	auto temp_path = m_driver_cache_path;
	temp_path += ".tmp";
	Common::File file;
	uint32_t     written = 0;
	if (file.Create(temp_path)) {
		file.Write(contents.data(), static_cast<uint32_t>(contents.size()), &written);
	}
	const bool flushed = !file.IsInvalid() && file.Flush();
	file.Close();
	if (written != contents.size() || !flushed ||
	    !Common::File::RenameFile(temp_path, m_driver_cache_path)) {
		PipelineCacheLog("Vulkan pipeline cache: failed to write {}",
		                 Common::PathToString(m_driver_cache_path));
		return false;
	}
	PipelineCacheLog("Vulkan pipeline cache: saved {} bytes to {}", payload.size(),
	                 Common::PathToString(m_driver_cache_path));
	return true;
}

// ---- --async-pipelines: pipeline jobs ----------------------------------------------------------
//
// A pipeline job runs CreatePipelineInternal (descriptor/pipeline layouts + vkCreate*Pipelines on
// the shared driver VkPipelineCache, which is created without EXTERNALLY_SYNCHRONIZED) on a worker
// and publishes the result into the pipeline map. All inputs are copies taken on the GPU thread.

// The optimized build of a pipeline that runs unoptimized (--pipelines-unoptimized-first). Runs on
// the pool's low-priority queue; nobody waits for it. It only fills `build`: the GPU thread swaps
// the result into the Pipeline at its next lookup (PromoteOptimized).
struct PipelineCache::GraphicsOptimizeJob final: AsyncJob {
	GraphicsOptimizeJob(PipelineCache& owner, uint64_t key_hash)
	    : AsyncJob(Kind::Optimize, key_hash), cache(owner) {}

	void Execute() override {
		Stopwatch create_sw;
		CreatePipelineInternal(cache.m_graphics, layouts, key.rendering, key.vertex_input,
		                       vertex_info, ps_input_info ? &*ps_input_info : nullptr, programs,
		                       key.static_params, driver_cache, PipelineBuild::Optimize);
		if (create_sw.On()) {
			AddTimingRow("pipeline_optimize_gfx", KeyHash(), create_sw.Start(), create_sw.Elapsed(),
			             fmt::format("vs_id={};ps_id={};driver_ns={}", vs_id, ps_id,
			                         Bench::TakeDriverCreateNs()));
		}
		build->pipeline = layouts.pipeline;
		build->done.store(true, std::memory_order_release);
		cache.m_unoptimized_counters.optimized_done.fetch_add(1, std::memory_order_relaxed);
	}

	PipelineCache&                      cache;
	GraphicsPipelineKey                 key;
	std::vector<ShaderVertexInputInfo>  vertex_info;
	std::optional<ShaderPixelInputInfo> ps_input_info;
	GraphicsPrograms                    programs;
	vk::PipelineCache                   driver_cache = nullptr;
	Pipeline                            layouts; // the unoptimized pipeline's layouts; .pipeline is the result
	std::shared_ptr<OptimizedBuild>     build;
	uint64_t                            vs_id = 0;
	uint64_t                            ps_id = 0;
};

struct PipelineCache::GraphicsPipelineJob final: AsyncJob {
	GraphicsPipelineJob(PipelineCache& owner, GraphicsPipelineKey job_key, uint64_t key_hash)
	    : AsyncJob(Kind::Graphics, key_hash), cache(owner), key(std::move(job_key)) {}

	void Execute() override {
		if (cache.FindGraphicsPipeline(key) != nullptr) {
			// Published while this (prefetch) request was being registered: nothing to build.
			cache.m_graphics_jobs.Erase(key);
			return;
		}
		auto cached = std::make_unique<Pipeline>();
		cached->prefetched = IsPrefetched();
		cached->loaded     = from_load;
		LogPipelineTrace("CreatePipelineInternal begin", vs_id, ps_id);
		Stopwatch create_sw;
		CreatePipelineInternal(cache.m_graphics, *cached, key.rendering, key.vertex_input,
		                       vertex_info, ps_input_info ? &*ps_input_info : nullptr, programs,
		                       key.static_params, driver_cache, cache.FirstBuild());
		if (create_sw.On()) {
			const auto create_ns = create_sw.Elapsed();
			AddTimingRow("pipeline_create_gfx", KeyHash(), create_sw.Start(), create_ns,
			             fmt::format("vs_id={};ps_id={};tess={};driver_cache={};driver_ns={};unopt={}",
			                         vs_id, ps_id, programs.VertexStageCount() == 3 ? 1 : 0,
			                         driver_cache != nullptr ? 1 : 0, Bench::TakeDriverCreateNs(),
			                         cached->unoptimized ? 1 : 0));
		}
		LogPipelineTrace("CreatePipelineInternal done", vs_id, ps_id);
		EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
		EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);
		if (from_load) {
			cached->load_ready_ns = Bench::NowNs();
		}
		cache.NoteFirstBuild(*cached);
		// An unoptimized pipeline gets its optimized build queued once it is published. The
		// follow-up copies everything it needs now: after the publish only the GPU thread may touch
		// the Pipeline.
		std::shared_ptr<GraphicsOptimizeJob> optimize;
		if (cached->unoptimized) {
			cached->optimized = std::make_shared<OptimizedBuild>();
			optimize          = std::make_shared<GraphicsOptimizeJob>(cache, KeyHash());
			optimize->key           = key;
			optimize->vertex_info   = vertex_info;
			optimize->ps_input_info = ps_input_info;
			optimize->programs      = programs;
			optimize->driver_cache  = driver_cache;
			optimize->build         = cached->optimized;
			optimize->layouts.pipeline_layout       = cached->pipeline_layout;
			optimize->layouts.descriptor_set_layout = cached->descriptor_set_layout;
			optimize->layouts.uses_push_descriptors = cached->uses_push_descriptors;
			optimize->vs_id = vs_id;
			optimize->ps_id = ps_id;
		}
		bool inserted = false;
		cache.PublishGraphicsPipeline(key, cached, inserted);
		if (!inserted) {
			cache.DestroyPipeline(*cached); // an equal pipeline was published first
			optimize.reset();
		}
		// Publish first, unregister last: "not registered" must imply "published".
		cache.m_graphics_jobs.Erase(key);
		if (optimize != nullptr) {
			if (auto* pool = cache.AsyncPool()) {
				pool->SubmitLowPriority(std::move(optimize));
			}
		}
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
	bool                               from_load    = false; // queued by a shader-load job
};

struct PipelineCache::ComputeOptimizeJob final: AsyncJob {
	ComputeOptimizeJob(PipelineCache& owner, uint64_t id)
	    : AsyncJob(Kind::Optimize, id), cache(owner), program_id(id) {}

	void Execute() override {
		Stopwatch create_sw;
		CreatePipelineInternal(cache.m_graphics, layouts, input_info, module, driver_cache,
		                       PipelineBuild::Optimize);
		if (create_sw.On()) {
			AddTimingRow("pipeline_optimize_cs", program_id, create_sw.Start(), create_sw.Elapsed(),
			             fmt::format("cs_id={};driver_ns={}", program_id, Bench::TakeDriverCreateNs()));
		}
		build->pipeline = layouts.pipeline;
		build->done.store(true, std::memory_order_release);
		cache.m_unoptimized_counters.optimized_done.fetch_add(1, std::memory_order_relaxed);
	}

	PipelineCache&                  cache;
	uint64_t                        program_id;
	ShaderComputeInputInfo          input_info;
	vk::ShaderModule                module       = nullptr;
	vk::PipelineCache               driver_cache = nullptr;
	Pipeline                        layouts;
	std::shared_ptr<OptimizedBuild> build;
};

struct PipelineCache::ComputePipelineJob final: AsyncJob {
	ComputePipelineJob(PipelineCache& owner, uint64_t id)
	    : AsyncJob(Kind::Compute, id), cache(owner), program_id(id) {}

	void Execute() override {
		if (cache.FindComputePipeline(program_id) != nullptr) {
			cache.m_compute_jobs.Erase(program_id); // see GraphicsPipelineJob::Execute
			return;
		}
		auto      cached = std::make_unique<Pipeline>();
		cached->prefetched = IsPrefetched();
		cached->loaded     = from_load;
		Stopwatch create_sw;
		CreatePipelineInternal(cache.m_graphics, *cached, input_info, module, driver_cache,
		                       cache.FirstBuild());
		if (from_load) {
			cached->load_ready_ns = Bench::NowNs();
		}
		if (create_sw.On()) {
			const auto create_ns = create_sw.Elapsed();
			AddTimingRow("pipeline_create_cs", program_id, create_sw.Start(), create_ns,
			             fmt::format("cs_id={};driver_cache={};driver_ns={};unopt={}", program_id,
			                         driver_cache != nullptr ? 1 : 0, Bench::TakeDriverCreateNs(),
			                         cached->unoptimized ? 1 : 0));
		}
		EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
		EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);
		cache.NoteFirstBuild(*cached);
		std::shared_ptr<ComputeOptimizeJob> optimize;
		if (cached->unoptimized) {
			cached->optimized = std::make_shared<OptimizedBuild>();
			optimize          = std::make_shared<ComputeOptimizeJob>(cache, program_id);
			optimize->input_info   = input_info;
			optimize->module       = module;
			optimize->driver_cache = driver_cache;
			optimize->build        = cached->optimized;
			optimize->layouts.pipeline_layout       = cached->pipeline_layout;
			optimize->layouts.descriptor_set_layout = cached->descriptor_set_layout;
			optimize->layouts.uses_push_descriptors = cached->uses_push_descriptors;
		}
		bool inserted = false;
		cache.PublishComputePipeline(program_id, cached, inserted);
		if (!inserted) {
			cache.DestroyPipeline(*cached);
			optimize.reset();
		}
		cache.m_compute_jobs.Erase(program_id);
		if (optimize != nullptr) {
			if (auto* pool = cache.AsyncPool()) {
				pool->SubmitLowPriority(std::move(optimize));
			}
		}
	}

	void Cancel() override { cache.m_compute_jobs.Erase(program_id); }

	PipelineCache&         cache;
	uint64_t               program_id;
	ShaderComputeInputInfo input_info;
	vk::ShaderModule       module       = nullptr;
	vk::PipelineCache      driver_cache = nullptr;
	bool                   from_load    = false; // queued by a shader-load job
};

// ---- shader-load jobs: see ProgramCache::LoadJob --------------------------------------------

void PipelineCache::ProgramCache::LoadJob::Unregister() {
	owner.m_program_cache->RemoveLoad(this);
}


void PipelineCache::ProgramCache::LoadJob::Execute() {
	ProgramCache& cache    = *owner.m_program_cache;
	LoadCounters& counters = owner.m_load;
	const bool    timing   = Bench::TimingEnabled();
	Stopwatch     sw;
	// Translation reads only the number of user SGPRs; their values matter to materialization,
	// which a load job does not do.
	const std::vector<uint32_t> user_data(user_data_count, 0u);
	ShaderRecompiler::CompileOptions options;
	options.stage              = ShaderType::Compute;
	options.shader_hash        = key.hash;
	options.user_data          = user_data;
	options.dump_ir            = Config::GetShaderLogDirection() != Config::LogDirection::Silent;
	options.early_dump         = options.dump_ir;
	options.dump_label         = "ShaderRecompiler CS";
	options.wave_size          = info.wave_size;
	options.input_info.compute = &info;

	auto source = ShaderRecompiler::PrepareShaderSource(code, options);
	if (source.call) {
		counters.skipped_call.fetch_add(1, std::memory_order_relaxed);
		Unregister();
		return;
	}
	auto translated = ShaderRecompiler::TranslateProgram(source.decoded, options);
	const int64_t frontend_ns = sw.Lap();
	auto plan = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
	ShaderRecompiler::IR::ResourceSpecialization wanted;
	bool usable = true;
	if (explicit_spec) {
		wanted = *explicit_spec;
		usable = SpecializationFits(plan, wanted);
		if (!usable) counters.skipped_stale.fetch_add(1, std::memory_order_relaxed);
	} else if (!ShaderRecompiler::IR::GuessSpecialization(plan, wanted)) {
		usable = false;
		counters.skipped_unguessable.fetch_add(1, std::memory_order_relaxed);
	}
	// The translated source is worth keeping even when no variant can be built: the first dispatch
	// then finds a known program (first insert wins).
	auto* entry = cache.InsertEntry(key, std::move(plan), nullptr);
	if (!usable) {
		Unregister();
		return;
	}
	{
		std::lock_guard lock(spec_mutex);
		spec = wanted;
	}
	if (entry->FindPublished(0, wanted) != nullptr) {
		// A dispatch or the lookahead got there first; its own pipeline job exists or will.
		counters.skipped_known.fetch_add(1, std::memory_order_relaxed);
		Unregister();
		return;
	}
	int64_t backend_ns = 0, module_ns = 0;
	auto    compiled = cache.CompilePermutation("cs", options, std::move(translated), wanted, 0, {},
	                                            timing ? &backend_ns : nullptr,
	                                            timing ? &module_ns : nullptr);
	compiled.from_load         = true;
	compiled.load_register_ns  = register_ns;
	compiled.load_ready_ns     = Bench::NowNs();
	bool dup                   = false;
	auto* const published      = entry->Publish(compiled, 0, false, dup);
	if (dup) {
		cache.device.destroyShaderModule(compiled.handle.module, nullptr);
		counters.duplicates.fetch_add(1, std::memory_order_relaxed);
	} else {
		cache.stage_permutation_counts[static_cast<size_t>(ShaderType::Compute)].fetch_add(
		    1, std::memory_order_relaxed);
		counters.built.fetch_add(1, std::memory_order_relaxed);
	}
	if (timing) {
		// The variant this job built, in the compute_variant row's format, so the guess can be
		// compared with what the dispatch used.
		Variants::ComputeVariant built_variant;
		built_variant.code_hash        = key.hash;
		built_variant.code_size_dwords = key.code_size;
		built_variant.state            = ToVariantStatic(info, user_data_count);
		built_variant.spec             = ToVariantSpec(wanted);
		AddTimingRow("load_program", key.hash, sw.Start(), sw.Elapsed(),
		             fmt::format("shader_id={};dup={};guess={};frontend_ns={};backend_ns={};"
		                         "module_ns={};register_ns={};code_dwords={};{}",
		                         published->handle.id, dup ? 1 : 0, guess ? 1 : 0, frontend_ns,
		                         backend_ns, module_ns, register_ns, code.size(),
		                         DescribeVariant(built_variant)));
	}
	owner.SubmitLoadedComputePipeline(published->handle, info, &published->program, priority);
	// Publish first, unregister last: a dispatch that no longer finds the job finds the program.
	Unregister();
}

// Worker: queues the pipeline of a program a load job published, on the low-priority queue.
void PipelineCache::SubmitLoadedComputePipeline(const ShaderProgram& program,
                                                ShaderComputeInputInfo info,
                                                const ShaderRecompiler::IR::CompiledShaderInfo* compiled,
                                                uint32_t priority) {
	auto* pool = AsyncPool();
	if (pool == nullptr) {
		return;
	}
	info.stage       = {.program = compiled, .resources = &PrefetchResources()};
	info.lds_storage = info.lds_size_dwords * 4u >
	    m_graphics.GetPhysicalDeviceProperties().limits.maxComputeSharedMemorySize;
	if (FindComputePipeline(program.id) != nullptr || m_compute_jobs.Find(program.id) != nullptr) {
		return;
	}
	auto fresh = std::make_shared<ComputePipelineJob>(*this, program.id);
	fresh->input_info      = info;
	fresh->module          = program.module;
	fresh->driver_cache    = m_driver_cache;
	fresh->from_load       = true;
	bool inserted          = false;
	m_compute_jobs.Insert(program.id, fresh, inserted);
	if (!inserted) {
		return;
	}
	if (!pool->SubmitLowPriority(fresh, true, priority)) {
		m_compute_jobs.Erase(program.id);
		return;
	}
	m_load.pipelines.fetch_add(1, std::memory_order_relaxed);
}

void PipelineCache::NoteShaderRegistered(const ShaderRegistration& reg) {
	if (m_async_mode == Config::AsyncPipelineMode::Off ||
	    (!Config::GetPrecompileOnLoad() && m_variants == nullptr)) {
		return;
	}
	const bool compute = static_cast<Prospero::ShaderBinaryType>(reg.type) == Prospero::ShaderBinaryType::kCs;
	if (!compute) {
		m_load.registered_other.fetch_add(1, std::memory_order_relaxed);
		if (m_gfx_by_hash.empty()) {
			return;
		}
	}
	std::shared_lock gate(m_prefetch_gate);
	if (m_prefetch_stopped) {
		return;
	}
	auto* pool = AsyncPool();
	if (pool == nullptr) {
		return;
	}
	if (!compute) {
		CaptureGraphicsRegistration(reg, *pool);
		return;
	}
	m_load.registered_cs.fetch_add(1, std::memory_order_relaxed);
	const auto register_ns = Bench::NowNs();

	// The stage registers the header carries, decoded like the PM4 stream would.
	HW::ComputeShaderInfo regs {};
	bool                  has_rsrc2 = false;
	for (const auto& entry: reg.sh_registers) {
		if (HW::ApplyComputeShaderRegister(regs.cs_regs, entry.offset, entry.value) ==
		    HW::CsRegisterResult::Applied) {
			has_rsrc2 = has_rsrc2 || entry.offset == Pm4::COMPUTE_PGM_RSRC2;
		}
	}
	if (!has_rsrc2 || regs.cs_regs.data_addr != reg.address || reg.code_size_bytes < 4u) {
		m_load.skipped_registers.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	const uint32_t code_dwords = reg.code_size_bytes / 4u;
	std::vector<uint32_t> code(code_dwords);
	std::memcpy(code.data(), reinterpret_cast<const void*>(reg.address), code_dwords * 4u);

	const uint32_t host_subgroup = m_graphics.SupportsComputeWave64() ? 64u : 32u;
	const auto submit = [&](ShaderComputeInputInfo info, uint32_t user_data_count,
	                        std::optional<ShaderRecompiler::IR::ResourceSpecialization> spec,
	                        uint32_t priority) {
		info.host_subgroup_size = host_subgroup;
		ProgramCache::ProgramKey key;
		key.stage           = ShaderType::Compute;
		key.hash            = reg.hash;
		key.user_data_count = user_data_count;
		key.code_size       = code_dwords;
		BuildStageStaticKey(info, key.static_state);
		auto job = std::make_shared<ProgramCache::LoadJob>(
		    *this, key, static_cast<uint64_t>(ProgramCache::ProgramKeyHash {}(key)));
		job->info            = info;
		job->code            = code;
		job->user_data_count = user_data_count;
		job->register_ns     = register_ns;
		job->priority        = priority;
		job->guess           = !spec.has_value();
		job->explicit_spec   = std::move(spec);
		if (!m_program_cache->AddLoad(job)) {
			return;
		}
		(job->guess ? m_load.jobs_guess : m_load.jobs_stored).fetch_add(1, std::memory_order_relaxed);
		if (job->guess) {
			// Not a pool job: nothing can wait for it, and its translation is speculative.
			job->joinable = false;
			if (m_guess_thread == nullptr) {
				m_program_cache->RemoveLoad(job.get());
			} else {
				m_guess_thread->Push(job);
			}
		} else if (!pool->SubmitLowPriority(job, true, priority)) {
			m_program_cache->RemoveLoad(job.get());
		}
	};

	if (m_variants != nullptr) {
		const auto stored = m_variants->Find(reg.hash, code_dwords);
		m_load.stored_variants.fetch_add(stored.size(), std::memory_order_relaxed);
		for (const auto& variant: stored) {
			ShaderComputeInputInfo info {};
			ApplyVariantStatic(variant.state, info);
			submit(info, variant.state.user_data_count, FromVariantSpec(variant.spec), variant.first_use_ms);
		}
		if (!stored.empty()) {
			return;
		}
	}
	if (!Config::GetPrecompileOnLoad()) {
		return;
	}
	// No history: the variant the header and the dispatch modifier imply. The modifier carries the
	// DISPATCH_INITIATOR bits the guest library ORs into every dispatch of this shader (wave size,
	// thread-dimension mode); the queue (async compute) is only known at the dispatch.
	regs.cs_regs.wave_size = Pm4::ComputeWaveSize(reg.dispatch_modifier);
	ShaderComputeInputInfo info {};
	info.async_compute              = false;
	info.dispatch_thread_dimensions = (reg.dispatch_modifier & Pm4::COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0;
	info.host_subgroup_size         = host_subgroup;
	const auto params               = PrepareProgram(regs, HW::ShaderRegisters {}, info);
	if (params.hash != reg.hash) {
		m_load.skipped_registers.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	submit(info, params.user_data_count, std::nullopt, Variants::ComputeVariant::kUnknownFirstUse);
}

// The guest registered a non-compute shader. When a stored graphics pipeline needs its binary the
// code is copied; the pipeline's job starts once all of its binaries have been seen.
void PipelineCache::CaptureGraphicsRegistration(const ShaderRegistration& reg, AsyncPipelineCompiler& pool) {
	const auto listed = m_gfx_by_hash.find(reg.hash);
	if (listed == m_gfx_by_hash.end() || reg.code_size_bytes < 4u) {
		return;
	}
	const uint32_t code_dwords = reg.code_size_bytes / 4u;
	auto code = std::make_shared<std::vector<uint32_t>>(code_dwords);
	std::memcpy(code->data(), reinterpret_cast<const void*>(reg.address), code_dwords * 4u);
	std::vector<std::shared_ptr<ProgramCache::GfxLoadJob>> ready;
	{
		std::lock_guard lock(m_gfx_mutex);
		for (const size_t index: listed->second) {
			auto& pending = m_gfx_pending[index];
			if (pending.submitted) continue;
			if (pending.variant.vs.RegistrationHash() == reg.hash &&
			    pending.variant.vs.code_size_dwords == code_dwords) {
				pending.vs_code = code;
			}
			if (pending.variant.has_ps && pending.variant.ps.code_hash == reg.hash &&
			    pending.variant.ps.code_size_dwords == code_dwords) {
				pending.ps_code = code;
			}
			if (pending.variant.vs.back_hash == reg.hash &&
			    pending.variant.vs.back_size_dwords == code_dwords) {
				pending.vs_back_code = code;
			}
			if (pending.vs_code == nullptr || (pending.variant.has_ps && pending.ps_code == nullptr) ||
			    (pending.variant.vs.back_hash != 0 && pending.vs_back_code == nullptr)) {
				continue;
			}
			pending.submitted = true;
			auto job = std::make_shared<ProgramCache::GfxLoadJob>(*this, index);
			job->variant  = pending.variant;
			job->vs_code  = pending.vs_code;
			job->ps_code  = pending.ps_code;
			job->vs_back_code = pending.vs_back_code;
			job->priority = pending.variant.first_use_ms;
			ready.push_back(std::move(job));
		}
	}
	for (auto& job: ready) {
		m_gfx_load_active.fetch_add(1, std::memory_order_relaxed);
		m_load.gfx_jobs.fetch_add(1, std::memory_order_relaxed);
		const auto priority = job->priority;
		if (!pool.SubmitLowPriority(job, true, priority)) {
			// Cancelled by a shutdown; the destructor releases the active count.
		}
	}
}

PipelineCache::ProgramCache::GfxLoadJob::~GfxLoadJob() {
	owner.m_gfx_load_active.fetch_sub(1, std::memory_order_relaxed);
}

void PipelineCache::ProgramCache::GfxLoadJob::Execute() {
	ProgramCache& cache    = *owner.m_program_cache;
	LoadCounters& counters = owner.m_load;
	const auto    skip     = [&](const char* reason) {
		counters.gfx_skipped.fetch_add(1, std::memory_order_relaxed);
		if (Bench::TimingEnabled()) {
			Bench::LogTiming("graphics_load_skip", variant.vs.code_hash, Bench::NowNs(), 0, false,
			                 fmt::format("reason={};vs_stage={};ps_hash={:016x};back={}", reason,
			                             variant.vs.stage, variant.ps.code_hash,
			                             variant.vs.back_hash != 0 ? 1 : 0));
		}
	};
	ShaderVertexInputInfo vertex;
	ShaderPixelInputInfo  pixel;
	if (!DecodeVertexInfo(variant.vs.input, variant.vs.input_layout, vertex) ||
	    (variant.has_ps && !DecodePixelInfo(variant.ps.input, variant.ps.input_layout, pixel))) {
		skip("input_decode");
		return;
	}
	uint32_t   built = 0;
	Permutation* ps  = nullptr;
	if (variant.has_ps) {
		ps = cache.BuildStoredStage(variant.ps, *ps_code, {}, nullptr, &pixel, counters, built);
		if (ps == nullptr) {
			skip("ps_stage");
			return;
		}
	}
	Permutation* vs = cache.BuildStoredStage(
	    variant.vs, *vs_code, vs_back_code != nullptr ? std::span<const uint32_t>(*vs_back_code) : std::span<const uint32_t> {},
	    &vertex, nullptr, counters, built);
	if (vs == nullptr) {
		skip("vs_stage");
		return;
	}
	GraphicsPipelineKey key {};
	if (!DecodePipelineKey(variant.key, key.rendering, key.vertex_input, key.static_params)) {
		skip("key_decode");
		return;
	}
	key.vertex_shader_ids[0] = vs->handle.id;
	key.ps_shader_id         = variant.has_ps ? ps->handle.id : 0;
	if (owner.FindGraphicsPipeline(key) != nullptr || owner.m_graphics_jobs.Find(key) != nullptr) {
		skip("known");
		return;
	}
	auto* pool = owner.AsyncPool();
	if (pool == nullptr) {
		return;
	}
	vertex.stage = {.program = &vs->program, .resources = &PrefetchResources()};
	if (variant.has_ps) pixel.stage = {.program = &ps->program, .resources = &PrefetchResources()};
	auto fresh = std::make_shared<GraphicsPipelineJob>(owner, key, GraphicsPipelineKeyHash {}(key));
	fresh->vertex_info.assign(1, vertex);
	if (variant.has_ps) fresh->ps_input_info = pixel;
	fresh->programs.vertex[0] = vs->handle;
	if (variant.has_ps) fresh->programs.pixel = ps->handle;
	fresh->driver_cache = owner.m_driver_cache;
	fresh->vs_id        = vs->handle.id;
	fresh->ps_id        = variant.has_ps ? ps->handle.id : 0;
	fresh->from_load    = true;
	bool inserted       = false;
	owner.m_graphics_jobs.Insert(key, fresh, inserted);
	if (!inserted) {
		skip("duplicate_job");
		return;
	}
	if (!pool->SubmitLowPriority(fresh, true, priority)) {
		owner.m_graphics_jobs.Erase(key);
		return;
	}
	counters.gfx_pipelines.fetch_add(1, std::memory_order_relaxed);
}

bool PipelineCache::WaitLoadIdle(int timeout_ms) {
	const auto deadline = Bench::NowNs() + static_cast<int64_t>(timeout_ms) * 1000000;
	while (m_program_cache->LoadActive() != 0 || m_compute_jobs.Size() != 0 ||
	       m_graphics_jobs.Size() != 0 || m_gfx_load_active.load() != 0) {
		if (Bench::NowNs() > deadline) {
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return true;
}

PipelineCache::GraphicsPrograms PipelineCache::GetGraphicsPrograms(
    const HW::VertexShaderInfo& vertex_regs, const HW::PixelShaderInfo& pixel_regs,
    const HW::ShaderRegisters& sh, const HW::Context& context, const HW::UserConfig& user_config,
    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping, bool pixel_active,
    std::array<ShaderVertexInputInfo, 3>& vertex_info, ShaderPixelInputInfo& pixel_info) {
	Common::GpuPhase::Scope gpu_phase_pipeline(Common::GpuPhase::Phase::Pipeline);
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
			m_lookahead.skip[LookaheadCounters::SkipPrepareFailed].fetch_add(
			    1, std::memory_order_relaxed);
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
		// Everything the pixel stage's static info is made of: its registers, the context and user config (blend state, export mapping,
		// shader mask, primitive type) and whether the vertex side is a mesh or tessellation. --draw-state-memo keeps the last result
		// per those inputs; only the user SGPRs are copied fresh. The workers (prefetch) always compute.
		const auto prepare_pixel = [&]() -> bool {
			pixel_params      = PrepareProgram(pixel_regs, sh, target_export_mapping, pixel_info);
			if (prefetch && Common::Speculation::TakeFailure()) {
				m_lookahead.skip[LookaheadCounters::SkipPrepareFailed].fetch_add(
				    1, std::memory_order_relaxed);
				return {};
			}
			if (!mesh_active && !tess_active && Prospero::IsRectList(user_config.GetPrimType())) {
				pixel_info.parameter_mode = ShaderPixelParameterMode::Rectangle;
			} else if (context.GetModeControl().provoking_vtx_last) {
				pixel_info.parameter_mode = ShaderPixelParameterMode::LastVertex;
			} else {
				pixel_info.parameter_mode = ShaderPixelParameterMode::FirstVertex;
			}
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
			                       [](uint8_t mode) { return mode == 0; })) {
				switch (ClassifyBlendMapping(blend, pixel_info.target_export_mapping[0])) {
					case BlendMappingSupport::SourceAlpha:
						pixel_info.alpha_blend_source = ShaderAlphaBlendSource::SourceAlpha;
						break;
					case BlendMappingSupport::SourceAlphaOne:
						pixel_info.alpha_blend_source = ShaderAlphaBlendSource::SourceAlphaOne;
						break;
					case BlendMappingSupport::SourceAlphaZero:
						pixel_info.alpha_blend_source = ShaderAlphaBlendSource::SourceAlphaZero;
						break;
					default: break;
				}
				if (pixel_info.alpha_blend_source != ShaderAlphaBlendSource::None) {
					pixel_info.dual_source_blending     = true;
					pixel_info.target_output_mode[1]    = pixel_info.target_output_mode[0];
					pixel_info.target_export_mapping[1] = {};
				}
			}
	
			return true;
		};
		struct PixelMemo {
			bool                  valid = false;
			uint64_t              ctx_epoch = 0, uc_epoch = 0;
			HW::PsStageRegisters  regs {};
			bool                  mesh_active = false, tess_active = false;
			ShaderParams          params;
			ShaderPixelInputInfo  info;
		};
		static thread_local PixelMemo memo;
		const bool use_memo = !prefetch && Config::DrawStateMemoEnabled();
		const bool hit = use_memo && memo.valid && memo.ctx_epoch == context.Epoch() && memo.uc_epoch == user_config.Epoch() &&
		                 memo.mesh_active == mesh_active && memo.tess_active == tess_active &&
		                 std::memcmp(&memo.regs, &pixel_regs.ps_regs, sizeof(memo.regs)) == 0;
		if (hit) {
			pixel_params = memo.params;
			pixel_info   = memo.info;
			// The user SGPRs are the only input of the params that the key leaves out.
			const auto count = static_cast<size_t>(pixel_regs.ps_regs.rsrc2.user_sgpr);
			std::copy_n(pixel_regs.ps_user_sgpr.value, count, pixel_params.user_data.begin());
			if (Config::ResolvedDrawVerify()) {
				const auto remembered_params = pixel_params;
				const auto remembered_info   = pixel_info;
				(void)prepare_pixel();
				std::vector<uint32_t> key_fresh, key_remembered;
				BuildStageStaticKey(pixel_info, key_fresh);
				BuildStageStaticKey(remembered_info, key_remembered);
				if (pixel_params.hash != remembered_params.hash || pixel_params.code.data() != remembered_params.code.data() ||
				    pixel_params.code.size() != remembered_params.code.size() ||
				    pixel_params.user_data_count != remembered_params.user_data_count ||
				    pixel_params.user_data != remembered_params.user_data || key_fresh != key_remembered ||
				    pixel_info.parameter_mode != remembered_info.parameter_mode ||
				    pixel_info.dual_source_blending != remembered_info.dual_source_blending ||
				    pixel_info.alpha_blend_source != remembered_info.alpha_blend_source) {
					EXIT("draw state memo: the remembered pixel program preparation differs from a fresh one\n");
				}
				static std::atomic<uint64_t> verified {0};
				if ((verified.fetch_add(1, std::memory_order_relaxed) + 1) % (1u << 20) == 0) {
					std::printf("DrawStateMemo verify: %" PRIu64 " remembered pixel preparations agree with a fresh one\n",
					            verified.load(std::memory_order_relaxed));
					std::fflush(stdout);
				}
			}
		} else {
			if (!prepare_pixel()) {
				return {};
			}
			if (use_memo) {
				memo.valid       = true;
				memo.ctx_epoch   = context.Epoch();
				memo.uc_epoch    = user_config.Epoch();
				memo.regs        = pixel_regs.ps_regs;
				memo.mesh_active = mesh_active;
				memo.tess_active = tess_active;
				memo.params      = pixel_params;
				memo.info        = pixel_info;
			}
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
	// A draw whose shaders write memory later frames read (buffer/image stores, image atomics, GDS)
	// is never skipped: it waits for its pipeline like in stall mode (DrawSkipSafety.h).
	bool allow_skip = !prefetch && m_async_mode == Config::AsyncPipelineMode::Skip;
	if (allow_skip) {
		const auto stage_may_skip = [&](const ShaderParams& params, ShaderType stage) {
			bool newly_unsafe = false;
			const bool safe = m_skip_safety.MayBeSkipped(params.code, params.hash, params.back_code,
			                                             stage == ShaderType::Local, &newly_unsafe);
			if (newly_unsafe) {
				PipelineCacheLog("Async pipelines: shader 0x{:016x} writes memory, its draws are never skipped",
				                 params.hash);
			}
			return safe;
		};
		allow_skip = (!pixel_active || stage_may_skip(pixel_params, ShaderType::Pixel));
		for (uint32_t i = 0; allow_skip && i < (tess_active ? 3u : 1u); i++) {
			allow_skip = stage_may_skip(vertex_params[i], vertex_info[i].logical_stage);
		}
	}
	result.may_skip = !prefetch && allow_skip;
	result.safety_kept = !prefetch && m_async_mode == Config::AsyncPipelineMode::Skip && !allow_skip;
	result.vs_hash     = vertex_params[0].hash;
	result.ps_hash     = pixel_active ? pixel_params.hash : 0;
	ProgramCache::AsyncContext async {
	    .pool       = AsyncPool(),
	    .allow_skip = allow_skip,
	    .prefetch   = prefetch,
	    .counters   = &m_lookahead};
	if (pixel_active) {
		result.pixel = m_program_cache->Get(pixel_params, pixel_info, push_data_cursor, &async);
		if (async.pending) {
			result.pending = true;
			result.program_waited = async.waited;
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
	result.program_waited = async.waited;
	result.guard_kept     = async.guard_waited;
	return result;
}

ShaderProgram PipelineCache::GetComputeProgram(const HW::ComputeShaderInfo& regs,
                                               const HW::ShaderRegisters&   sh,
                                               ShaderComputeInputInfo&      input_info) {
	Common::GpuPhase::Scope gpu_phase_pipeline(Common::GpuPhase::Phase::Pipeline);
	input_info.host_subgroup_size = m_graphics.SupportsComputeWave64() ? 64u : 32u;
	const auto        params      = PrepareProgram(regs, sh, input_info);
	input_info.lds_storage = input_info.lds_size_dwords * 4u >
	    m_graphics.GetPhysicalDeviceProperties().limits.maxComputeSharedMemorySize;
	uint32_t          push_data_cursor = 0;
	// Compute dispatches are never skipped: the dispatch waits for the worker (stall and skip).
	ProgramCache::AsyncContext async {.pool = AsyncPool(), .allow_skip = false};
	return m_program_cache->Get(params, input_info, push_data_cursor, &async);
}

// GPU thread: a draw/dispatch is about to use `pipeline`. The first use of a pipeline that a
// prefetch job built is a prediction hit ("in flight" when the draw had to wait for the job).
void PipelineCache::NoteDrawUse(Pipeline& pipeline, bool graphics, bool joined_in_flight) {
	if (pipeline.loaded && !pipeline.load_claimed) {
		pipeline.load_claimed = true;
		(graphics ? (joined_in_flight ? m_load.gfx_hit_inflight : m_load.gfx_hit_ready)
		          : (joined_in_flight ? m_load.pipeline_hit_inflight : m_load.pipeline_hit_ready))
		    .fetch_add(1, std::memory_order_relaxed);
		if (Bench::TimingEnabled()) {
			const auto now = Bench::NowNs();
			Bench::LogTiming("load_pipeline_hit", 0, now, 0, true,
			                 fmt::format("gfx={};waited={};ready_ns={};idle_ns={}", graphics ? 1 : 0, joined_in_flight ? 1 : 0,
			                             pipeline.load_ready_ns,
			                             joined_in_flight ? 0 : now - pipeline.load_ready_ns));
		}
	}
	if (!pipeline.prefetched || pipeline.claimed) {
		return;
	}
	pipeline.claimed = true;
	auto& counter    = graphics ? (joined_in_flight ? m_lookahead.gfx_hit_inflight
	                                                 : m_lookahead.gfx_hit_ready)
	                            : (joined_in_flight ? m_lookahead.cs_hit_inflight
	                                                 : m_lookahead.cs_hit_ready);
	counter.fetch_add(1, std::memory_order_relaxed);
}

void PipelineCache::PrefetchCompute(const HW::ComputeShaderInfo& regs,
                                    const HW::ShaderRegisters& sh, uint32_t mode,
                                    bool async_compute, const uint32_t (&groups)[3]) {
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
	input_info.async_compute              = async_compute;
	input_info.dispatch_thread_dimensions = (mode & (1u << 5u)) != 0;
	std::copy(std::begin(groups), std::end(groups), input_info.workgroup_counts);
	if (input_info.dispatch_thread_dimensions) {
		// Same conversion as RenderExecutor::DispatchDirect.
		const uint32_t group_sizes[] = {regs.cs_regs.num_thread_x, regs.cs_regs.num_thread_y,
		                                regs.cs_regs.num_thread_z};
		for (uint32_t axis = 0; axis < 3u; ++axis) {
			const auto size    = std::max(group_sizes[axis], 1u);
			const auto threads = input_info.workgroup_counts[axis];
			input_info.workgroup_counts[axis] = threads / size + (threads % size != 0u);
		}
	}
	const auto params                     = PrepareProgram(regs, sh, input_info);
	if (Common::Speculation::TakeFailure()) {
		m_lookahead.cs_skipped.fetch_add(1, std::memory_order_relaxed);
		m_lookahead.skip[LookaheadCounters::SkipPrepareFailed].fetch_add(
		    1, std::memory_order_relaxed);
		return;
	}
	input_info.lds_storage = input_info.lds_size_dwords * 4u >
	    m_graphics.GetPhysicalDeviceProperties().limits.maxComputeSharedMemorySize;
	uint32_t   push_data_cursor           = 0;
	ProgramCache::AsyncContext async {
	    .pool = pool, .allow_skip = false, .prefetch = true, .counters = &m_lookahead};
	const auto program = m_program_cache->Get(params, input_info, push_data_cursor, &async);
	if (!program) {
		m_lookahead.cs_skipped.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	if ((Config::GetPipelineLookaheadKinds() & 4u) != 0) {
		return; // --pipeline-lookahead-kinds *-programs: debug, programs only
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
	// A registered job is always submitted: the GPU thread may already have found it in the
	// registry and be waiting on it, so it must not be retracted without finishing. If the pipeline
	// got published meanwhile the job notices at the start of Execute() and does nothing.
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
		auto alpha_source = ShaderAlphaBlendSource::None;
		if (slot == 0 && ps_input_info != nullptr) {
			alpha_source = ps_input_info->alpha_blend_source;
		}
		static_params.blend_enable[slot] = bc.enable && !rt.info.blend_bypass;
		if (static_params.blend_enable[slot] && alpha_source == ShaderAlphaBlendSource::None &&
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
		if (static_params.blend_enable[slot]) {
			auto blend = bc;
			switch (alpha_source) {
				case ShaderAlphaBlendSource::SourceAlpha:
					blend.color_srcblend  = RemapSourceAlphaFactor(blend.color_srcblend);
					blend.color_destblend = RemapSourceAlphaFactor(blend.color_destblend);
					blend.separate_alpha_blend = false;
					break;
				case ShaderAlphaBlendSource::SourceAlphaOne:
				case ShaderAlphaBlendSource::SourceAlphaZero:
					// The second source carries the mapped source factor; its alpha stays logical Sa.
					blend.color_srcblend = static_cast<uint8_t>(Prospero::BlendFactor::kSrc1Color);
					blend.color_destblend =
					    static_cast<uint8_t>(Prospero::BlendFactor::kOneMinusSrc1Alpha);
					blend.separate_alpha_blend = false;
					break;
				case ShaderAlphaBlendSource::None: break;
			}
			static_params.color_srcblend[slot]       = blend.color_srcblend;
			static_params.color_comb_fcn[slot]       = blend.color_comb_fcn;
			static_params.color_destblend[slot]      = blend.color_destblend;
			static_params.separate_alpha_blend[slot] = blend.separate_alpha_blend;
			if (blend.separate_alpha_blend) {
				static_params.alpha_srcblend[slot]  = blend.alpha_srcblend;
				static_params.alpha_comb_fcn[slot]  = blend.alpha_comb_fcn;
				static_params.alpha_destblend[slot] = blend.alpha_destblend;
			}
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
	Common::GpuPhase::Scope gpu_phase_pipeline(Common::GpuPhase::Phase::Pipeline);

	GraphicsPipelineKey key;
	{
		Common::GpuPhase::Scope gpu_phase_key(Common::GpuPhase::Phase::PipeKeyBuild);
		key = BuildGraphicsKey(colors, depth, vertex_info, command.GetRegisters(),
		                       command.GetUserConfig(), ps_input_info, topology,
		                       primitive_restart_enable, programs);
	}
	const bool  ps_active      = ps_input_info != nullptr;
	const auto  vs_id          = programs.vertex[0].id;
	const auto  ps_id          = ps_active ? programs.pixel.id : 0;
	const auto& vertex_program = programs.vertex[0];
	const auto& pixel_program  = programs.pixel;
	const auto& vs_input_info  = vertex_info.front();
	const auto& rendering      = key.rendering;
	const auto& static_params  = key.static_params;

	Pipeline* found = nullptr;
	{
		Common::GpuPhase::Scope gpu_phase_find(Common::GpuPhase::Phase::PipeFind);
		found = FindGraphicsPipelineMemo(key);
	}
	if (found != nullptr) {
		NoteDrawUse(*found, true, false);
		NoteGraphicsVariant(*found, key, programs, ps_active);
		PromoteOptimized(*found);
		return found;
	}

	// --async-pipelines: build the pipeline on a worker. A job captures the key, the vertex/pixel
	// input info (by value), the compiled program handles and the driver cache handle; the
	// stages' CompiledShaderInfo pointers it carries point at published, immutable permutations.
	if (auto* pool = AsyncPool()) {
		const bool                may_skip =
		    m_async_mode == Config::AsyncPipelineMode::Skip && programs.may_skip;
		std::shared_ptr<AsyncJob> job      = m_graphics_jobs.Find(key, may_skip);
		bool joined_prefetch = job != nullptr && job->IsPrefetched();
		bool joined_load     = job != nullptr && static_cast<GraphicsPipelineJob&>(*job).from_load;
		if (job == nullptr) {
			// Not in flight. It may have been published since the lookup above: a worker
			// publishes before it unregisters.
			if (auto* published = FindGraphicsPipeline(key)) {
				NoteDrawUse(*published, true, false);
				NoteGraphicsVariant(*published, key, programs, ps_active);
				PromoteOptimized(*published);
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
				joined_prefetch = job->IsPrefetched();
				joined_load     = static_cast<GraphicsPipelineJob&>(*job).from_load;
				if (may_skip) {
					job->CountSkippedDraw();
				}
			}
		}
		if (job != nullptr) {
			// --skip-guard: a draw writing memory a hard-class reader has read from a skipped draw
			// before waits for its pipeline (asked only here, when the draw would be skipped).
			const bool guard_veto = may_skip && SkipDeps::GuardVetoesSkip();
			if (may_skip && !guard_veto) {
				pool->Stats().skipped_pipeline_draws.fetch_add(1, std::memory_order_relaxed);
				return nullptr; // the caller skips this draw
			}
			programs.pipeline_guard_kept = guard_veto;
			programs.pipeline_waited = true;
			if (pool->WaitFor(*job)) {
				if (auto* published = FindGraphicsPipeline(key)) {
					NoteDrawUse(*published, true, joined_prefetch || joined_load);
					NoteGraphicsVariant(*published, key, programs, ps_active);
					PromoteOptimized(*published);
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
	NoteGraphicsVariant(*published, key, programs, ps_active);

	return published;
}

void PipelineCache::NoteGraphicsVariant(Pipeline& pipeline, const GraphicsPipelineKey& key,
                                        const GraphicsPrograms& programs, bool ps_active) {
	if (m_variants == nullptr || pipeline.gfx_recorded) {
		return;
	}
	pipeline.gfx_recorded = true;
	if (programs.VertexStageCount() != 1) {
		m_load.gfx_unrecorded.fetch_add(1, std::memory_order_relaxed);
		return; // tessellation is not replayed
	}
	Variants::GraphicsVariant variant;
	if (!m_program_cache->FindGraphicsProgram(programs.vertex[0].id, variant.vs) ||
	    (ps_active && !m_program_cache->FindGraphicsProgram(programs.pixel.id, variant.ps))) {
		m_load.gfx_unrecorded.fetch_add(1, std::memory_order_relaxed);
		if (Bench::TimingEnabled()) {
			Bench::LogTiming("graphics_variant_skipped", GraphicsPipelineKeyHash {}(key), Bench::NowNs(), 0,
			                 true, fmt::format("vs_id={};ps_id={}", programs.vertex[0].id, programs.pixel.id));
		}
		return;
	}
	variant.has_ps = ps_active;
	variant.key          = EncodePipelineKey(key.rendering, key.vertex_input, key.static_params);
	variant.first_use_ms = static_cast<uint32_t>(std::min<int64_t>(
	    (Bench::NowNs() - m_program_cache->clock_start_ns) / 1000000, 0xfffffffeLL));
	const bool added = m_variants->Add(variant);
	if (added) {
		m_load.recorded_graphics.fetch_add(1, std::memory_order_relaxed);
		m_load.recorded.fetch_add(1, std::memory_order_relaxed);
	}
	if (Bench::TimingEnabled()) {
		Bench::LogTiming("graphics_variant", GraphicsPipelineKeyHash {}(key), Bench::NowNs(), 0, true,
		                 fmt::format("new={};vs_hash={:016x};ps_hash={:016x};vs_stage={};cursor_vs={};"
		                             "cursor_ps={};vs_spec_buffers={};ps_spec_buffers={};input_bytes={};vs_back={}",
		                             added ? 1 : 0, variant.vs.code_hash, variant.ps.code_hash,
		                             variant.vs.stage, variant.vs.push_data_cursor,
		                             variant.ps.push_data_cursor, variant.vs.spec.buffers.size(),
		                             variant.ps.spec.buffers.size(),
		                             variant.vs.input.size() + variant.ps.input.size(), variant.vs.back_hash != 0 ? 1 : 0));
	}
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
	if ((Config::GetPipelineLookaheadKinds() & 4u) != 0) {
		return; // --pipeline-lookahead-kinds *-programs: debug, programs only
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
	// See PrefetchCompute: a registered job is always submitted.
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
	Common::GpuPhase::Scope gpu_phase_pipeline(Common::GpuPhase::Phase::Pipeline);

	EXIT_IF(!compute_program);

	if (auto* found = FindComputePipeline(compute_program.id)) {
		NoteDrawUse(*found, false, false);
		PromoteOptimized(*found);
		return *found;
	}

	// --async-pipelines: compute dispatches are never skipped, they wait for the worker.
	if (auto* pool = AsyncPool()) {
		std::shared_ptr<AsyncJob> job = m_compute_jobs.Find(compute_program.id);
		bool                      joined_prefetch = job != nullptr && job->IsPrefetched();
		bool                      joined_load =
		    job != nullptr && static_cast<ComputePipelineJob&>(*job).from_load;
		if (job == nullptr) {
			if (auto* published = FindComputePipeline(compute_program.id)) {
				NoteDrawUse(*published, false, false);
				PromoteOptimized(*published);
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
			} else {
				joined_prefetch = job->IsPrefetched();
				joined_load     = static_cast<ComputePipelineJob&>(*job).from_load;
			}
			if (inserted && !pool->Submit(fresh, true)) {
				m_compute_jobs.Erase(compute_program.id);
				job = nullptr;
			}
		}
		if (job != nullptr && pool->WaitFor(*job)) {
			if (auto* published = FindComputePipeline(compute_program.id)) {
				NoteDrawUse(*published, false, joined_prefetch || joined_load);
				PromoteOptimized(*published);
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
