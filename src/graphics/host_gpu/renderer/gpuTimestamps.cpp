#include "graphics/host_gpu/renderer/gpuTimestamps.h"

#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/framePath.h"
#include "common/logging/log.h"
#include "common/timer.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/gpuClock.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/sync.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace Libs::Graphics::GpuTimestamps {

namespace {

// Time scale in the parse-time domain: values are the reference clock at the moment the emulator
// parses the packet, boundaries are the parse-time flips.
GpuClock::TimeScaler& ParseTimeScaler() {
	static GpuClock::TimeScaler scaler(Config::GetGpuTimeScale());
	return scaler;
}

// Time scale in the GPU-execution domain: values are the reference clock at the moment the GPU
// reached the timestamp, boundaries are the GPU times of the flips.
GpuClock::TimeScaler& GpuTimeScaler() {
	static GpuClock::TimeScaler scaler(Config::GetGpuTimeScale());
	return scaler;
}

// Status lines are printed to the console and the log whatever the printf direction is: which
// source the guest timestamps come from is something a benchmark needs to see.
void Say(const char* format, ...) {
	char    text[512];
	va_list args;
	va_start(args, format);
	std::vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	Log::WriteToConsoleAndLog(std::string(text));
}

constexpr uint32_t QUERY_SLOTS         = 4096;
constexpr uint64_t RECALIBRATE_SECONDS = 1;

// Host GPU timestamps: a ring of timestamp queries. Each guest timestamp takes a slot, records a
// query at its point of the command stream, and a priority operation reads the value when the
// command buffer has executed, rewrites the guest memory and resets the slot on the host
// (hostQueryReset), so recording needs no reset command inside a render pass.
class HostTimestamps {
public:
	// Prepares the query pool and the clock calibration; false if the device cannot do it.
	bool Initialize(GraphicContext& graphics) {
		if (m_ready.load(std::memory_order_acquire)) {
			return true;
		}
		std::lock_guard lock(m_init_mutex);
		if (m_state != State::Untried) {
			return m_state == State::Ready;
		}
		m_state = State::Unavailable;
		if (!graphics.calibrated_timestamps_enabled || !graphics.host_query_reset_enabled ||
		    graphics.physical_device_properties.limits.timestampComputeAndGraphics == VK_FALSE) {
			Say("GPU timestamps: unsupported on this device, using the legacy clock\n");
			return false;
		}
		const auto families = graphics.physical_device.getQueueFamilyProperties();
		if (graphics.queue_family >= families.size() ||
		    families[graphics.queue_family].timestampValidBits == 0) {
			Say("GPU timestamps: the queue family has no timestamps, using the legacy clock\n");
			return false;
		}
		m_valid_bits = families[graphics.queue_family].timestampValidBits;

		m_get_calibrated = reinterpret_cast<PFN_vkGetCalibratedTimestampsEXT>(
		    graphics.device.getProcAddr("vkGetCalibratedTimestampsEXT"));
		const auto get_domains = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT>(
		    graphics.instance.getProcAddr("vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"));
		if (m_get_calibrated == nullptr || get_domains == nullptr) {
			Say("GPU timestamps: calibrated timestamps are missing, using the legacy clock\n");
			return false;
		}
		uint32_t count = 0;
		if (get_domains(graphics.physical_device, &count, nullptr) != VK_SUCCESS) {
			return false;
		}
		std::vector<VkTimeDomainEXT> domains(count);
		if (get_domains(graphics.physical_device, &count, domains.data()) < VK_SUCCESS) {
			return false;
		}
		const auto has = [&](VkTimeDomainEXT domain) {
			return std::find(domains.begin(), domains.end(), domain) != domains.end();
		};
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		m_host_domain = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT;
#else
		// Common::Timer::QueryPerformanceCounter is clock_gettime(CLOCK_MONOTONIC) on Linux.
		m_host_domain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT;
#endif
		if (!has(VK_TIME_DOMAIN_DEVICE_EXT) || !has(m_host_domain)) {
			Say("GPU timestamps: no usable time domain pair, using the legacy clock\n");
			return false;
		}

		vk::QueryPoolCreateInfo info {};
		info.queryType  = vk::QueryType::eTimestamp;
		info.queryCount = QUERY_SLOTS;
		if (graphics.device.createQueryPool(&info, nullptr, &m_pool) != vk::Result::eSuccess) {
			m_pool = nullptr;
			return false;
		}
		graphics.device.resetQueryPool(m_pool, 0, QUERY_SLOTS);
		m_device       = graphics.device;
		m_ns_per_tick  = static_cast<double>(graphics.physical_device_properties.limits.timestampPeriod);
		m_busy         = std::make_unique<std::atomic<uint8_t>[]>(QUERY_SLOTS);
		if (!Calibrate()) {
			graphics.device.destroyQueryPool(m_pool, nullptr);
			m_pool = nullptr;
			Say("GPU timestamps: calibration failed, using the legacy clock\n");
			return false;
		}
		m_state = State::Ready;
		m_ready.store(true, std::memory_order_release);
		Say("GPU timestamps: guest timestamps follow GPU execution (%.3f ns per tick, %u valid "
		     "bits)\n",
		     m_ns_per_tick, m_valid_bits);
		return true;
	}

