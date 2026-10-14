#include "graphics/host_gpu/renderer/gpuCost.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Debug-only (env KYTY_GPU_COST). Files written, all CSV with a header line:
//   <prefix>.cb.csv      one row per submitted command buffer
//                        serial,frame,tick,timed,begin_ns,end_ns,draws,dispatches,barriers,copies,clears,passes,ops
//   <prefix>.ops.csv     one row per timed operation (window frames only)
//                        serial,frame,idx,kind,pass,pipe,s0,s1,a,b,c,d,t_ns,pre_ns
//     t_ns is the device timestamp (ns) taken right after the operation, idx 0 is the begin timestamp;
//     pre_ns (mode both) the TOP_OF_PIPE timestamp taken just before the operation, else 0.
//     kind-specific columns: draw a=vertices/indices b=instances; dispatch a,b,c=groups;
//     copy a=bytes or texels b=regions; barrier a=src stages b=dst stages c=mem|buf<<8|img<<16 d=layout changes;
//     begin_rendering a=width b=height c=colors|layers<<8 d=flags (1 depth, 2 stencil, 4 any clear/load).
//   <prefix>.shaders.csv hash,stage,bvh,lds,function_lds,function_lds_dwords,wave,threads,buffers,images,samplers,dma,gds
namespace Libs::Graphics::GpuCost {

std::atomic<bool> g_enabled {false};

namespace {

enum Kind : uint8_t {
	KBegin,
	KEnd,
	KDraw,
	KDrawIndexed,
	KDrawIndirect,
	KDrawMesh,
	KDispatch,
	KDispatchIndirect,
	KCopyBuffer,
	KCopyImage,
	KBufferToImage,
	KImageToBuffer,
	KBlit,
	KFill,
	KUpdate,
	KClearColor,
	KClearDepth,
	KClearAttachments,
	KResolve,
	KBarrier,
	KBeginRendering,
	KEndRendering,
	KindCount
};

const char* const kKindNames[KindCount] = {
    "begin",     "end",         "draw",      "draw_indexed", "draw_indirect", "draw_mesh",
    "dispatch",  "dispatch_ind", "copy_buf", "copy_img",     "buf2img",       "img2buf",
    "blit",      "fill",        "update",    "clear_color",  "clear_depth",   "clear_att",
    "resolve",   "barrier",     "begin_rp",  "end_rp"};

enum class Mode { CommandBuffers, Bottom, Top, Both };

constexpr uint32_t POOL_SLOTS = 1u << 18;

struct Op {
	uint8_t  kind = 0;
	int32_t  slot = -1;
	int32_t  pre_slot = -1;
	uint32_t pass = 0;
	uint64_t pipe = 0;
	uint64_t s0   = 0;
	uint64_t s1   = 0;
	uint32_t a = 0, b = 0, c = 0, d = 0;
};

struct Record {
	VkCommandBuffer cb      = nullptr;
	uint64_t        serial  = 0;
	uint64_t        frame   = 0;
	uint64_t        tick    = 0;
	bool            ops_timed = false;
	int32_t         begin_slot = -1;
	int32_t         end_slot   = -1;
	uint32_t        pass    = 0;
	uint64_t        pipe    = 0;
	int32_t         pending_pre = -1;
	uint64_t        s0 = 0, s1 = 0;
	uint32_t        draws = 0, dispatches = 0, barriers = 0, copies = 0, clears = 0, passes = 0, ops_count = 0;
	std::vector<Op> ops;
};

struct State {
	vk::Device      device;
	VkQueryPool     pool = nullptr;
	Mode            mode = Mode::Bottom;
	double          period = 1.0;
	uint64_t        every = 100, window = 20, start = 0;
	uint32_t        drop = 0; // KYTY_GPU_COST_DROP: 1 = leave out barriers that carry no image barrier (measurement only, unsafe)
	std::atomic<uint64_t> frame {0};
	std::atomic<uint64_t> serial {0};
	std::atomic<uint32_t> cursor {0};
	std::atomic<uint32_t> in_use {0};
	std::atomic<uint64_t> dropped_ops {0};
	std::mutex      map_mutex;
	std::unordered_map<VkCommandBuffer, std::shared_ptr<Record>> records;
	std::mutex      file_mutex;
	FILE*           cb_file = nullptr;
	FILE*           ops_file = nullptr;
	FILE*           shaders_file = nullptr;
	std::mutex      pending_mutex;
	std::deque<std::shared_ptr<Record>> pending;
	std::unordered_set<uint64_t> described;
	std::mutex      described_mutex;
};

State* g_state = nullptr;

thread_local uint64_t t_tag0 = 0;
thread_local uint64_t t_tag1 = 0;

// Originals of the wrapped entry points.
PFN_vkCmdWriteTimestamp           o_write_timestamp    = nullptr;
PFN_vkCmdBindPipeline             o_bind_pipeline      = nullptr;
PFN_vkCmdDraw                     o_draw               = nullptr;
PFN_vkCmdDrawIndexed              o_draw_indexed       = nullptr;
PFN_vkCmdDrawIndirect             o_draw_indirect      = nullptr;
PFN_vkCmdDrawIndexedIndirect      o_draw_indexed_indirect = nullptr;
PFN_vkCmdDrawMeshTasksEXT         o_draw_mesh          = nullptr;
PFN_vkCmdDrawMeshTasksIndirectEXT o_draw_mesh_indirect = nullptr;
PFN_vkCmdDispatch                 o_dispatch           = nullptr;
PFN_vkCmdDispatchIndirect         o_dispatch_indirect  = nullptr;
PFN_vkCmdCopyBuffer               o_copy_buffer        = nullptr;
PFN_vkCmdCopyImage                o_copy_image         = nullptr;
PFN_vkCmdCopyBufferToImage        o_buffer_to_image    = nullptr;
PFN_vkCmdCopyImageToBuffer        o_image_to_buffer    = nullptr;
PFN_vkCmdBlitImage                o_blit_image         = nullptr;
PFN_vkCmdFillBuffer               o_fill_buffer        = nullptr;
PFN_vkCmdUpdateBuffer             o_update_buffer      = nullptr;
PFN_vkCmdClearColorImage          o_clear_color        = nullptr;
PFN_vkCmdClearDepthStencilImage   o_clear_depth        = nullptr;
PFN_vkCmdClearAttachments         o_clear_attachments  = nullptr;
PFN_vkCmdResolveImage             o_resolve_image      = nullptr;
PFN_vkCmdPipelineBarrier          o_barrier            = nullptr;
PFN_vkCmdPipelineBarrier2         o_barrier2           = nullptr;
PFN_vkCmdPipelineBarrier2KHR      o_barrier2_khr       = nullptr;
PFN_vkCmdBeginRendering           o_begin_rendering    = nullptr;
PFN_vkCmdBeginRenderingKHR        o_begin_rendering_khr = nullptr;
PFN_vkCmdEndRendering             o_end_rendering      = nullptr;
PFN_vkCmdEndRenderingKHR          o_end_rendering_khr  = nullptr;
PFN_vkEndCommandBuffer            o_end_command_buffer = nullptr;

std::shared_ptr<Record> Find(VkCommandBuffer cb) {
	std::lock_guard lock(g_state->map_mutex);
	const auto      it = g_state->records.find(cb);
	return it == g_state->records.end() ? nullptr : it->second;
}

bool AllocSlot(int32_t& slot) {
	auto& s = *g_state;
	if (s.in_use.load(std::memory_order_relaxed) + 1u >= POOL_SLOTS) {
		s.dropped_ops.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	s.in_use.fetch_add(1, std::memory_order_relaxed);
	slot = static_cast<int32_t>(s.cursor.fetch_add(1, std::memory_order_relaxed) % POOL_SLOTS);
	return true;
}

void Stamp(VkCommandBuffer cb, VkPipelineStageFlagBits stage, int32_t& slot) {
	if (AllocSlot(slot)) {
		o_write_timestamp(cb, stage, g_state->pool, static_cast<uint32_t>(slot));
	}
}

// --mode both: a TOP_OF_PIPE timestamp before the operation, so the time between the previous
// operation finishing and this one starting (barrier wait, launch latency) is separate from its run time.
void Before(VkCommandBuffer cb) {
	if (g_state->mode != Mode::Both) {
		return;
	}
	const auto record = Find(cb);
	if (!record || !record->ops_timed) {
		return;
	}
	record->pending_pre = -1;
	Stamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, record->pending_pre);
}

// Counts the operation and, in a timed record, timestamps it.
void After(VkCommandBuffer cb, Kind kind, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0,
           uint32_t d = 0) {
	const auto record = Find(cb);
	if (!record) {
		return;
	}
	auto& r = *record;
	r.ops_count++;
	switch (kind) {
		case KDraw: case KDrawIndexed: case KDrawIndirect: case KDrawMesh: r.draws++; break;
		case KDispatch: case KDispatchIndirect: r.dispatches++; break;
		case KBarrier: r.barriers++; break;
		case KClearColor: case KClearDepth: case KClearAttachments: r.clears++; break;
		case KBeginRendering: r.passes++; break;
		case KEndRendering: break;
		default: r.copies++; break;
	}
	if (!r.ops_timed) {
		return;
	}
	Op op;
	op.kind = kind;
	op.pass = r.pass;
	op.pipe = r.pipe;
	op.s0   = r.s0;
	op.s1   = r.s1;
	op.a = a; op.b = b; op.c = c; op.d = d;
	op.pre_slot = r.pending_pre;
	r.pending_pre = -1;
	Stamp(cb, g_state->mode == Mode::Top ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
	                                     : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
	      op.slot);
	r.ops.push_back(op);
}

void VKAPI_CALL W_BindPipeline(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipeline pipeline) {
	o_bind_pipeline(cb, point, pipeline);
	if (const auto record = Find(cb)) {
		record->pipe = reinterpret_cast<uint64_t>(pipeline);
		record->s0   = t_tag0;
		record->s1   = t_tag1;
		t_tag0 = t_tag1 = 0;
	}
}

void VKAPI_CALL W_Draw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
	Before(cb);
	o_draw(cb, vc, ic, fv, fi);
	After(cb, KDraw, vc, ic);
}
void VKAPI_CALL W_DrawIndexed(VkCommandBuffer cb, uint32_t xc, uint32_t ic, uint32_t fi, int32_t vo,
                              uint32_t fin) {
	Before(cb);
	o_draw_indexed(cb, xc, ic, fi, vo, fin);
	After(cb, KDrawIndexed, xc, ic);
}
void VKAPI_CALL W_DrawIndirect(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, uint32_t count,
                               uint32_t stride) {
	Before(cb);
	o_draw_indirect(cb, buf, off, count, stride);
	After(cb, KDrawIndirect, count);
}
void VKAPI_CALL W_DrawIndexedIndirect(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off,
                                      uint32_t count, uint32_t stride) {
	Before(cb);
	o_draw_indexed_indirect(cb, buf, off, count, stride);
	After(cb, KDrawIndirect, count);
}
void VKAPI_CALL W_DrawMesh(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
	Before(cb);
	o_draw_mesh(cb, x, y, z);
	After(cb, KDrawMesh, x, y, z);
}
void VKAPI_CALL W_DrawMeshIndirect(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, uint32_t count,
                                   uint32_t stride) {
	Before(cb);
	o_draw_mesh_indirect(cb, buf, off, count, stride);
	After(cb, KDrawMesh, count);
}
void VKAPI_CALL W_Dispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
	Before(cb);
	o_dispatch(cb, x, y, z);
	After(cb, KDispatch, x, y, z);
}
void VKAPI_CALL W_DispatchIndirect(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off) {
	Before(cb);
	o_dispatch_indirect(cb, buf, off);
	After(cb, KDispatchIndirect);
}
void VKAPI_CALL W_CopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t count,
                             const VkBufferCopy* regions) {
	Before(cb);
	o_copy_buffer(cb, src, dst, count, regions);
	uint64_t bytes = 0;
	for (uint32_t i = 0; i < count; i++) bytes += regions[i].size;
	After(cb, KCopyBuffer, static_cast<uint32_t>(std::min<uint64_t>(bytes, UINT32_MAX)), count);
}
uint32_t Texels(const VkExtent3D& e) {
	return static_cast<uint32_t>(std::min<uint64_t>(
	    static_cast<uint64_t>(e.width) * e.height * std::max(1u, e.depth), UINT32_MAX));
}
void VKAPI_CALL W_CopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst,
                            VkImageLayout dl, uint32_t count, const VkImageCopy* regions) {
	Before(cb);
	o_copy_image(cb, src, sl, dst, dl, count, regions);
	After(cb, KCopyImage, count > 0 ? Texels(regions[0].extent) : 0, count);
}
void VKAPI_CALL W_BufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout dl,
                                uint32_t count, const VkBufferImageCopy* regions) {
	Before(cb);
	o_buffer_to_image(cb, src, dst, dl, count, regions);
	uint64_t texels = 0;
	for (uint32_t i = 0; i < count; i++) texels += Texels(regions[i].imageExtent);
	After(cb, KBufferToImage, static_cast<uint32_t>(std::min<uint64_t>(texels, UINT32_MAX)), count);
}
void VKAPI_CALL W_ImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkBuffer dst,
                                uint32_t count, const VkBufferImageCopy* regions) {
	Before(cb);
	o_image_to_buffer(cb, src, sl, dst, count, regions);
	uint64_t texels = 0;
	for (uint32_t i = 0; i < count; i++) texels += Texels(regions[i].imageExtent);
	After(cb, KImageToBuffer, static_cast<uint32_t>(std::min<uint64_t>(texels, UINT32_MAX)), count);
}
void VKAPI_CALL W_Blit(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst,
                       VkImageLayout dl, uint32_t count, const VkImageBlit* regions, VkFilter f) {
	Before(cb);
	o_blit_image(cb, src, sl, dst, dl, count, regions, f);
	After(cb, KBlit, 0, count);
}
void VKAPI_CALL W_Fill(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize off, VkDeviceSize size,
                       uint32_t data) {
	Before(cb);
	o_fill_buffer(cb, dst, off, size, data);
	After(cb, KFill, static_cast<uint32_t>(std::min<uint64_t>(size, UINT32_MAX)));
}
void VKAPI_CALL W_Update(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize off, VkDeviceSize size,
                         const void* data) {
	Before(cb);
	o_update_buffer(cb, dst, off, size, data);
	After(cb, KUpdate, static_cast<uint32_t>(std::min<uint64_t>(size, UINT32_MAX)));
}
void VKAPI_CALL W_ClearColor(VkCommandBuffer cb, VkImage image, VkImageLayout layout,
                             const VkClearColorValue* color, uint32_t count,
                             const VkImageSubresourceRange* ranges) {
	Before(cb);
	o_clear_color(cb, image, layout, color, count, ranges);
	After(cb, KClearColor, 0, count);
}
void VKAPI_CALL W_ClearDepth(VkCommandBuffer cb, VkImage image, VkImageLayout layout,
                             const VkClearDepthStencilValue* value, uint32_t count,
                             const VkImageSubresourceRange* ranges) {
	Before(cb);
	o_clear_depth(cb, image, layout, value, count, ranges);
	After(cb, KClearDepth, 0, count);
}
void VKAPI_CALL W_ClearAttachments(VkCommandBuffer cb, uint32_t ac, const VkClearAttachment* a,
                                   uint32_t rc, const VkClearRect* r) {
	Before(cb);
	o_clear_attachments(cb, ac, a, rc, r);
	After(cb, KClearAttachments, ac, rc);
}
void VKAPI_CALL W_Resolve(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst,
                          VkImageLayout dl, uint32_t count, const VkImageResolve* regions) {
	Before(cb);
	o_resolve_image(cb, src, sl, dst, dl, count, regions);
	After(cb, KResolve, count > 0 ? Texels(regions[0].extent) : 0, count);
}
void VKAPI_CALL W_Barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                          VkDependencyFlags flags, uint32_t mc, const VkMemoryBarrier* m,
                          uint32_t bc, const VkBufferMemoryBarrier* b, uint32_t ic,
                          const VkImageMemoryBarrier* i) {
	if ((g_state->drop & 1u) == 0 || ic != 0) {
		o_barrier(cb, src, dst, flags, mc, m, bc, b, ic, i);
	}
	uint32_t changes = 0;
	for (uint32_t k = 0; k < ic; k++) changes += i[k].oldLayout != i[k].newLayout;
	After(cb, KBarrier, src, dst, (mc & 0xffu) | ((bc & 0xffu) << 8) | ((ic & 0xffu) << 16),
	      changes);
}
void Barrier2(VkCommandBuffer cb, const VkDependencyInfo* info) {
	uint64_t src = 0, dst = 0;
	uint32_t changes = 0;
	for (uint32_t k = 0; k < info->memoryBarrierCount; k++) {
		src |= info->pMemoryBarriers[k].srcStageMask;
		dst |= info->pMemoryBarriers[k].dstStageMask;
	}
	for (uint32_t k = 0; k < info->bufferMemoryBarrierCount; k++) {
		src |= info->pBufferMemoryBarriers[k].srcStageMask;
		dst |= info->pBufferMemoryBarriers[k].dstStageMask;
	}
	for (uint32_t k = 0; k < info->imageMemoryBarrierCount; k++) {
		src |= info->pImageMemoryBarriers[k].srcStageMask;
		dst |= info->pImageMemoryBarriers[k].dstStageMask;
		changes += info->pImageMemoryBarriers[k].oldLayout != info->pImageMemoryBarriers[k].newLayout;
	}
	After(cb, KBarrier, static_cast<uint32_t>(src), static_cast<uint32_t>(dst),
	      (info->memoryBarrierCount & 0xffu) | ((info->bufferMemoryBarrierCount & 0xffu) << 8) |
	          ((info->imageMemoryBarrierCount & 0xffu) << 16),
	      changes);
}
void VKAPI_CALL W_Barrier2(VkCommandBuffer cb, const VkDependencyInfo* info) {
	if ((g_state->drop & 1u) == 0 || info->imageMemoryBarrierCount != 0) {
		o_barrier2(cb, info);
	}
	Barrier2(cb, info);
}
void VKAPI_CALL W_Barrier2Khr(VkCommandBuffer cb, const VkDependencyInfo* info) {
	if ((g_state->drop & 1u) == 0 || info->imageMemoryBarrierCount != 0) {
		o_barrier2_khr(cb, info);
	}
	Barrier2(cb, info);
}
void BeginRendering(VkCommandBuffer cb, const VkRenderingInfo* info) {
	if (const auto record = Find(cb)) {
		record->pass = record->passes + 1u;
	}
	uint32_t flags = info->pDepthAttachment != nullptr ? 1u : 0u;
	flags |= info->pStencilAttachment != nullptr ? 2u : 0u;
	bool load = false;
	for (uint32_t k = 0; k < info->colorAttachmentCount; k++) {
		load |= info->pColorAttachments[k].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
	}
	load |= info->pDepthAttachment != nullptr &&
	        info->pDepthAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
	flags |= load ? 4u : 0u;
	After(cb, KBeginRendering, info->renderArea.extent.width, info->renderArea.extent.height,
	      (info->colorAttachmentCount & 0xffu) | ((info->layerCount & 0xffu) << 8), flags);
}
void VKAPI_CALL W_BeginRendering(VkCommandBuffer cb, const VkRenderingInfo* info) {
	o_begin_rendering(cb, info);
	BeginRendering(cb, info);
}
void VKAPI_CALL W_BeginRenderingKhr(VkCommandBuffer cb, const VkRenderingInfo* info) {
	o_begin_rendering_khr(cb, info);
	BeginRendering(cb, info);
}
void VKAPI_CALL W_EndRendering(VkCommandBuffer cb) {
	o_end_rendering(cb);
	After(cb, KEndRendering);
}
void VKAPI_CALL W_EndRenderingKhr(VkCommandBuffer cb) {
	o_end_rendering_khr(cb);
	After(cb, KEndRendering);
}
VkResult VKAPI_CALL W_EndCommandBuffer(VkCommandBuffer cb) {
	if (const auto record = Find(cb)) {
		Stamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, record->end_slot);
	}
	return o_end_command_buffer(cb);
}

