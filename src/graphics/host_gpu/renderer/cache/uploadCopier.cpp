#include "graphics/host_gpu/renderer/cache/uploadCopier.h"

#include "common/profiler.h"

#include "kernel/memory.h"

#include <cstring>
#include <emmintrin.h>

namespace Libs::Graphics {

namespace {
std::atomic<UploadCopier*> g_instance {nullptr};
std::mutex                 g_instance_mutex;
} // namespace

UploadCopier& UploadCopier::Get() {
	if (auto* existing = g_instance.load(std::memory_order_acquire)) {
		return *existing;
	}
	std::lock_guard lock(g_instance_mutex);
	if (auto* existing = g_instance.load(std::memory_order_acquire)) {
		return *existing;
	}
	auto* created = new UploadCopier(); // leaked: helpers may still be parked at exit
	g_instance.store(created, std::memory_order_release);
	return *created;
}

void UploadCopier::FenceIfCreated() {
	if (auto* existing = g_instance.load(std::memory_order_acquire)) {
		existing->Fence();
	}
}

UploadCopier::UploadCopier() {
	for (size_t i = 0; i < kThreads; ++i) {
		m_threads.emplace_back([this] { Run(); });
		m_threads.back().detach();
	}
}

void UploadCopier::Submit(const Item* items, size_t count) {
	{
		std::lock_guard lock(m_mutex);
		for (size_t i = 0; i < count; ++i) {
			m_queue.push_back(items[i]);
		}
	}
	m_pending.fetch_add(static_cast<uint32_t>(count), std::memory_order_release);
	m_submitted.fetch_add(count, std::memory_order_relaxed);
	if (m_sleepers.load(std::memory_order_acquire) != 0) {
		m_wake.notify_all();
	}
}

void UploadCopier::Run() {
	KYTY_PROFILER_THREAD("kyUpload"); // not the creator's name: the bench reads the GPU thread's CPU by name
	for (;;) {
		Item item;
		bool got = false;
		{
			std::lock_guard lock(m_mutex);
			if (!m_queue.empty()) {
				item = m_queue.front();
				m_queue.pop_front();
				m_pending.fetch_sub(1, std::memory_order_relaxed);
				got = true;
			}
		}
		if (!got) {
			// The next batch usually follows within a few microseconds: spin before going to sleep.
			for (int i = 0; i < 20000 && m_pending.load(std::memory_order_acquire) == 0; ++i) {
				_mm_pause();
			}
			if (m_pending.load(std::memory_order_acquire) == 0) {
				std::unique_lock lock(m_mutex);
				m_sleepers.fetch_add(1, std::memory_order_acq_rel);
				m_wake.wait(lock, [this] { return !m_queue.empty(); });
				m_sleepers.fetch_sub(1, std::memory_order_acq_rel);
			}
			continue;
		}
		const bool ok = LibKernel::Memory::TryReadBacking(item.guest_address, item.destination, item.size) ||
		                LibKernel::Memory::TryReadSparseBacking(item.guest_address, item.destination, item.size);
		if (!ok) {
			std::lock_guard lock(m_mutex);
			m_failed.push_back(item);
		}
		_mm_sfence(); // write-combined stores are visible before the count says so
		m_done.fetch_add(1, std::memory_order_release);
	}
}

void UploadCopier::Fence() {
	while (m_done.load(std::memory_order_acquire) != m_submitted.load(std::memory_order_relaxed)) {
		_mm_pause();
	}
	std::vector<Item> failed;
	{
		std::lock_guard lock(m_mutex);
		failed.swap(m_failed);
	}
	for (const auto& item: failed) {
		// What the plain path does when neither backing view takes the range: a load from the guest address (it may fault into the tracker).
		std::memcpy(item.destination, reinterpret_cast<const void*>(item.guest_address), item.size);
	}
}

} // namespace Libs::Graphics
