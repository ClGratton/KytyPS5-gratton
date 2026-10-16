#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_DRIVERCACHECHECKPOINT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_DRIVERCACHECHECKPOINT_H_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace Libs::Graphics {

/**
 * @brief Writes the driver pipeline cache to disk from a background thread while the game runs.
 *
 * The cache used to be written once, when the emulator shut down, so a crash, a GPU hang or a kill
 * lost every pipeline compiled in that session and the next session compiled them again on the
 * GPU thread. Every `period` this thread asks how many pipelines the driver has created so far and,
 * when that number moved since the last successful write, calls `write` on its own thread: the
 * driver synchronizes the cache object, so pipeline creation (the GPU thread, the pipeline
 * workers) goes on while the file is produced and the GPU thread never waits for the file.
 *
 * `Stop()` joins the thread: no write is running or starts after it returns. The owner stops it
 * before the final write at shutdown and before the cache object is destroyed.
 *
 * Ported idea: a periodic write of the live cache (KytyPS5/KytyPS5@959bee479, nmzik and fxpw) done
 * off the GPU thread (TheCruZ/KytyPS5-GTA@ee11e85ad).
 */
class DriverCacheCheckpointer {
public:
	/// Pipelines created so far (never decreases).
	using CreatedCounter = std::function<uint64_t()>;
	/// Writes the cache file; true when the file was replaced. A failure is retried next period.
	using WriteFunction = std::function<bool()>;

	DriverCacheCheckpointer(std::chrono::milliseconds period, CreatedCounter created,
	                        WriteFunction write):
	    m_period(period),
	    m_created(std::move(created)), m_write(std::move(write)), m_last_written(m_created()) {
		m_thread = std::thread([this] { Run(); });
	}

	DriverCacheCheckpointer(const DriverCacheCheckpointer&)            = delete;
	DriverCacheCheckpointer& operator=(const DriverCacheCheckpointer&) = delete;

	~DriverCacheCheckpointer() { Stop(); }

	void Stop() {
		{
			std::scoped_lock lock {m_mutex};
			m_stop = true;
		}
		m_wake.notify_all();
		if (m_thread.joinable()) {
			m_thread.join();
		}
	}

	/// Writes that replaced the file (successful `write` calls).
	[[nodiscard]] uint64_t Writes() const {
		std::scoped_lock lock {m_mutex};
		return m_writes;
	}

	/// Writes that failed and will be retried.
	[[nodiscard]] uint64_t Failures() const {
		std::scoped_lock lock {m_mutex};
		return m_failures;
	}

private:
	void Run() {
		std::unique_lock lock {m_mutex};
		while (!m_stop) {
			if (m_wake.wait_for(lock, m_period, [this] { return m_stop; })) {
				break;
			}
			const auto created = m_created();
			if (created == m_last_written) {
				continue; // nothing new since the last file: no write, no cost
			}
			lock.unlock();
			const bool written = m_write();
			lock.lock();
			if (written) {
				// Pipelines created while the file was produced may or may not be in it; the next
				// period writes again for them (`created` was read before the write started).
				m_last_written = created;
				++m_writes;
			} else {
				++m_failures;
			}
		}
	}

	const std::chrono::milliseconds m_period;
	CreatedCounter                  m_created;
	WriteFunction                   m_write;
	mutable std::mutex              m_mutex;
	std::condition_variable         m_wake;
	bool                            m_stop = false;
	uint64_t                        m_last_written = 0;
	uint64_t                        m_writes       = 0;
	uint64_t                        m_failures     = 0;
	std::thread                     m_thread;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_DRIVERCACHECHECKPOINT_H_