template <typename F, typename W>
void Wrap(F& slot, F& original, W wrapper) {
	if (slot != nullptr) {
		original = slot;
		slot     = reinterpret_cast<F>(wrapper);
	}
}

void Say(const char* format, ...) __attribute__((format(printf, 1, 2)));
void Say(const char* format, ...) {
	char    text[512];
	va_list args;
	va_start(args, format);
	std::vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	Log::WriteToConsoleAndLog(std::string(text));
}

// The command buffer's timestamps once its tick completed: written to the CSV files.
void Resolve(Record& r) {
	auto& s = *g_state;
	// Slots in recording order: begin, then (pre, op) per operation, then end.
	std::vector<int32_t> slots;
	slots.reserve(2 * r.ops.size() + 2);
	slots.push_back(r.begin_slot);
	for (const auto& op: r.ops) {
		slots.push_back(op.pre_slot);
		slots.push_back(op.slot);
	}
	slots.push_back(r.end_slot);
	std::vector<uint64_t> ticks(slots.size(), 0);
	const auto device = s.device;
	// Contiguous runs of slots are read with one call each.
	size_t   i        = 0;
	uint32_t released = 0;
	while (i < slots.size()) {
		if (slots[i] < 0) {
			i++;
			continue;
		}
		size_t j = i + 1;
		while (j < slots.size() && slots[j] >= 0 && slots[j] == slots[j - 1] + 1) {
			j++;
		}
		const auto count = static_cast<uint32_t>(j - i);
		const auto first = static_cast<uint32_t>(slots[i]);
		(void)device.getQueryPoolResults(s.pool, first, count, count * sizeof(uint64_t),
		                                 &ticks[i], sizeof(uint64_t),
		                                 vk::QueryResultFlagBits::e64);
		device.resetQueryPool(s.pool, first, count);
		released += count;
		i = j;
	}
	s.in_use.fetch_sub(released, std::memory_order_relaxed);
	const auto ns = [&](uint64_t t) {
		return static_cast<uint64_t>(static_cast<double>(t) * s.period);
	};
	std::lock_guard lock(s.file_mutex);
	const bool timed = r.begin_slot >= 0 && r.end_slot >= 0;
	std::fprintf(s.cb_file,
	             "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%d,%" PRIu64 ",%" PRIu64
	             ",%u,%u,%u,%u,%u,%u,%u\n",
	             r.serial, r.frame, r.tick, timed ? 1 : 0,
	             timed ? ns(ticks.front()) : 0, timed ? ns(ticks.back()) : 0, r.draws, r.dispatches,
	             r.barriers, r.copies, r.clears, r.passes, r.ops_count);
	if (r.ops_timed && timed) {
		std::fprintf(s.ops_file, "%" PRIu64 ",%" PRIu64 ",0,begin,0,0,0,0,0,0,0,0,%" PRIu64 ",0\n",
		             r.serial, r.frame, ns(ticks.front()));
		for (size_t k = 0; k < r.ops.size(); k++) {
			const auto& op = r.ops[k];
			if (op.slot < 0) {
				continue;
			}
			const uint64_t pre = op.pre_slot >= 0 ? ns(ticks[1 + 2 * k]) : 0;
			std::fprintf(s.ops_file,
			             "%" PRIu64 ",%" PRIu64 ",%zu,%s,%u,%" PRIx64 ",%" PRIx64 ",%" PRIx64
			             ",%u,%u,%u,%u,%" PRIu64 ",%" PRIu64 "\n",
			             r.serial, r.frame, k + 1, kKindNames[op.kind], op.pass, op.pipe, op.s0,
			             op.s1, op.a, op.b, op.c, op.d, ns(ticks[2 + 2 * k]), pre);
		}
		std::fprintf(s.ops_file, "%" PRIu64 ",%" PRIu64 ",%zu,end,0,0,0,0,0,0,0,0,%" PRIu64 ",0\n",
		             r.serial, r.frame, r.ops.size() + 1, ns(ticks.back()));
	}
	std::fflush(s.cb_file);
	if (r.ops_timed) {
		std::fflush(s.ops_file);
	}
}

} // namespace