	// Takes a query slot; false when every slot still waits to be read (the guest then keeps
	// the parse-time value for this timestamp).
	bool Acquire(uint32_t& slot) {
		const auto candidate = m_next.fetch_add(1, std::memory_order_relaxed) % QUERY_SLOTS;
		if (m_busy[candidate].exchange(1u, std::memory_order_acq_rel) != 0u) {
			m_ring_full.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		slot = candidate;
		return true;
	}

	void Record(CommandBuffer& command, Point point, uint32_t slot) {
		command.Handle().writeTimestamp(point == Point::BottomOfPipe
		                                    ? vk::PipelineStageFlagBits::eBottomOfPipe
		                                    : vk::PipelineStageFlagBits::eTopOfPipe,
		                                m_pool, slot);
	}

	// Reads and releases a slot. The GPU time as a reference clock value, if it is available.
	bool Resolve(uint32_t slot, uint64_t& reference) {
		uint64_t   ticks  = 0;
		const auto result = m_device.getQueryPoolResults(
		    m_pool, slot, 1, sizeof(ticks), &ticks, sizeof(ticks), vk::QueryResultFlagBits::e64);
		const bool ok = result == vk::Result::eSuccess && ToReference(ticks, reference);
		m_device.resetQueryPool(m_pool, slot, 1);
		m_busy[slot].store(0u, std::memory_order_release);
		if (!ok) {
			m_unavailable.fetch_add(1, std::memory_order_relaxed);
		}
		return ok;
	}

	// KYTY_FRAME_PATH: reads a slot without waiting; the GPU time as host CLOCK_MONOTONIC ns.
	// Returns 0 = not available yet (slot kept), 1 = ok (slot released), 2 = failed (slot released).
	int ResolveHostNs(uint32_t slot, uint64_t& host_ns) {
		uint64_t   ticks  = 0;
		const auto result = m_device.getQueryPoolResults(
		    m_pool, slot, 1, sizeof(ticks), &ticks, sizeof(ticks), vk::QueryResultFlagBits::e64);
		if (result == vk::Result::eNotReady) {
			return 0;
		}
		int status = 2;
		if (result == vk::Result::eSuccess) {
			std::lock_guard lock(m_fp_mutex);
			const uint64_t  now = Common::FramePath::NowNs();
			if (m_fp_host_ns == 0 || now - m_fp_calibrated_ns > 500'000'000ull) {
				VkCalibratedTimestampInfoEXT infos[2] {};
				infos[0].sType      = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT;
				infos[0].timeDomain = VK_TIME_DOMAIN_DEVICE_EXT;
				infos[1].sType      = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT;
				infos[1].timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT;
				uint64_t stamps[2]  = {};
				uint64_t deviation  = 0;
				if (m_get_calibrated(m_device, 2, infos, stamps, &deviation) == VK_SUCCESS) {
					m_fp_device_ticks  = stamps[0];
					m_fp_host_ns       = stamps[1];
					m_fp_calibrated_ns = now;
				}
			}
			if (m_fp_host_ns != 0) {
				const double delta = static_cast<double>(static_cast<int64_t>(ticks - m_fp_device_ticks)) *
				                     m_ns_per_tick;
				host_ns = static_cast<uint64_t>(static_cast<int64_t>(m_fp_host_ns) +
				                                static_cast<int64_t>(delta));
				status  = 1;
			}
		}
		m_device.resetQueryPool(m_pool, slot, 1);
		m_busy[slot].store(0u, std::memory_order_release);
		return status;
	}

	void NoteLag(int64_t ticks) {
		m_lag_sum.fetch_add(ticks, std::memory_order_relaxed);
		const auto n = m_lag_count.fetch_add(1, std::memory_order_relaxed) + 1;
		if (n % 8192 == 0) {
			Say("GPU timestamps: %llu rewritten, mean GPU-time lag behind parse time %.3f ms, "
			     "ring full %llu, unavailable %llu\n",
			     static_cast<unsigned long long>(n),
			     static_cast<double>(m_lag_sum.load(std::memory_order_relaxed)) /
			         static_cast<double>(n) / (GpuClock::REFERENCE_FREQUENCY / 1000.0),
			     static_cast<unsigned long long>(m_ring_full.load(std::memory_order_relaxed)),
			     static_cast<unsigned long long>(m_unavailable.load(std::memory_order_relaxed)));
		}
	}

private:
	enum class State { Untried, Unavailable, Ready };

	// A calibration pair: a device timestamp and the host TSC at the same moment.
	bool Calibrate() {
		VkCalibratedTimestampInfoEXT infos[2] {};
		infos[0].sType      = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT;
		infos[0].timeDomain = VK_TIME_DOMAIN_DEVICE_EXT;
		infos[1].sType      = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT;
		infos[1].timeDomain = m_host_domain;
		uint64_t stamps[2]  = {};
		uint64_t deviation  = 0;
		if (m_get_calibrated(m_device, 2, infos, stamps, &deviation) != VK_SUCCESS) {
			return false;
		}
		const auto tsc_frequency = LibKernel::KernelGetTscFrequency();
		const auto host_now      = Common::Timer::QueryPerformanceCounter();
		const auto tsc_now       = LibKernel::KernelReadTsc();
		const auto host_frequency = Common::Timer::QueryPerformanceFrequency();
		if (tsc_frequency == 0 || host_frequency == 0) {
			return false;
		}
		const auto to_ns = [host_frequency](uint64_t ticks) {
			return host_frequency == 1000000000ull
			           ? ticks
			           : static_cast<uint64_t>(static_cast<double>(ticks) * 1e9 /
			                                   static_cast<double>(host_frequency));
		};
		GpuClock::DeviceCalibration c;
		c.device_ticks  = stamps[0];
		c.ns_per_tick   = m_ns_per_tick;
		c.valid_bits    = m_valid_bits;
		c.tsc_frequency = tsc_frequency;
		c.host_tsc      = GpuClock::HostSampleToTsc(to_ns(stamps[1]), to_ns(host_now), tsc_now,
		                                            tsc_frequency);
		m_calibration    = c;
		m_calibrated_tsc = tsc_now;
		return true;
	}

	bool ToReference(uint64_t ticks, uint64_t& reference) {
		GpuClock::DeviceCalibration c;
		{
			std::lock_guard lock(m_calibration_mutex);
			const auto now = LibKernel::KernelReadTsc();
			if (now - m_calibrated_tsc > RECALIBRATE_SECONDS * m_calibration.tsc_frequency) {
				(void)Calibrate(); // keeps the old pair if this fails
			}
			c = m_calibration;
		}
		uint64_t tsc = 0;
		return GpuClock::DeviceTicksToTsc(c, ticks, tsc) &&
		       GpuClock::TscToReferenceClock(tsc, c.tsc_frequency, reference);
	}

	std::mutex                                  m_init_mutex;
	std::atomic<bool>                           m_ready {false};
	State                                       m_state       = State::Untried;
	vk::Device                                  m_device      = nullptr;
	vk::QueryPool                               m_pool        = nullptr;
	PFN_vkGetCalibratedTimestampsEXT            m_get_calibrated = nullptr;
	VkTimeDomainEXT                             m_host_domain = VK_TIME_DOMAIN_DEVICE_EXT;
	double                                      m_ns_per_tick = 1.0;
	uint32_t                                    m_valid_bits  = 64;
	std::unique_ptr<std::atomic<uint8_t>[]>     m_busy;
	std::atomic<uint32_t>                       m_next {0};
	std::mutex                                  m_calibration_mutex;
	GpuClock::DeviceCalibration                 m_calibration;
	uint64_t                                    m_calibrated_tsc = 0;
	std::atomic<int64_t>                        m_lag_sum {0};
	std::atomic<uint64_t>                       m_lag_count {0};
	std::atomic<uint64_t>                       m_ring_full {0};
	std::atomic<uint64_t>                       m_unavailable {0};
	std::mutex                                  m_fp_mutex;
	uint64_t                                    m_fp_device_ticks  = 0;
	uint64_t                                    m_fp_host_ns       = 0;
	uint64_t                                    m_fp_calibrated_ns = 0;
};

HostTimestamps& Host() {
	// Leaked on purpose: priority operations may still resolve queries while the process exits.
	static auto* host = new HostTimestamps;
	return *host;
}

bool GpuModeAvailable(CommandBuffer& command) {
	return Config::GetGpuTimestampMode() == Config::GpuTimestampMode::Gpu &&
	       Host().Initialize(command.GetGraphics());
}

void StoreGuestValue(void* dst, uint32_t size, uint64_t value) {
	const auto low = static_cast<uint32_t>(value);
	(void)LibKernel::Memory::TryWriteBacking(reinterpret_cast<uint64_t>(dst),
	                                         size == sizeof(uint32_t)
	                                             ? static_cast<const void*>(&low)
	                                             : static_cast<const void*>(&value),
	                                         size);
}

} // namespace

uint64_t SampleGuestClock(CommandBuffer& command, void* dst, uint32_t size, Point point) {
	static std::once_flag announced;
	std::call_once(announced, [] {
		Say("Guest GPU timestamps: %s, time scale %.2f\n",
		    Config::GetGpuTimestampMode() == Config::GpuTimestampMode::Gpu ? "gpu" : "legacy",
		    Config::GetGpuTimeScale());
	});
	const auto parse_time = Sync::ReadReferenceClock();
	const auto immediate  = ParseTimeScaler().Apply(parse_time);
	uint32_t   slot       = 0;
	if (dst == nullptr || (size != sizeof(uint32_t) && size != sizeof(uint64_t)) ||
	    !GpuModeAvailable(command) || !Host().Acquire(slot)) {
		return immediate;
	}
	Host().Record(command, point, slot);
	command.GetContext().GetCommandScheduler().DeferPriorityOperation(
	    [slot, dst, size, parse_time] {
		    uint64_t reference = 0;
		    if (Host().Resolve(slot, reference)) {
			    Host().NoteLag(static_cast<int64_t>(reference) - static_cast<int64_t>(parse_time));
			    StoreGuestValue(dst, size, GpuTimeScaler().Apply(reference));
		    }
	    });
	return immediate;
}

void OnGuestFlip(CommandBuffer& command) {
	ParseTimeScaler().AddBoundary(Sync::ReadReferenceClock());
	uint32_t slot = 0;
	if (!GpuModeAvailable(command) || !Host().Acquire(slot)) {
		return;
	}
	Host().Record(command, Point::BottomOfPipe, slot);
	command.GetContext().GetCommandScheduler().DeferPriorityOperation([slot] {
		uint64_t reference = 0;
		if (Host().Resolve(slot, reference)) {
			GpuTimeScaler().AddBoundary(reference);
		}
	});
}

namespace {
struct FpPending {
	uint64_t tick;
	uint32_t begin_slot;
	uint32_t end_slot;
};
std::mutex            g_fp_mutex;
std::vector<FpPending> g_fp_pending;
} // namespace

uint32_t FramePathStamp(CommandBuffer& command, Point point) {
	uint32_t slot = 0;
	if (!Common::FramePath::On() || !GpuModeAvailable(command) || !Host().Acquire(slot)) {
		return kNoSlot;
	}
	Host().Record(command, point, slot);
	return slot;
}

void FramePathCommit(uint64_t tick, uint32_t begin_slot, uint32_t end_slot) {
	if (begin_slot == kNoSlot || end_slot == kNoSlot) {
		// A half-recorded pair cannot be read: leave the other slot to the ring (it is reset when reused).
		return;
	}
	std::lock_guard lock(g_fp_mutex);
	g_fp_pending.push_back({tick, begin_slot, end_slot});
}

void FramePathResolve() {
	std::lock_guard lock(g_fp_mutex);
	size_t          done = 0;
	for (; done < g_fp_pending.size(); done++) {
		const auto& p = g_fp_pending[done];
		uint64_t    end_ns = 0, begin_ns = 0;
		const int   end = Host().ResolveHostNs(p.end_slot, end_ns);
		if (end == 0) {
			break; // the GPU has not got there; later buffers are not ready either
		}
		const int begin = Host().ResolveHostNs(p.begin_slot, begin_ns);
		if (end == 1 && begin == 1 && end_ns >= begin_ns) {
			Common::FramePath::Log(Common::FramePath::GpuExec, begin_ns, end_ns - begin_ns, p.tick, 0);
		}
	}
	g_fp_pending.erase(g_fp_pending.begin(), g_fp_pending.begin() + static_cast<ptrdiff_t>(done));
}

} // namespace Libs::Graphics::GpuTimestamps
