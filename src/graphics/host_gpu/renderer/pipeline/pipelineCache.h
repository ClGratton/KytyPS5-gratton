#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "graphics/host_gpu/renderer/pipeline/asyncPipelineCompiler.h"
#include "graphics/host_gpu/renderer/pipeline/variantStore.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/shader.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>
#include <memory>
#include <shared_mutex>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
struct RenderColorInfo;
struct RenderDepthInfo;
class CommandBuffer;

namespace HW {
class Context;
class Shader;
class UserConfig;
struct ComputeShaderInfo;
} // namespace HW

#pragma pack(push, 1)

struct PipelineStaticParameters {
	bool                       negative_one_to_one      = false;
	bool                       depth_clip_enable        = true;
	vk::PrimitiveTopology      topology                 = vk::PrimitiveTopology::ePointList;
	bool                       primitive_restart_enable = false;
	uint32_t                   samples                  = 1;
	bool                       sample_shading_enable    = false;
	uint32_t                   color_mask[RENDER_COLOR_ATTACHMENTS_MAX]           = {};
	bool                       cull_front                                         = false;
	bool                       cull_back                                          = false;
	bool                       face                                               = false;
	bool                       provoking_vtx_last                                 = false;
	vk::PolygonMode            polygon_mode                                       = vk::PolygonMode::eFill;
	uint8_t                    color_srcblend[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    color_comb_fcn[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    color_destblend[RENDER_COLOR_ATTACHMENTS_MAX]      = {};
	uint8_t                    alpha_srcblend[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    alpha_comb_fcn[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    alpha_destblend[RENDER_COLOR_ATTACHMENTS_MAX]      = {};
	bool                       separate_alpha_blend[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	bool                       blend_enable[RENDER_COLOR_ATTACHMENTS_MAX]         = {};

	bool operator==(const PipelineStaticParameters& other) const noexcept;
};

#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<PipelineStaticParameters>);
static_assert(std::is_standard_layout_v<PipelineStaticParameters>);
static_assert(alignof(PipelineStaticParameters) == 1);
static_assert(sizeof(PipelineStaticParameters) == 116);

struct PipelineRenderingState {
	std::array<vk::Format, RENDER_COLOR_ATTACHMENTS_MAX> color_formats {};
	vk::Format                                           depth_format   = vk::Format::eUndefined;
	vk::Format                                           stencil_format = vk::Format::eUndefined;
	uint32_t                                             color_count    = 0;

	bool operator==(const PipelineRenderingState&) const = default;
};

struct PipelineVertexInputState {
	struct Binding {
		uint32_t stride                           = 0;
		bool     instance                         = false;
		bool     operator==(const Binding&) const = default;
	};
	struct Attribute {
		uint32_t offset                             = 0;
		uint8_t  binding                            = 0;
		bool     operator==(const Attribute&) const = default;
	};

	std::array<Binding, ShaderVertexInputInfo::RES_MAX>   bindings {};
	std::array<Attribute, ShaderVertexInputInfo::RES_MAX> attributes {};
	uint8_t                                               binding_count   = 0;
	uint8_t                                               attribute_count = 0;

	bool operator==(const PipelineVertexInputState&) const = default;
};

struct ShaderProgram {
	uint64_t         id     = 0;
	vk::ShaderModule module = nullptr;

	explicit operator bool() const { return id != 0 && module != nullptr; }
};

// How CreatePipelineInternal gets the VkPipeline (--pipelines-unoptimized-first):
//  Full      the usual create on the driver cache (stock behaviour);
//  Fast      the driver cache when it already holds the pipeline (probed with
//            FAIL_ON_PIPELINE_COMPILE_REQUIRED, nothing is compiled), else compiled with
//            DISABLE_OPTIMIZATION and no cache (Pipeline::unoptimized);
//  Optimize  the optimized build of a Fast pipeline: its layouts are already in `pipeline`, only
//            `pipeline.pipeline` (null) is created, on the driver cache.
enum class PipelineBuild { Full, Fast, Optimize };

// The owning renderer serializes access, including saves while the GPU is running. With
// --async-pipelines on, worker threads also touch the cache: they only publish into the program
// cache (per-entry locks) and into the two pipeline maps (m_pipelines_mutex); see
// docs/phase2-notes.md for the full thread-safety argument.
class PipelineCache {
public:
	explicit PipelineCache(GraphicContext& graphics);
	~PipelineCache();
	KYTY_CLASS_NO_COPY(PipelineCache);
	void Save();

	// The background optimized build of an unoptimized Pipeline (--pipelines-unoptimized-first):
	// a worker stores `pipeline` (null when the build failed) and then sets `done`.
	struct OptimizedBuild {
		std::atomic<bool> done {false};
		vk::Pipeline      pipeline = nullptr;
	};

	struct Pipeline {
		vk::PipelineLayout      pipeline_layout       = nullptr;
		vk::Pipeline            pipeline              = nullptr;
		vk::DescriptorSetLayout descriptor_set_layout = nullptr;
		bool                    uses_push_descriptors = false;
		// --pipeline-lookahead: built by a prefetch job; `claimed` is set by the GPU thread when
		// the first draw/dispatch uses it (prediction hit accounting only).
		bool prefetched = false;
		bool claimed    = false;
		// --pipelines-unoptimized-first: `pipeline` was created with DISABLE_OPTIMIZATION and
		// `optimized` is the background build that replaces it (PromoteOptimized(), GPU thread
		// only). Both are set before the Pipeline is published and cleared by the promotion.
		bool                                            unoptimized = false;
		std::shared_ptr<OptimizedBuild>                 optimized;
	};

	struct GraphicsPrograms {
		std::array<ShaderProgram, 3> vertex;
		ShaderProgram pixel;

		// --async-pipelines skip: a program was not ready, nothing else in here is valid and
		// the draw must be skipped.
		bool pending = false;

		[[nodiscard]] uint32_t VertexStageCount() const { return vertex[1] ? 3u : 1u; }
	};

	GraphicsPrograms
	GetGraphicsPrograms(const HW::VertexShaderInfo& vertex_regs,
	                    const HW::PixelShaderInfo& pixel_regs, const HW::ShaderRegisters& sh,
	                    const HW::Context& context, const HW::UserConfig& user_config,
	                    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping,
	                    bool pixel_active, std::array<ShaderVertexInputInfo, 3>& vertex_info,
	                    ShaderPixelInputInfo& pixel_info);
	ShaderProgram GetComputeProgram(const HW::ComputeShaderInfo& regs,
	                                const HW::ShaderRegisters&   sh,
	                                ShaderComputeInputInfo&      input_info);

	// Returns nullptr only with --async-pipelines skip, when the pipeline is still being built:
	// the caller must skip the draw.
	// ---- --pipeline-lookahead, graphics (lookahead thread only) -----------------------------
	// Same as GetGraphicsPrograms with `prefetch` set: the lookahead thread materializes
	// privately, waits for the program jobs itself and never skips. An empty result (vertex[0]
	// false) means "not prefetchable".
	GraphicsPrograms
	PrefetchGraphicsPrograms(const HW::VertexShaderInfo& vertex_regs,
	                         const HW::PixelShaderInfo& pixel_regs, const HW::ShaderRegisters& sh,
	                         const HW::Context& context, const HW::UserConfig& user_config,
	                         std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping,
	                         bool pixel_active, std::array<ShaderVertexInputInfo, 3>& vertex_info,
	                         ShaderPixelInputInfo& pixel_info);
	// Builds the pipeline key exactly like the draw path and submits the creation job.
	void PrefetchGraphicsPipeline(std::span<const RenderColorInfo>       colors,
	                              const RenderDepthInfo&                 depth,
	                              std::span<const ShaderVertexInputInfo> vertex_info,
	                              const HW::Context& ctx, const HW::UserConfig& user_config,
	                              const ShaderPixelInputInfo* ps_input_info,
	                              vk::PrimitiveTopology topology, bool primitive_restart_enable,
	                              const GraphicsPrograms& programs);

	Pipeline* GetGraphicsPipeline(std::span<const RenderColorInfo>       colors,
	                              const RenderDepthInfo&                 depth,
	                              std::span<const ShaderVertexInputInfo> vertex_info,
	                              CommandBuffer& command, const ShaderPixelInputInfo* ps_input_info,
	                              vk::PrimitiveTopology topology, bool primitive_restart_enable,
	                              const GraphicsPrograms& programs);
	Pipeline& GetComputePipeline(const ShaderComputeInputInfo& input_info,
	                             const ShaderProgram&          compute_program);

	// ---- --pipelines-unoptimized-first (docs/unoptimized-first-notes.md) ---------------------
	// True when new pipelines are created unoptimized and rebuilt optimized in the background:
	// the flag, async pipelines on, an NVIDIA device (or KYTY_PIPELINE_FAST_BUILD=1) and a way
	// to ask the driver cache without compiling.
	[[nodiscard]] bool UnoptimizedFirstActive() const { return m_unoptimized_first; }
	// GPU thread: swaps in the optimized build of `pipeline` when a worker finished it (a no-op
	// otherwise). The replaced VkPipeline may still be used by command buffers in flight; it goes to
	// the retire handler (or is destroyed with the cache). Called by GetGraphicsPipeline /
	// GetComputePipeline before they return, so every draw binds the best handle that is ready.
	void PromoteOptimized(Pipeline& pipeline);
	// The handler gets each replaced VkPipeline and must destroy it once the GPU is done with it;
	// it returns false when it cannot take it now (then the cache keeps it until it is destroyed).
	void SetRetireHandler(std::function<bool(vk::Pipeline)> handler) {
		m_retire_handler = std::move(handler);
	}
	struct UnoptimizedFirstCounters {
		std::atomic<uint64_t> unoptimized_created {0}; // created with DISABLE_OPTIMIZATION
		std::atomic<uint64_t> served_from_cache {0};   // the driver cache held it (probe hit)
		std::atomic<uint64_t> optimized_done {0};      // background builds finished
		std::atomic<uint64_t> swapped {0};             // promoted into the Pipeline
		std::atomic<uint64_t> retired {0};             // replaced handles given to the handler
	};
	[[nodiscard]] const UnoptimizedFirstCounters& UnoptimizedFirst() const {
		return m_unoptimized_counters;
	}

	// ---- --pipeline-lookahead (lookahead thread only, docs/lookahead-notes.md) ---------------
	// Counters of the prediction accounting. Predictions are made by the lookahead thread, hits
	// and misses are counted by the GPU thread when a draw/dispatch needs its pipeline.
	struct LookaheadCounters {
		std::atomic<uint64_t> cs_seen {0};            // dispatches the lookahead looked at
		std::atomic<uint64_t> cs_skipped {0};         // no prefetch (null shader, program not ready, ...)
		std::atomic<uint64_t> cs_requested {0};       // pipeline jobs submitted by the lookahead
		std::atomic<uint64_t> cs_already {0};         // pipeline already published or in flight
		std::atomic<uint64_t> cs_hit_ready {0};       // dispatch found a prefetched pipeline ready
		std::atomic<uint64_t> cs_hit_inflight {0};    // dispatch joined a prefetched job in flight
		std::atomic<uint64_t> cs_miss {0};            // dispatch had to create its own job
		std::atomic<uint64_t> gfx_seen {0};
		std::atomic<uint64_t> gfx_skipped {0};
		std::atomic<uint64_t> gfx_requested {0};
		std::atomic<uint64_t> gfx_already {0};
		std::atomic<uint64_t> gfx_hit_ready {0};
		std::atomic<uint64_t> gfx_hit_inflight {0};
		std::atomic<uint64_t> gfx_miss {0};
		// Why a prefetch request was dropped (graphics draws, then program lookups).
		enum Skip : uint32_t {
			SkipZeroCount, SkipColorMode, SkipDepthCopy, SkipNoVertexShader, SkipTopology,
			SkipIndexType, SkipPrograms, SkipNoTarget, SkipRectNoExport, SkipMesh,
			SkipPrepareFailed, SkipMaterializeFailed, SkipCount
		};
		std::atomic<uint64_t> skip[SkipCount] {};
		static constexpr const char* SkipName(uint32_t reason) {
			constexpr const char* names[] = {"zero_count", "color_mode", "depth_copy",
			                                 "no_vertex_shader", "topology", "index_type",
			                                 "programs", "no_target", "rect_no_export", "mesh",
			                                 "prepare_failed", "materialize_failed"};
			return reason < SkipCount ? names[reason] : "?";
		}
	};
	[[nodiscard]] LookaheadCounters& Lookahead() { return m_lookahead; }
	// Printed with the lookahead counters when the pool stops (the lookahead thread's own stats).
	void SetLookaheadThreadSummary(std::function<std::string()> summary) {
		m_lookahead_thread_summary = std::move(summary);
	}
	// Compute: `mode` is the DISPATCH_DIRECT initiator; `groups` the direct group counts (all
	// zero for an indirect dispatch, like the real path before it reads the arguments). Every
	// input-info field the real dispatch sets before GetComputeProgram() must be set here too:
	// whatever is not part of the program key (lds_storage) still changes the compiled shader.
	void PrefetchCompute(const HW::ComputeShaderInfo& regs, const HW::ShaderRegisters& sh,
	                     uint32_t mode, bool async_compute, const uint32_t (&groups)[3]);

	// ---- --precompile-on-load / --pipeline-variants-replay (docs/shader-load-notes.md) -------
	// Counters of the load-time build and the variant store. Everything the guest thread does at
	// registration is counted here; hits are counted by the GPU thread at the first dispatch.
	struct LoadCounters {
		std::atomic<uint64_t> registered_cs {0};       // compute binaries the guest registered
		std::atomic<uint64_t> registered_other {0};    // other stages (ignored)
		std::atomic<uint64_t> stored_variants {0};     // variants found in the store for them
		std::atomic<uint64_t> jobs_stored {0};         // load jobs for a stored variant
		std::atomic<uint64_t> jobs_guess {0};          // load jobs for the guessed variant
		std::atomic<uint64_t> skipped_unguessable {0}; // typed buffers, bindless, guest-memory specialization
		std::atomic<uint64_t> skipped_call {0};        // S_SWAP_PC programs stay on the GPU thread
		std::atomic<uint64_t> skipped_known {0};       // an equal permutation was already there
		std::atomic<uint64_t> skipped_registers {0};   // the header had no usable compute registers
		std::atomic<uint64_t> built {0};               // permutations published by load jobs
		std::atomic<uint64_t> duplicates {0};          // equal permutation published first by someone else
		std::atomic<uint64_t> pipelines {0};           // pipeline jobs queued by load jobs
		std::atomic<uint64_t> program_hit {0};         // first dispatch found a load-built permutation
		std::atomic<uint64_t> program_other {0};       // dispatch of a loaded binary with another variant
		std::atomic<uint64_t> pipeline_hit_ready {0};  // pipeline was finished at the first dispatch
		std::atomic<uint64_t> pipeline_hit_inflight {0}; // dispatch had to wait for it
		std::atomic<uint64_t> recorded {0};            // new variants written to the store
		std::atomic<uint64_t> lead_n {0};              // register-to-first-use lead of the hits
		std::atomic<int64_t>  lead_sum_ns {0};
		std::atomic<int64_t>  lead_min_ns {std::numeric_limits<int64_t>::max()};
		std::atomic<int64_t>  lead_max_ns {0};
		void NoteLead(int64_t ns) {
			lead_n.fetch_add(1, std::memory_order_relaxed);
			lead_sum_ns.fetch_add(ns, std::memory_order_relaxed);
			auto lo = lead_min_ns.load(std::memory_order_relaxed);
			while (ns < lo && !lead_min_ns.compare_exchange_weak(lo, ns, std::memory_order_relaxed)) {
			}
			auto hi = lead_max_ns.load(std::memory_order_relaxed);
			while (ns > hi && !lead_max_ns.compare_exchange_weak(hi, ns, std::memory_order_relaxed)) {
			}
		}
		[[nodiscard]] std::string Summary() const;
	};
	[[nodiscard]] LoadCounters& Load() { return m_load; }
	[[nodiscard]] const Variants::VariantStore* VariantsStore() const { return m_variants.get(); }

private:
	struct ProgramCache;
	struct GraphicsPipelineJob;
	struct ComputePipelineJob;
	struct GraphicsOptimizeJob;
	struct ComputeOptimizeJob;

	struct GraphicsPipelineKey {
		PipelineRenderingState   rendering;
		std::array<uint64_t, 3>  vertex_shader_ids {};
		uint64_t                 ps_shader_id = 0;
		PipelineVertexInputState vertex_input;
		PipelineStaticParameters static_params;

		bool operator==(const GraphicsPipelineKey& other) const {
			return rendering == other.rendering && vertex_shader_ids == other.vertex_shader_ids &&
			       ps_shader_id == other.ps_shader_id && vertex_input == other.vertex_input &&
			       static_params == other.static_params;
		}
	};

	struct PipelineKeyHash {
		static void Mix(std::size_t& hash, std::size_t value) {
			hash ^= value + static_cast<std::size_t>(0x9e3779b97f4a7c15ull) + (hash << 6u) +
			        (hash >> 2u);
		}
	};

	struct GraphicsPipelineKeyHash {
		std::size_t operator()(const GraphicsPipelineKey& key) const;
	};

	GraphicsPrograms GetGraphicsProgramsImpl(
	    const HW::VertexShaderInfo& vertex_regs, const HW::PixelShaderInfo& pixel_regs,
	    const HW::ShaderRegisters& sh, const HW::Context& context,
	    const HW::UserConfig& user_config,
	    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping,
	    bool pixel_active, std::array<ShaderVertexInputInfo, 3>& vertex_info,
	    ShaderPixelInputInfo& pixel_info, bool prefetch);
	void NoteDrawUse(Pipeline& pipeline, bool graphics, bool joined_in_flight);
	GraphicsPipelineKey BuildGraphicsKey(std::span<const RenderColorInfo>       colors,
	                                     const RenderDepthInfo&                 depth,
	                                     std::span<const ShaderVertexInputInfo> vertex_info,
	                                     const HW::Context& ctx, const HW::UserConfig& user_config,
	                                     const ShaderPixelInputInfo* ps_input_info,
	                                     vk::PrimitiveTopology topology,
	                                     bool primitive_restart_enable,
	                                     const GraphicsPrograms& programs);

	GraphicContext&               m_graphics;
	std::unique_ptr<ProgramCache> m_program_cache;
	vk::PipelineCache             m_driver_cache = nullptr;
	std::filesystem::path         m_driver_cache_path;
	// Guarded by m_pipelines_mutex whenever m_async_mode != Off (workers publish into them).
	// Pipeline objects are heap nodes that are never erased while the cache lives, so a returned
	// Pipeline& / Pipeline* stays valid after the lock is dropped.
	std::unordered_map<GraphicsPipelineKey, std::unique_ptr<Pipeline>, GraphicsPipelineKeyHash>
	                                                        m_graphics_pipelines;
	std::unordered_map<uint64_t, std::unique_ptr<Pipeline>> m_compute_pipelines;
	std::shared_mutex                                       m_pipelines_mutex;

	// --async-pipelines. The mode is fixed at construction; m_async is null in Off mode.
	const Config::AsyncPipelineMode m_async_mode;
	std::unique_ptr<AsyncPipelineCompiler> m_async;
	InFlightRegistry<GraphicsPipelineKey, GraphicsPipelineKeyHash> m_graphics_jobs;
	InFlightRegistry<uint64_t>                                     m_compute_jobs;

	// --pipelines-unoptimized-first (fixed at construction, after the driver cache is set up).
	bool                       m_unoptimized_first = false;
	UnoptimizedFirstCounters   m_unoptimized_counters;
	std::function<bool(vk::Pipeline)> m_retire_handler;
	std::vector<vk::Pipeline>  m_retired_pipelines; // no handler took them: destroyed with the cache

	LookaheadCounters m_lookahead;
	LoadCounters      m_load;
	std::unique_ptr<Variants::VariantStore> m_variants; // --pipeline-variants-replay
	void InitializeVariants();
	std::function<std::string()> m_lookahead_thread_summary;
	// Taken shared by every prefetch call, exclusive by ShutdownAsync(): no prefetch is running
	// (or starts) while the pool and the registries are torn down.
	std::shared_mutex m_prefetch_gate;
	bool              m_prefetch_stopped = false;

	void InitializeDriverCache();
	// The pool when async is on and still running (it stops in Save()), else nullptr.
	AsyncPipelineCompiler* AsyncPool();
	// Stops the workers (cancels queued jobs, waits for running ones). Idempotent.
	void ShutdownAsync();
	Pipeline* FindGraphicsPipeline(const GraphicsPipelineKey& key);
	Pipeline* FindComputePipeline(uint64_t program_id);
	// Inserts a finished pipeline; returns the one in the map (ours, or an existing equal one, in
	// which case `inserted` is false and `pipeline` is left untouched for the caller to destroy).
	Pipeline* PublishGraphicsPipeline(const GraphicsPipelineKey& key,
	                                  std::unique_ptr<Pipeline>& pipeline, bool& inserted);
	Pipeline* PublishComputePipeline(uint64_t program_id, std::unique_ptr<Pipeline>& pipeline,
	                                 bool& inserted);
	void      DestroyPipeline(Pipeline& pipeline);
	// Worker side: the build mode for a new job, and the follow-up of an unoptimized pipeline.
	[[nodiscard]] PipelineBuild FirstBuild() const {
		return m_unoptimized_first ? PipelineBuild::Fast : PipelineBuild::Full;
	}
	void DecideUnoptimizedFirst();
	void NoteFirstBuild(const Pipeline& created);
};

void LogPipelineTrace(const char* phase, uint64_t vertex_program_id, uint64_t pixel_program_id);
void CreatePipelineInternal(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                            const PipelineRenderingState&          rendering,
                            const PipelineVertexInputState&        vertex_input,
                            std::span<const ShaderVertexInputInfo> vertex_info,
                            const ShaderPixelInputInfo*            ps_input_info,
                            const PipelineCache::GraphicsPrograms& programs,
                            const PipelineStaticParameters&        static_params,
                            vk::PipelineCache                      driver_cache,
                            PipelineBuild                          build = PipelineBuild::Full);
void CreatePipelineInternal(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                            const ShaderComputeInputInfo& input_info,
                            vk::ShaderModule compute_module, vk::PipelineCache driver_cache,
                            PipelineBuild build = PipelineBuild::Full);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_