void Initialize(GraphicContext& graphics) {
	static bool done = false;
	if (done) {
		return;
	}
	done = true;
	const char* prefix = std::getenv("KYTY_GPU_COST");
	if (prefix == nullptr || prefix[0] == '\0') {
		return;
	}
	if (!graphics.host_query_reset_enabled ||
	    graphics.physical_device_properties.limits.timestampComputeAndGraphics == VK_FALSE) {
		Say("GPU cost: timestamps or host query reset are not available, off\n");
		return;
	}
	auto* s  = new State;
	s->device = graphics.device;
	s->period = static_cast<double>(graphics.physical_device_properties.limits.timestampPeriod);
	if (const char* mode = std::getenv("KYTY_GPU_COST_MODE")) {
		s->mode = std::strcmp(mode, "cb") == 0     ? Mode::CommandBuffers
		          : std::strcmp(mode, "top") == 0  ? Mode::Top
		          : std::strcmp(mode, "both") == 0 ? Mode::Both
		                                           : Mode::Bottom;
	}
	if (const char* v = std::getenv("KYTY_GPU_COST_EVERY")) s->every = std::strtoull(v, nullptr, 10);
	if (const char* v = std::getenv("KYTY_GPU_COST_WINDOW")) s->window = std::strtoull(v, nullptr, 10);
	if (const char* v = std::getenv("KYTY_GPU_COST_START")) s->start = std::strtoull(v, nullptr, 10);
	if (const char* v = std::getenv("KYTY_GPU_COST_DROP")) s->drop = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
	if (s->every == 0) s->every = 1;

	vk::QueryPoolCreateInfo info {};
	info.queryType  = vk::QueryType::eTimestamp;
	info.queryCount = POOL_SLOTS;
	vk::QueryPool pool = nullptr;
	if (graphics.device.createQueryPool(&info, nullptr, &pool) != vk::Result::eSuccess) {
		Say("GPU cost: query pool failed, off\n");
		delete s;
		return;
	}
	graphics.device.resetQueryPool(pool, 0, POOL_SLOTS);
	s->pool = pool;

	const std::string base(prefix);
	s->cb_file      = std::fopen((base + ".cb.csv").c_str(), "w");
	s->ops_file     = std::fopen((base + ".ops.csv").c_str(), "w");
	s->shaders_file = std::fopen((base + ".shaders.csv").c_str(), "w");
	if (s->cb_file == nullptr || s->ops_file == nullptr || s->shaders_file == nullptr) {
		Say("GPU cost: cannot create the output files at %s, off\n", prefix);
		delete s;
		return;
	}
	std::fprintf(s->cb_file,
	             "serial,frame,tick,timed,begin_ns,end_ns,draws,dispatches,barriers,copies,clears,"
	             "passes,ops\n");
	std::fprintf(s->ops_file, "serial,frame,idx,kind,pass,pipe,s0,s1,a,b,c,d,t_ns,pre_ns\n");
	std::fprintf(s->shaders_file,
	             "hash,stage,bvh,lds,function_lds,function_lds_dwords,wave,threads,buffers,images,"
	             "samplers,dma,gds\n");
	g_state = s;

	auto& d = VULKAN_HPP_DEFAULT_DISPATCHER;
	o_write_timestamp = d.vkCmdWriteTimestamp;
	Wrap(d.vkCmdBindPipeline, o_bind_pipeline, W_BindPipeline);
	Wrap(d.vkCmdDraw, o_draw, W_Draw);
	Wrap(d.vkCmdDrawIndexed, o_draw_indexed, W_DrawIndexed);
	Wrap(d.vkCmdDrawIndirect, o_draw_indirect, W_DrawIndirect);
	Wrap(d.vkCmdDrawIndexedIndirect, o_draw_indexed_indirect, W_DrawIndexedIndirect);
	Wrap(d.vkCmdDrawMeshTasksEXT, o_draw_mesh, W_DrawMesh);
	Wrap(d.vkCmdDrawMeshTasksIndirectEXT, o_draw_mesh_indirect, W_DrawMeshIndirect);
	Wrap(d.vkCmdDispatch, o_dispatch, W_Dispatch);
	Wrap(d.vkCmdDispatchIndirect, o_dispatch_indirect, W_DispatchIndirect);
	Wrap(d.vkCmdCopyBuffer, o_copy_buffer, W_CopyBuffer);
	Wrap(d.vkCmdCopyImage, o_copy_image, W_CopyImage);
	Wrap(d.vkCmdCopyBufferToImage, o_buffer_to_image, W_BufferToImage);
	Wrap(d.vkCmdCopyImageToBuffer, o_image_to_buffer, W_ImageToBuffer);
	Wrap(d.vkCmdBlitImage, o_blit_image, W_Blit);
	Wrap(d.vkCmdFillBuffer, o_fill_buffer, W_Fill);
	Wrap(d.vkCmdUpdateBuffer, o_update_buffer, W_Update);
	Wrap(d.vkCmdClearColorImage, o_clear_color, W_ClearColor);
	Wrap(d.vkCmdClearDepthStencilImage, o_clear_depth, W_ClearDepth);
	Wrap(d.vkCmdClearAttachments, o_clear_attachments, W_ClearAttachments);
	Wrap(d.vkCmdResolveImage, o_resolve_image, W_Resolve);
	Wrap(d.vkCmdPipelineBarrier, o_barrier, W_Barrier);
	Wrap(d.vkCmdPipelineBarrier2, o_barrier2, W_Barrier2);
	Wrap(d.vkCmdPipelineBarrier2KHR, o_barrier2_khr, W_Barrier2Khr);
	Wrap(d.vkCmdBeginRendering, o_begin_rendering, W_BeginRendering);
	Wrap(d.vkCmdBeginRenderingKHR, o_begin_rendering_khr, W_BeginRenderingKhr);
	Wrap(d.vkCmdEndRendering, o_end_rendering, W_EndRendering);
	Wrap(d.vkCmdEndRenderingKHR, o_end_rendering_khr, W_EndRenderingKhr);
	Wrap(d.vkEndCommandBuffer, o_end_command_buffer, W_EndCommandBuffer);
	g_enabled.store(true, std::memory_order_release);
	if (s->drop != 0) {
		Say("GPU cost: DROP=%u, barriers without image barriers are NOT recorded (measurement only, output may be wrong)\n", s->drop);
	}
	Say("GPU cost: on, output %s.*.csv, mode %s, window %" PRIu64 " frames every %" PRIu64
	    " from frame %" PRIu64 " (%.3f ns per tick)\n",
	    prefix, s->mode == Mode::CommandBuffers ? "cb" : (s->mode == Mode::Top ? "top" : (s->mode == Mode::Both ? "both" : "bottom")),
	    s->window, s->every, s->start, s->period);
}

