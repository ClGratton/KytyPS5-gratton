#include "graphics/host_gpu/renderer/computeGpuTimes.h"

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace Libs::Graphics::ComputeGpuTimes {

namespace {

constexpr uint32_t PAIRS = 2048;

uint64_t NowMs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Key {
	uint64_t hash;
	uint32_t x, y, z;
	bool     generic;
	bool     operator<(const Key& o) const {
		return std::tie(hash, x, y, z, generic) < std::tie(o.hash, o.x, o.y, o.z, o.generic);
	}
};

struct Stats {
	uint64_t count    = 0;
	double   total_ns = 0;
	double   max_ns   = 0;
};

class Recorder {
public:
	bool Initialize(GraphicContext& graphics) {
		if (m_ready.load(std::memory_order_acquire)) return true;
		std::lock_guard lock(m_init_mutex);
		if (m_tried) return m_pool != nullptr;
		m_tried = true;
		if (!graphics.host_query_reset_enabled ||
		    graphics.physical_device_properties.limits.timestampComputeAndGraphics == VK_FALSE) {
			Log::WriteToConsoleAndLog("Compute GPU times: timestamps are unavailable on this device\n");
			return false;
		}
		const auto families = graphics.physical_device.getQueueFamilyProperties();
		if (graphics.queue_family >= families.size() ||
		    families[graphics.queue_family].timestampValidBits == 0) {
			Log::WriteToConsoleAndLog("Compute GPU times: the queue family has no timestamps\n");
			return false;
		}
		const uint32_t bits = families[graphics.queue_family].timestampValidBits;
		m_mask              = bits >= 64 ? ~0ull : ((1ull << bits) - 1ull);
		vk::QueryPoolCreateInfo info {};
		info.queryType  = vk::QueryType::eTimestamp;
		info.queryCount = PAIRS * 2;
		if (graphics.device.createQueryPool(&info, nullptr, &m_pool) != vk::Result::eSuccess) {
			m_pool = nullptr;
			return false;
		}
		graphics.device.resetQueryPool(m_pool, 0, PAIRS * 2);
		m_device      = graphics.device;
		m_ns_per_tick = static_cast<double>(graphics.physical_device_properties.limits.timestampPeriod);
		m_busy        = std::make_unique<std::atomic<uint8_t>[]>(PAIRS);
		m_last_report = NowMs();
		m_ready.store(true, std::memory_order_release);
		Log::WriteToConsoleAndLog("Compute GPU times: on\n");
		return true;
	}

	bool Acquire(uint32_t& pair) {
		const auto candidate = m_next.fetch_add(1, std::memory_order_relaxed) % PAIRS;
		if (m_busy[candidate].exchange(1u, std::memory_order_acq_rel) != 0u) {
			m_dropped.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		pair = candidate;
		return true;
	}

	void Stamp(CommandBuffer& command, uint32_t query) {
		command.Handle().writeTimestamp(vk::PipelineStageFlagBits::eComputeShader, m_pool, query);
	}

	void Resolve(uint32_t pair, const Key& key) {
		uint64_t   ticks[2] = {};
		const auto result   = m_device.getQueryPoolResults(m_pool, pair * 2, 2, sizeof(ticks), ticks,
		                                                   sizeof(uint64_t),
		                                                   vk::QueryResultFlagBits::e64);
		m_device.resetQueryPool(m_pool, pair * 2, 2);
		m_busy[pair].store(0u, std::memory_order_release);
		if (result != vk::Result::eSuccess) return;
		const double ns = static_cast<double>((ticks[1] - ticks[0]) & m_mask) * m_ns_per_tick;
		bool         report = false;
		{
			std::lock_guard lock(m_table_mutex);
			auto&           s = m_table[key];
			s.count++;
			s.total_ns += ns;
			s.max_ns = std::max(s.max_ns, ns);
			const auto now = NowMs();
			if (now - m_last_report >= static_cast<uint64_t>(Config::GetComputeGpuTimesSeconds()) * 1000u) {
				m_last_report = now;
				report        = true;
			}
		}
		if (report) Report();
	}

private:
	void Report() {
		std::vector<std::pair<Key, Stats>> rows;
		{
			std::lock_guard lock(m_table_mutex);
			rows.assign(m_table.begin(), m_table.end());
		}
		std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
			return a.second.total_ns > b.second.total_ns;
		});
		double total = 0;
		uint64_t n   = 0;
		for (const auto& r: rows) {
			total += r.second.total_ns;
			n += r.second.count;
		}
		if (FILE* f = std::fopen("_compute_gpu_times.csv", "w")) {
			std::fprintf(f, "hash,groups_x,groups_y,groups_z,generic,count,total_ms,mean_us,max_us\n");
			for (const auto& r: rows) {
				std::fprintf(f, "%016" PRIx64 ",%u,%u,%u,%d,%" PRIu64 ",%.3f,%.2f,%.2f\n", r.first.hash,
				             r.first.x, r.first.y, r.first.z, r.first.generic ? 1 : 0, r.second.count,
				             r.second.total_ns / 1e6, r.second.total_ns / static_cast<double>(r.second.count) / 1e3,
				             r.second.max_ns / 1e3);
			}
			std::fclose(f);
		}
		std::string text = "Compute GPU times: " + std::to_string(n) + " dispatches, " +
		                   std::to_string(total / 1e6) + " ms in total, dropped " +
		                   std::to_string(m_dropped.load()) + "\n";
		for (size_t i = 0; i < rows.size() && i < 8; i++) {
			char line[256];
			std::snprintf(line, sizeof(line),
			              "  cs %016" PRIx64 " %ux%ux%u%s: %" PRIu64 " x %.1f us (total %.1f ms)\n",
			              rows[i].first.hash, rows[i].first.x, rows[i].first.y, rows[i].first.z,
			              rows[i].first.generic ? " generic" : "", rows[i].second.count,
			              rows[i].second.total_ns / static_cast<double>(rows[i].second.count) / 1e3,
			              rows[i].second.total_ns / 1e6);
			text += line;
		}
		Log::WriteToConsoleAndLog(text);
	}

	std::mutex                              m_init_mutex;
	bool                                    m_tried = false;
	std::atomic<bool>                       m_ready {false};
	vk::Device                              m_device {};
	vk::QueryPool                           m_pool {};
	double                                  m_ns_per_tick = 1.0;
	uint64_t                                m_mask        = ~0ull;
	std::unique_ptr<std::atomic<uint8_t>[]> m_busy;
	std::atomic<uint32_t>                   m_next {0};
	std::atomic<uint64_t>                   m_dropped {0};
	std::mutex                              m_table_mutex;
	std::map<Key, Stats>                    m_table;
	uint64_t                                m_last_report = 0;
};

Recorder& Get() {
	// Leaked on purpose: priority operations may still resolve queries while the process exits.
	static auto* recorder = new Recorder;
	return *recorder;
}

} // namespace

Scope Begin(CommandBuffer& command) {
	Scope scope;
	if (Config::GetComputeGpuTimesSeconds() == 0 || !Get().Initialize(command.GetGraphics())) {
		return scope;
	}
	if (!Get().Acquire(scope.slot)) return scope;
	Get().Stamp(command, scope.slot * 2);
	scope.active = true;
	return scope;
}

void End(CommandBuffer& command, const Scope& scope, uint64_t shader_hash, uint32_t groups_x,
         uint32_t groups_y, uint32_t groups_z, bool generic) {
	if (!scope.active) return;
	Get().Stamp(command, scope.slot * 2 + 1);
	const Key key {shader_hash, groups_x, groups_y, groups_z, generic};
	const auto pair = scope.slot;
	command.GetContext().GetCommandScheduler().DeferPriorityOperation(
	    [pair, key] { Get().Resolve(pair, key); });
}

} // namespace Libs::Graphics::ComputeGpuTimes
