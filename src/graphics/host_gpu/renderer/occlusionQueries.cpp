#include "graphics/host_gpu/renderer/occlusionQueries.h"

#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/occlusionCounters.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "kernel/memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdlib>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics::OcclusionQueries {

namespace {

constexpr uint32_t POOL_SLOTS = 16384;

class Host {
public:
	// Creates the query pool on first use; false if the device cannot back the counters.
	bool Initialize(GraphicContext& graphics) {
		if (m_ready.load(std::memory_order_acquire)) {
			return true;
		}
		std::lock_guard lock(m_init_mutex);
		if (m_state != State::Untried) {
			return m_state == State::Ready;
		}
		m_state = State::Unavailable;
		if (!Config::OcclusionQueriesEnabled()) {
			Say("Occlusion queries: off (--occlusion-queries false), every query reads visible\n");
			return false;
		}
		if (!graphics.host_query_reset_enabled) {
			Say("Occlusion queries: the device has no host query reset, every query reads visible\n");
			return false;
		}
		vk::QueryPoolCreateInfo info {};
		info.queryType  = vk::QueryType::eOcclusion;
		info.queryCount = POOL_SLOTS;
		if (graphics.device.createQueryPool(&info, nullptr, &m_pool) != vk::Result::eSuccess) {
			m_pool = nullptr;
			Say("Occlusion queries: no query pool, every query reads visible\n");
			return false;
		}
		graphics.device.resetQueryPool(m_pool, 0, POOL_SLOTS);
		m_device  = graphics.device;
		m_control = graphics.occlusion_query_precise_enabled ? vk::QueryControlFlagBits::ePrecise
		                                                     : vk::QueryControlFlags {};
		m_state   = State::Ready;
		m_ready.store(true, std::memory_order_release);
		Say("Occlusion queries: guest ZPASS_DONE counters follow host occlusion queries (%s)\n",
		    graphics.occlusion_query_precise_enabled ? "precise" : "boolean");
		return true;
	}

	[[nodiscard]] bool Ready() const noexcept { return m_ready.load(std::memory_order_acquire); }

	void Dump(CommandBuffer& command, uint64_t address) {
		auto& scheduler = command.GetContext().GetCommandScheduler();
		std::lock_guard lock(m_mutex);
		if (m_query_active) {
			EndQuery(command);
		}
		const auto dump = m_segments.OnDump(address);
		m_pending.push_back({scheduler.CurrentTick(), m_frame});
		scheduler.DeferPriorityOperation([this, dump] { Publish(dump); });
		if (command.IsRendering() && m_segments.Counting()) {
			BeginQuery(command);
		}
	}

	void OnBeginRendering(const CommandBuffer& command) {
		std::lock_guard lock(m_mutex);
		if (m_segments.Counting() && !m_query_active) {
			BeginQuery(command);
		}
	}

	void OnEndRendering(const CommandBuffer& command) {
		std::lock_guard lock(m_mutex);
		if (m_query_active) {
			EndQuery(command);
		}
	}

	void OnGuestFlip(CommandBuffer& command) {
		uint64_t wait_tick = 0;
		{
			std::lock_guard lock(m_mutex);
			// Dumps recorded in earlier frames must have reached guest memory by this flip.
			while (!m_pending.empty() && m_pending.front().frame < m_frame) {
				wait_tick = std::max(wait_tick, m_pending.front().tick);
				m_pending.pop_front();
			}
			m_frame++;
			m_segments.OnFrame();
		}
		if (wait_tick != 0) {
			auto& scheduler = command.GetContext().GetCommandScheduler();
			if (!scheduler.IsFree(wait_tick)) {
				m_waits.fetch_add(1, std::memory_order_relaxed);
			}
			scheduler.Wait(wait_tick);
			scheduler.WaitPriorityOperations(wait_tick);
		}
	}

private:
	enum class State { Untried, Unavailable, Ready };

	struct Pending {
		uint64_t tick  = 0;
		uint64_t frame = 0;
	};

	static void Say(const char* format, ...) __attribute__((format(printf, 1, 2)));