bool DropGdsBarrier() noexcept {
	return g_state != nullptr && (g_state->drop & 2u) != 0;
}

void Tag(uint64_t shader0, uint64_t shader1) {
	t_tag0 = shader0;
	t_tag1 = shader1;
}

void DescribeShader(const char* stage, const ShaderRecompiler::IR::CompiledShaderInfo& program,
                    uint32_t threads_per_group) {
	auto& s = *g_state;
	{
		std::lock_guard lock(s.described_mutex);
		if (!s.described.insert(program.shader_hash ^ (static_cast<uint64_t>(stage[0]) << 56)).second) {
			return;
		}
	}
	const auto& i = program.info;
	std::lock_guard lock(s.file_mutex);
	std::fprintf(s.shaders_file, "%016" PRIx64 ",%s,%d,%d,%d,%u,%u,%u,%zu,%zu,%zu,%d,%d\n",
	             program.shader_hash, stage, i.bvh ? 1 : 0, i.uses_lds ? 1 : 0,
	             i.function_lds ? 1 : 0, i.function_lds_dwords, program.wave_size, threads_per_group,
	             i.buffers.size(), i.images.size(), i.samplers.size(), i.uses_dma ? 1 : 0,
	             i.uses_gds ? 1 : 0);
	std::fflush(s.shaders_file);
}

