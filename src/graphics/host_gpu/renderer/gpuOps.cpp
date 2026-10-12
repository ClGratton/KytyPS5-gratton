#include "graphics/host_gpu/renderer/gpuOps.h"

#include "common/hangWatchdog.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include <cstdio>
#include <memory>
#include <mutex>

namespace Libs::Graphics::GpuOps {

std::atomic<bool> g_enabled {false};

namespace {

constexpr size_t RingSize     = 1u << 15;
constexpr size_t ReportOps    = 96;
constexpr size_t CheckpointOps = 6;

struct State {
	std::unique_ptr<Ring> ring;
	GraphicContext*       graphics    = nullptr;
	MasterSemaphore*      master      = nullptr;
	bool                  checkpoints = false;
};

State& GetState() {
	static State state;
	return state;
}

void Emit(const std::string& text) {
	std::fputs(text.c_str(), stdout);
	std::fflush(stdout);
	LOGF("%s", text.c_str());
}

std::string CheckpointReport(State& s) {
	std::string text;
	if (!s.checkpoints) {
		return text;
	}
	uint32_t count = 0;
	s.graphics->queue.getCheckpointDataNV(&count, nullptr);
	std::vector<vk::CheckpointDataNV> data(count);
	s.graphics->queue.getCheckpointDataNV(&count, data.data());
	text += "gpu-ops: " + std::to_string(count) + " NV checkpoint(s) reached by the queue\n";
	for (uint32_t i = 0; i < count; i++) {
		const auto seq = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(data[i].pCheckpointMarker));
		char       head[160];
		std::snprintf(head, sizeof(head), "gpu-ops:   stage=%s marker=#%llu\n",
		              vk::to_string(data[i].stage).c_str(), static_cast<unsigned long long>(seq));
		text += head;
		Op op;
		if (s.ring->Find(seq, op)) {
			text += "gpu-ops:     " + Format(op) + "\n";
			for (uint64_t k = 1; k <= CheckpointOps; k++) {
				if (s.ring->Find(seq + k, op)) {
					text += "gpu-ops:       next " + Format(op) + "\n";
				}
			}
		}
	}
	return text;
}

} // namespace

void Initialize(GraphicContext& graphics, MasterSemaphore& master, uint32_t mode) {
	auto& s = GetState();
	if (mode == 0 || s.ring) {
		return;
	}
	s.ring        = std::make_unique<Ring>(RingSize);
	s.graphics    = &graphics;
	s.master      = &master;
	s.checkpoints = mode >= 2 && graphics.op_checkpoints_enabled &&
	                VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdSetCheckpointNV != nullptr;
	g_enabled.store(true, std::memory_order_release);
	std::printf("GPU ops: ring of %zu draws/dispatches on, NV checkpoints %s\n", RingSize,
	            s.checkpoints ? "on" : "off");
	Common::Hang::RegisterProvider("gpu_ops", [] {
		auto& st = GetState();
		uint64_t counter = 0;
		const auto result = st.graphics->device.getSemaphoreCounterValue(st.master->Handle(), &counter);
		if (result != vk::Result::eSuccess) {
			return std::string("getSemaphoreCounterValue: ") + vk::to_string(result) + "\n" +
			       st.ring->Report(st.master->KnownGpuTick(), ReportOps) +
			       CheckpointReport(st);
		}
		return "timeline counter " + std::to_string(counter) + ", current tick " +
		       std::to_string(st.master->CurrentTick()) + "\n" + st.ring->Report(counter, ReportOps) +
		       CheckpointReport(st);
	});
}

void Record(vk::CommandBuffer command, Op op, uint64_t tick) {
	auto& s = GetState();
	if (!s.ring) {
		return;
	}
	op.tick      = tick;
	const auto seq = s.ring->Push(op);
	if (s.checkpoints && command) {
		command.setCheckpointNV(reinterpret_cast<const void*>(static_cast<uintptr_t>(seq)));
	}
}

void Report(const char* why, uint64_t completed_tick) {
	auto& s = GetState();
	if (!s.ring) {
		return;
	}
	std::string text = std::string("gpu-ops: report (") + why + ")\n" + s.ring->Report(completed_tick, ReportOps);
	text += CheckpointReport(s);
	Emit(text);
}

} // namespace Libs::Graphics::GpuOps