	void BeginQuery(const CommandBuffer& command) {
		uint32_t slot = 0;
		if (!m_segments.AllocateSlot(slot)) {
			m_ring_full.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		command.Handle().beginQuery(m_pool, slot, m_control);
		m_active_slot  = slot;
		m_query_active = true;
	}

	void EndQuery(const CommandBuffer& command) {
		command.Handle().endQuery(m_pool, m_active_slot);
		m_query_active = false;
	}

	// Runs on the priority thread once the GPU has executed the command buffer of the dump.
	void Publish(const OcclusionCounters::Segments::Dump& dump) {
		uint64_t samples = dump.uncounted ? OcclusionCounters::UncountedSamples : 0;
		for (uint64_t slot = dump.first_slot; slot < dump.end_slot;) {
			const auto index = static_cast<uint32_t>(slot % POOL_SLOTS);
			const auto count = static_cast<uint32_t>(
			    std::min<uint64_t>(dump.end_slot - slot, POOL_SLOTS - index));
			std::vector<uint64_t> results(count);
			const auto            result = m_device.getQueryPoolResults(
                m_pool, index, count, count * sizeof(uint64_t), results.data(), sizeof(uint64_t),
			    vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
			if (result == vk::Result::eSuccess) {
				for (uint32_t i = 0; i < count; i++) {
					samples += results[i];
				}
			} else {
				samples += OcclusionCounters::UncountedSamples;
			}
			m_device.resetQueryPool(m_pool, index, count);
			slot += count;
		}
		{
			std::lock_guard lock(m_mutex);
			m_segments.Release(dump.end_slot);
		}
		m_total += samples;
		m_dumps.fetch_add(1, std::memory_order_relaxed);
		NoteStats(dump.address);
		// DB 0 last: a reader that sees its ready bit finds the whole dump in place.
		for (uint32_t db = OcclusionCounters::DbCount; db-- > 0;) {
			const uint64_t value = OcclusionCounters::DbValue(db, m_total);
			(void)LibKernel::Memory::TryWriteBacking(dump.address + db * OcclusionCounters::DbStride,
			                                         &value, sizeof(value));
		}
	}

	// KYTY_OCCLUSION_STATS=1: every 10 s, how many begin/end pairs were read, how many saw no sample
	// (occluded) and the mean sample count of the others.
	void NoteStats(uint64_t address) {
		static const bool enabled = std::getenv("KYTY_OCCLUSION_STATS") != nullptr;
		if (!enabled) {
			return;
		}
		if (const auto begin = m_stat_begins.find(address - sizeof(uint64_t)); begin != m_stat_begins.end()) {
			const uint64_t diff = m_total - begin->second;
			m_stat_pairs++;
			m_stat_zero += diff == 0 ? 1 : 0;
			m_stat_samples += diff;
			m_stat_begins.erase(begin);
		} else {
			if (m_stat_begins.size() > 4096) {
				m_stat_begins.clear();
			}
			m_stat_begins[address] = m_total;
		}
		const auto now = std::chrono::steady_clock::now();
		if (now - m_stat_last > std::chrono::seconds(10)) {
			m_stat_last = now;
			std::printf("Occlusion queries: %" PRIu64 " pairs in 10 s, %" PRIu64 " read zero samples, mean %" PRIu64
			            " samples of the others, ring full %" PRIu64 ", flip waits %" PRIu64 "\n",
			            m_stat_pairs, m_stat_zero, m_stat_pairs > m_stat_zero ? m_stat_samples / (m_stat_pairs - m_stat_zero) : 0,
			            m_ring_full.load(), m_waits.load());
			std::fflush(stdout);
			m_stat_pairs = m_stat_zero = m_stat_samples = 0;
		}
	}

	std::unordered_map<uint64_t, uint64_t> m_stat_begins;
	uint64_t                               m_stat_pairs = 0, m_stat_zero = 0, m_stat_samples = 0;
	std::chrono::steady_clock::time_point  m_stat_last = std::chrono::steady_clock::now();

	std::mutex                         m_init_mutex;
	std::atomic<bool>                  m_ready {false};
	State                              m_state = State::Untried;
	vk::Device                         m_device = nullptr;
	vk::QueryPool                      m_pool   = nullptr;
	vk::QueryControlFlags              m_control;

	std::mutex                         m_mutex; // the segment bookkeeping and the active query
	OcclusionCounters::Segments        m_segments {POOL_SLOTS};
	std::deque<Pending>                m_pending;
	bool                               m_query_active = false;
	uint32_t                           m_active_slot  = 0;
	uint64_t                           m_frame        = 0;

	uint64_t                           m_total = 0; // priority thread only
	std::atomic<uint64_t>              m_dumps {0};
	std::atomic<uint64_t>              m_waits {0};
	std::atomic<uint64_t>              m_ring_full {0};
};

void Host::Say(const char* format, ...) {
	char    text[256];
	va_list args;
	va_start(args, format);
	std::vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	Log::WriteToConsoleAndLog(std::string(text));
}

Host& Instance() {
	// Leaked on purpose: priority operations may still publish results while the process exits.
	static auto* host = new Host;
	return *host;
}

} // namespace

bool Dump(CommandBuffer& command, uint64_t address) {
	auto& host = Instance();
	// A dump with no command buffer being recorded (a packet decoded outside a submission) cannot record
	// a query: it publishes the visible count.
	if (address == 0 || (address & 7u) != 0 ||
	    !command.GetContext().GetCommandScheduler().Active() ||
	    !host.Initialize(command.GetGraphics())) {
		return false;
	}
	host.Dump(command, address);
	return true;
}

void OnBeginRendering(const CommandBuffer& command) {
	auto& host = Instance();
	if (host.Ready()) {
		host.OnBeginRendering(command);
	}
}

void OnEndRendering(const CommandBuffer& command) {
	auto& host = Instance();
	if (host.Ready()) {
		host.OnEndRendering(command);
	}
}

void OnGuestFlip(CommandBuffer& command) {
	auto& host = Instance();
	if (host.Ready()) {
		host.OnGuestFlip(command);
	}
}

} // namespace Libs::Graphics::OcclusionQueries