void OnFlip() {
	g_state->frame.fetch_add(1, std::memory_order_relaxed);
}

void OnBegin(vk::CommandBuffer buffer) {
	auto& s = *g_state;
	auto  r = std::make_shared<Record>();
	r->cb     = buffer;
	r->serial = s.serial.fetch_add(1, std::memory_order_relaxed);
	r->frame  = s.frame.load(std::memory_order_relaxed);
	r->ops_timed = s.mode != Mode::CommandBuffers && r->frame >= s.start &&
	               ((r->frame - s.start) % s.every) < s.window;
	Stamp(buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, r->begin_slot);
	std::lock_guard lock(s.map_mutex);
	s.records[buffer] = std::move(r);
}

void OnSubmit(vk::CommandBuffer buffer, uint64_t tick, CommandScheduler& scheduler) {
	auto& s = *g_state;
	std::shared_ptr<Record> record;
	{
		std::lock_guard lock(s.map_mutex);
		const auto it = s.records.find(buffer);
		if (it == s.records.end()) {
			return;
		}
		record = std::move(it->second);
		s.records.erase(it);
	}
	record->tick = tick;
	{
		std::lock_guard lock(s.pending_mutex);
		s.pending.push_back(std::move(record));
	}
	// Completed command buffers are resolved here (ticks complete in order).
	for (;;) {
		std::shared_ptr<Record> done;
		{
			std::lock_guard lock(s.pending_mutex);
			if (s.pending.empty() || !scheduler.IsFree(s.pending.front()->tick)) {
				return;
			}
			done = std::move(s.pending.front());
			s.pending.pop_front();
		}
		Resolve(*done);
	}
}

} // namespace Libs::Graphics::GpuCost
