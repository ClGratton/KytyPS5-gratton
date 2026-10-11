#ifndef GRAPHICS_GUEST_GPU_PIPELINE_LOOKAHEAD_H
#define GRAPHICS_GUEST_GPU_PIPELINE_LOOKAHEAD_H

// --pipeline-lookahead (shaders.md D2-C2, docs/lookahead-notes.md).
//
// Every submitted command buffer is copied at submit time and handed to a dedicated lookahead
// thread. That thread runs the normal PM4 decoder on "shadow" CommandProcessors (one per guest
// queue, same register tracking as the real ones, no side effects) and, for every draw and
// dispatch it sees, asks the PipelineCache to start translating the shaders and creating the
// pipeline on the worker pool, so the GPU thread finds them done (or at least running) when it
// gets there. It never skips and never changes what the GPU thread draws: a request the
// lookahead cannot answer exactly is dropped, and a wrong request only costs worker time.

#include "graphics/guest_gpu/command_processor/commandProcessor.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace Libs::Graphics {

class RenderContext;

class PipelineLookahead final: public ShadowSink {
public:
	enum class Kind { Graphics, Compute, Reset };

	struct Item {
		Kind                  kind     = Kind::Graphics;
		uint32_t              queue_id = 0;
		uint64_t              seq      = 0;
		std::vector<uint32_t> commands;
		std::vector<uint32_t> constant_commands;
	};

	explicit PipelineLookahead(RenderContext& renderer);
	~PipelineLookahead() override;
	PipelineLookahead(const PipelineLookahead&)            = delete;
	PipelineLookahead& operator=(const PipelineLookahead&) = delete;

	// Guest thread (under the GuestGpu queue lock): cheap, only queues the copy.
	void Push(Item item);
	// GPU thread: the highest submission sequence number the GPU thread has started executing.
	void NoteStarted(uint64_t seq) { m_started_seq.store(seq, std::memory_order_release); }
	// Stops the thread (waits briefly; a parked thread is abandoned). Idempotent.
	void Stop();

	void OnDraw(CommandProcessor& cp, const ShadowDraw& draw) override;
	void OnDispatch(CommandProcessor& cp, const ShadowDispatch& dispatch) override;

	struct Stats {
		std::atomic<uint64_t> items {0};
		std::atomic<uint64_t> items_late {0};   // GPU thread had already started them
		std::atomic<uint64_t> dispatches {0};
		std::atomic<uint64_t> draws {0};
		std::atomic<uint64_t> max_queue {0};
		std::atomic<int64_t>  busy_ns {0};
	};
	[[nodiscard]] const Stats& GetStats() const { return m_stats; }

private:
	void Run();
	void Process(Item& item);
	CommandProcessor& Shadow(uint32_t queue_id);

	RenderContext&                      m_renderer;
	std::mutex                          m_mutex;
	std::condition_variable             m_cv;
	std::deque<Item>                    m_queue;
	bool                                m_stopping = false;
	std::atomic<bool>                   m_exited {false};
	std::atomic<uint64_t>               m_started_seq {0};
	std::map<uint32_t, std::unique_ptr<CommandProcessor>> m_shadows;
	// Valid only on the lookahead thread while an item is processed.
	uint64_t                            m_current_seq  = 0;
	bool                                m_current_late = false;
	Stats                               m_stats;
	std::thread                         m_thread;
};

} // namespace Libs::Graphics

#endif // GRAPHICS_GUEST_GPU_PIPELINE_LOOKAHEAD_H
