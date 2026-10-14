#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"
#include "common/emulatorConfig.h"
#include "graphics/host_gpu/renderer/gpuOps.h"
#include "graphics/host_gpu/renderer/pollWait.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace Libs::Graphics {

MasterSemaphore::MasterSemaphore(GraphicContext& graphics): m_graphics(graphics) {
	vk::SemaphoreTypeCreateInfo type_info {};
	type_info.semaphoreType = vk::SemaphoreType::eTimeline;
	type_info.initialValue  = 0;

	vk::SemaphoreCreateInfo create_info {};
	create_info.pNext = &type_info;

	const auto result = m_graphics.device.createSemaphore(&create_info, nullptr, &m_semaphore);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_semaphore == nullptr);
}

MasterSemaphore::~MasterSemaphore() {
	if (m_semaphore != nullptr) {
		m_graphics.device.destroySemaphore(m_semaphore, nullptr);
	}
}

void MasterSemaphore::Refresh() {
	uint64_t   counter = 0;
	const auto result  = m_graphics.device.getSemaphoreCounterValue(m_semaphore, &counter);
	if (result == vk::Result::eErrorDeviceLost) {
		GpuOps::Report("device lost", m_gpu_tick.load(std::memory_order_acquire));
		EXIT("GPU device lost (timeline tick %" PRIu64 " completed, %" PRIu64 " submitted)\n",
		     m_gpu_tick.load(std::memory_order_acquire), CurrentTick() - 1);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	auto known = m_gpu_tick.load(std::memory_order_acquire);
	while (known < counter &&
	       !m_gpu_tick.compare_exchange_weak(known, counter, std::memory_order_release,
	                                         std::memory_order_relaxed)) {
	}
}

void MasterSemaphore::Wait(uint64_t tick) {
	if (IsFree(tick)) {
		return;
	}
	Refresh();
	if (IsFree(tick)) {
		return;
	}
	// --poll-before-wait: a small readback finishes within tens of microseconds; polling the
	// timeline value saves the kernel sleep and wake-up of a blocking wait.
	if (const auto budget_us = Config::PollBeforeWaitUs(); budget_us != 0) {
		const bool seen = PollUntil(
		    [&] {
			    Refresh();
			    return IsFree(tick);
		    },
		    [] {
			    return static_cast<uint64_t>(
			        std::chrono::duration_cast<std::chrono::nanoseconds>(
			            std::chrono::steady_clock::now().time_since_epoch()).count());
		    },
		    [] {
#if defined(__x86_64__)
			    _mm_pause();
#endif
		    },
		    uint64_t {budget_us} * 1000u);
		if (seen) {
			return;
		}
	}

	vk::SemaphoreWaitInfo wait_info {};
	wait_info.semaphoreCount = 1;
	wait_info.pSemaphores    = &m_semaphore;
	wait_info.pValues        = &tick;

	// Wait in one-second slices so that a lost device ends in a fatal error naming the wait instead
	// of a frozen emulator (an unbounded vkWaitSemaphores can stay blocked after the driver reset
	// the GPU), and so that a wait that lasts seconds can report the unfinished work
	// (--debug-gpu-ops). While the GPU is alive the wait is the same as an unbounded one.
	// Slice wait from ogeidexe (5ca09b4b2, GPL-2.0), re-implemented.
	constexpr uint64_t SliceNs        = 1'000'000'000ull;
	constexpr double   FirstReportSec = 5.0;
	const auto         start          = std::chrono::steady_clock::now();
	double             next_report    = FirstReportSec;
	for (;;) {
		const auto result = m_graphics.device.waitSemaphores(&wait_info, SliceNs);
		if (result == vk::Result::eSuccess) {
			break;
		}
		if (result == vk::Result::eErrorDeviceLost) {
			GpuOps::Report("device lost", m_gpu_tick.load(std::memory_order_acquire));
			EXIT("GPU device lost while waiting for timeline tick %" PRIu64 " (completed %" PRIu64
			     ", submitted %" PRIu64 ")\n",
			     tick, m_gpu_tick.load(std::memory_order_acquire), CurrentTick() - 1);
		}
		EXIT_NOT_IMPLEMENTED(result != vk::Result::eTimeout);
		const double waited =
		    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		if (waited >= next_report) {
			next_report *= 2.0;
			uint64_t counter = 0;
			(void)m_graphics.device.getSemaphoreCounterValue(m_semaphore, &counter);
			std::printf("GPU wait: timeline tick %" PRIu64 " not reached after %.0f s (counter %" PRIu64
			            ", submitted %" PRIu64 ")\n",
			            tick, waited, counter, CurrentTick() - 1);
			std::fflush(stdout);
			GpuOps::Report("wait stalled", counter);
		}
	}
	Refresh();
}

} // namespace Libs::Graphics
