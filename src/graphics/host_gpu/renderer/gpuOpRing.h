#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUOPRING_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUOPRING_H_

// --debug-gpu-ops <ring|checkpoints>: a host-side record of the last draws and dispatches that were
// put into command buffers, with the timeline tick of the submission that carries them. When the
// GPU stops answering (hang watchdog report, a semaphore wait that lasts seconds, or a device loss)
// the ops of the submissions the GPU has not completed are printed, newest last: the hung work is
// among them. Pure logic, unit tested in tests/GpuOpRingTests.cpp. No GPU command is recorded, so
// the timing of a race is not disturbed; "checkpoints" additionally drops a
// VK_NV_device_diagnostic_checkpoints marker per op, whose value is the op's sequence number.
//
// Idea from Yotei Fork (KYTY_DEBUG_OP_RING, 5a1f94e86) and ogeidexe (5ca09b4b2, GPU checkpoints
// naming the draw), both GPL-2.0; re-implemented.

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace Libs::Graphics::GpuOps {

enum class Kind : uint8_t {
	Draw,             // n: prim, indexed, restart, index_count, instance_count, first_vertex, first_instance
	MeshDraw,         // as Draw, plus n[7..11]: groups, primitives/group, vertices/group, max prims, max verts
	Dispatch,         // n: group x, y, z, mode
	DispatchIndirect, // addr: args address; n: guest view of x, y, z, mode
};

struct Op {
	uint64_t seq       = 0; // 1, 2, 3 ... (assigned by Push)
	uint64_t tick      = 0; // timeline value the carrying submission signals
	uint64_t submit_id = 0;
	Kind     kind      = Kind::Draw;
	uint64_t shader0   = 0; // vertex / mesh / compute shader hash
	uint64_t shader1   = 0; // pixel shader hash
	uint64_t addr      = 0; // index buffer / args address
	uint32_t n[12]     = {};
};

[[nodiscard]] inline std::string Format(const Op& op) {
	char line[400];
	switch (op.kind) {
		case Kind::Draw:
		case Kind::MeshDraw:
			std::snprintf(line, sizeof(line),
			              "#%" PRIu64 " tick=%" PRIu64 " submit=%" PRIu64 " %s vs=%016" PRIx64
			              " ps=%016" PRIx64 " prim=%u indexed=%u restart=%u count=%u instances=%u"
			              " first=%u/%u index_addr=0x%" PRIx64,
			              op.seq, op.tick, op.submit_id, op.kind == Kind::Draw ? "draw" : "mesh-draw",
			              op.shader0, op.shader1, op.n[0], op.n[1], op.n[2], op.n[3], op.n[4], op.n[5],
			              op.n[6], op.addr);
			if (op.kind == Kind::MeshDraw) {
				const auto used = std::strlen(line);
				std::snprintf(line + used, sizeof(line) - used,
				              " groups=%u prims/group=%u verts/group=%u max_prims=%u max_verts=%u",
				              op.n[7], op.n[8], op.n[9], op.n[10], op.n[11]);
			}
			break;
		case Kind::Dispatch:
			std::snprintf(line, sizeof(line),
			              "#%" PRIu64 " tick=%" PRIu64 " submit=%" PRIu64 " dispatch cs=%016" PRIx64
			              " groups=%ux%ux%u mode=0x%x",
			              op.seq, op.tick, op.submit_id, op.shader0, op.n[0], op.n[1], op.n[2], op.n[3]);
			break;
		case Kind::DispatchIndirect:
			std::snprintf(line, sizeof(line),
			              "#%" PRIu64 " tick=%" PRIu64 " submit=%" PRIu64 " dispatch-indirect cs=%016" PRIx64
			              " args=0x%" PRIx64 " guest-view=%ux%ux%u mode=0x%x",
			              op.seq, op.tick, op.submit_id, op.shader0, op.addr, op.n[0], op.n[1], op.n[2],
			              op.n[3]);
			break;
	}
	return line;
}

class Ring {
public:
	explicit Ring(size_t capacity): m_ops(capacity == 0 ? 1 : capacity) {}

	// Returns the op's sequence number (never 0).
	uint64_t Push(Op op) {
		std::lock_guard lock(m_mutex);
		op.seq                    = ++m_next;
		m_ops[(op.seq - 1) % m_ops.size()] = op;
		return op.seq;
	}

	[[nodiscard]] uint64_t Count() const {
		std::lock_guard lock(m_mutex);
		return m_next;
	}

	// The ops of submissions the GPU has not completed (tick > completed_tick), oldest first. At most
	// `max` of them, keeping the newest.
	[[nodiscard]] std::vector<Op> InFlight(uint64_t completed_tick, size_t max) const {
		std::lock_guard lock(m_mutex);
		std::vector<Op> out;
		const uint64_t  oldest = m_next > m_ops.size() ? m_next - m_ops.size() + 1 : 1;
		for (uint64_t seq = m_next; seq >= oldest && seq != 0; seq--) {
			const auto& op = m_ops[(seq - 1) % m_ops.size()];
			if (op.tick <= completed_tick) {
				break; // ticks never go back: everything older is complete too
			}
			out.push_back(op);
			if (out.size() >= max) {
				break;
			}
		}
		std::reverse(out.begin(), out.end());
		return out;
	}

	// The op carrying a sequence number (a checkpoint marker), if it is still in the ring.
	[[nodiscard]] bool Find(uint64_t seq, Op& out) const {
		std::lock_guard lock(m_mutex);
		if (seq == 0 || seq > m_next || m_next - seq >= m_ops.size()) {
			return false;
		}
		out = m_ops[(seq - 1) % m_ops.size()];
		return true;
	}

	[[nodiscard]] std::string Report(uint64_t completed_tick, size_t max) const {
		const auto ops   = InFlight(completed_tick, max);
		std::string text = "gpu-ops: completed tick " + std::to_string(completed_tick) + ", " +
		                   std::to_string(ops.size()) + " op(s) of unfinished submissions (newest last)\n";
		for (const auto& op: ops) {
			text += "gpu-op: " + Format(op) + "\n";
		}
		return text;
	}

private:
	mutable std::mutex m_mutex;
	std::vector<Op>    m_ops;
	uint64_t           m_next = 0;
};

} // namespace Libs::Graphics::GpuOps

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUOPRING_H_
