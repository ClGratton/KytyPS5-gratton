#include "emulatorOptions.h"

#include <QCoreApplication>

namespace EmulatorOptions {

namespace {

using C = Config::ConfigOptions;

#define EMU_BOOL(field) [](const C& c) { return QVariant(static_cast<bool>(c.field)); }
#define EMU_INT(field) [](const C& c) { return QVariant(static_cast<int>(c.field)); }

QString tr(const char* text) {
	return QCoreApplication::translate("EmulatorOptions", text);
}

struct Def {
	const char* id;
	const char* flag;
	Tab         tab;
	const char* section;
	Kind        kind;
	const char* label;
	const char* tooltip;
	std::function<QVariant(const C&)> emulator_default;
	QStringList choices   = {};
	int         min       = 0;
	int         max       = 0;
	const char* special   = "";
	const char* suffix    = "";
	bool        needs_async = false;
	bool        flag_if_off = false;
	const char* env       = "";
	QVariant    launcher_default = {};
};

QString AsyncText(Config::AsyncPipelineMode mode) {
	switch (mode) {
		case Config::AsyncPipelineMode::Stall: return QStringLiteral("stall");
		case Config::AsyncPipelineMode::Skip: return QStringLiteral("skip");
		default: return QStringLiteral("off");
	}
}

QList<Option> BuildTable() {
	QList<Def> defs;

	// ---- Display -------------------------------------------------------------------------------
	defs.append({"overlay", "", Tab::Display, "Overlay and window", Kind::Bool,
	             "Performance overlay (MangoHud)",
	             "Show MangoHud's fps, frame-time graph, CPU, GPU and memory overlay over the game. "
	             "Needs the mangohud package. The launcher sets MANGOHUD=1 and MANGOHUD_CONFIGFILE to the "
             "mangohud.conf installed with the emulator (per-frame logging is off there).",
	             nullptr, {}, 0, 0, "", "", false, false, "MANGOHUD", QVariant(false)});
	defs.append({"splash", "--no-splash", Tab::Display, "Overlay and window", Kind::Bool,
	             "Show the startup splash screen",
	             "The emulator's own splash window while the game loads.",
	             EMU_BOOL(splash_enabled), {}, 0, 0, "", "", false, true});

	// ---- Shaders and stutter --------------------------------------------------------------------
	defs.append({"async_pipelines", "--async-pipelines", Tab::Shaders, "Pipeline compilation",
	             Kind::Choice, "Async pipelines:",
	             "Where shader translation and Vulkan pipeline creation run.\n"
	             "off: on the GPU thread, as stock Kyty (stutters while shaders compile).\n"
	             "stall: on worker threads; the draw waits for its pipeline (no visual difference "
	             "from off, shorter stalls, and the basis for look-ahead).\n"
	             "skip: draws whose pipeline is not ready are skipped instead of waited for. This "
	             "removes most stutter but can show transient glitches (objects missing for a few "
	             "frames the first time they are seen).",
	             [](const C& c) { return QVariant(AsyncText(c.async_pipelines)); },
	             {"off", "stall", "skip"}});
	defs.append({"pipeline_lookahead", "--pipeline-lookahead", Tab::Shaders, "Pipeline compilation",
	             Kind::Bool, "Look-ahead (build pipelines from queued command buffers)",
	             "Reads the submitted command buffers ahead of the GPU thread and starts building the "
	             "shaders and pipelines they will need. Halved the GPU thread's wait on pipeline "
	             "compiles in the measurements and removes most first-play stutter."
	             "\n\nNeeds Async pipelines = stall or skip.",
	             EMU_BOOL(pipeline_lookahead), {}, 0, 0, "", "", true});
	defs.append({"lookahead_workers", "--lookahead-workers", Tab::Shaders, "Pipeline compilation",
	             Kind::Int, "Look-ahead workers:",
	             "Worker threads that do the per-draw look-ahead work (shader programs, SRT plans, "
	             "pipeline requests) while the look-ahead thread only decodes the command stream. "
	             "0 keeps all of it on the look-ahead thread.\n\nNeeds Look-ahead.",
	             EMU_INT(lookahead_workers), {}, 0, 16, "Off", ""});
	defs.append({"pipeline_variants_replay", "--pipeline-variants-replay", Tab::Shaders,
	             "Pipeline compilation", Kind::Bool, "Replay recorded shader variants",
	             "Records the compute shader variants the game uses in _PipelineCache/<title>.variants "
	             "and, in the next session, builds the stored ones as soon as the game registers the "
	             "shader, seconds before its first dispatch. Helps from the second session on."
	             "\n\nNeeds Async pipelines = stall or skip.",
	             EMU_BOOL(pipeline_variants_replay), {}, 0, 0, "", "", true});
	defs.append({"precompile_on_load", "--precompile-on-load", Tab::Shaders, "Pipeline compilation",
	             Kind::Bool, "Precompile compute shaders when the game loads them (guess)",
	             "Builds a compute shader's program and pipeline for its most likely variant when the "
	             "game registers it. A guess: a wrong variant is wasted work. Experimental."
	             "\n\nNeeds Async pipelines = stall or skip.",
	             EMU_BOOL(precompile_on_load), {}, 0, 0, "", "", true});
	defs.append({"pipelines_unoptimized_first", "--pipelines-unoptimized-first", Tab::Shaders,
	             "Pipeline compilation", Kind::Bool, "Unoptimized pipelines first",
	             "A pipeline no cache holds is created unoptimized so its draw does not wait, then "
	             "rebuilt optimized in the background. Measured with no gain on NVIDIA; other GPUs "
	             "need the KYTY_PIPELINE_FAST_BUILD=1 environment variable. Experimental."
	             "\n\nNeeds Async pipelines = stall or skip.",
	             EMU_BOOL(pipelines_unoptimized_first), {}, 0, 0, "", "", true});
	defs.append({"skip_guard", "--skip-guard", Tab::Shaders, "Pipeline compilation", Kind::Bool,
	             "Skip guard (dependency-aware skip)",
	             "Memory that a skipped draw should have written and that a later pass reads as "
	             "descriptor tables, draw arguments, DMA or CPU readback is marked unsafe, and later "
	             "draws writing it wait for their pipeline instead of being skipped. Skipping still "
	             "changes the picture for a few frames; this only narrows what may be dropped."
	             "\n\nNeeds Async pipelines = skip.",
	             EMU_BOOL(skip_guard), {}, 0, 0, "", "", true});
	defs.append({"pipeline_workers", "--pipeline-workers", Tab::Shaders, "Pipeline compilation",
	             Kind::Int, "Compile worker threads:",
	             "Worker threads for the async pipeline pool. Auto = the smaller of 8 and half the "
	             "hardware threads."
	             "\n\nNeeds Async pipelines = stall or skip.",
	             EMU_INT(pipeline_workers), {}, 0, 64, "Auto", "", true});
	defs.append({"driver_cache_checkpoint_s", "--driver-cache-checkpoint", Tab::Shaders,
	             "Pipeline compilation", Kind::Int, "Save the driver cache every:",
	             "While pipelines are being compiled, a background thread writes the driver "
	             "pipeline cache to disk this often, so a crash or hang does not lose what the "
	             "session compiled and the next start does not compile it again. The game never "
	             "waits for the file. Off = the cache is written only when the emulator exits.",
	             EMU_INT(driver_cache_checkpoint_s), {}, 0, 3600, "Off", " s"});
	defs.append({"spirv_robust_bounds", "--spirv-robust-bounds", Tab::Shaders, "Shader code",
	             Kind::Bool, "Robust buffer bounds (shorter SPIR-V)",
	             "Leaves storage-buffer loads to the GPU's robustBufferAccess2 instead of emitting a "
	             "bounds check around each one: shorter SPIR-V and faster driver compiles. An "
	             "out-of-bounds load reads zero either way; stores and atomics keep their checks. "
	             "Ignored on a device without robustBufferAccess2.",
	             EMU_BOOL(spirv_robust_bounds)});
	defs.append({"skip_ir_validation", "--skip-ir-validation", Tab::Shaders, "Shader code",
	             Kind::Bool, "Skip shader IR validation",
	             "Skips the internal consistency checks of every shader translation (about a fifth of "
	             "translate time; the output is identical). Saves about 0.6 s per run.",
	             EMU_BOOL(skip_ir_validation)});

	// ---- GPU thread -----------------------------------------------------------------------------
	defs.append({"submit_coalesce", "--submit-coalesce", Tab::Gpu, "Submission", Kind::Bool,
	             "Coalesce submits",
	             "Holds back the Vulkan submit of end-of-pipe events and sends several at once: "
	             "about 47 instead of 360 submits per frame, +5.8% fps in the measurements.",
	             EMU_BOOL(submit_coalesce)});
	defs.append({"submit_coalesce_max", "--submit-coalesce-max", Tab::Gpu, "Submission", Kind::Int,
	             "Coalesce at most this many events:", "Events held back before a submit is forced.",
	             EMU_INT(submit_coalesce_max), {}, 1, 256});
	defs.append({"submit_coalesce_us", "--submit-coalesce-us", Tab::Gpu, "Submission", Kind::Int,
	             "Coalesce age limit:",
	             "Longest time an event is held back, in microseconds. Auto = one eighth of the "
	             "vblank period.",
	             EMU_INT(submit_coalesce_us), {}, 0, 20000, "Auto", " us"});
	defs.append({"gc_clock", "--gc-clock", Tab::Gpu, "Submission", Kind::Choice,
	             "Cache garbage-collector clock:",
	             "What ages the texture and buffer caches: collector runs (one per guest submission, "
	             "stock) or presented frames. Use frames together with Coalesce submits.",
	             [](const C& c) { return QVariant(c.gc_clock_frames ? QStringLiteral("frames")
	                                                                 : QStringLiteral("runs")); },
	             {"runs", "frames"}});
	defs.append({"elide_barriers", "--elide-barriers", Tab::Gpu, "Barriers and readbacks",
	             Kind::Bool, "Elide redundant barriers",
	             "Leaves out a guest cache-flush barrier when nothing was recorded since the "
	             "previous one in the same command buffer (about half of them in the measurements).",
	             EMU_BOOL(elide_barriers)});
	defs.append({"defer_draw_write_barriers", "--defer-draw-write-barriers", Tab::Gpu,
	             "Barriers and readbacks", Kind::Bool, "Defer draw write barriers",
	             "A draw whose shaders write buffers (feedback, counters, atomic maxima) no longer "
	             "ends the render pass and records a barrier after itself; the barrier waits until a "
	             "later draw may read the written bytes or any other command is recorded.",
	             EMU_BOOL(defer_draw_write_barriers)});
	defs.append({"overlap_dispatches", "--overlap-dispatches", Tab::Gpu, "Barriers and readbacks",
	             Kind::Bool, "Overlap independent dispatches",
	             "Consecutive compute dispatches whose bound resources do not conflict run without a "
	             "barrier between them, as on the console; the barrier is recorded once, before the "
	             "next other command.",
	             EMU_BOOL(overlap_dispatches)});
	defs.append({"known_fills", "--known-fills", Tab::Gpu, "Barriers and readbacks", Kind::Bool,
	             "Remember GPU fills",
	             "Remembers ranges last written by a uniform GPU fill so clear discovery needs no "
	             "GPU to CPU readback for them.",
	             EMU_BOOL(known_fills)});
	defs.append({"known_stores", "--known-stores", Tab::Gpu, "Barriers and readbacks", Kind::Bool,
	             "Know the dwords a compute dispatch stores",
	             "When a compute shader stores an immediate to an address its descriptor decides "
	             "(queue headers, flags), the GPU thread knows the value and does not drain the GPU "
	             "to read it back. Exact; only pays together with GPU-built indirect draws and "
	             "known clear codes.",
	             EMU_BOOL(known_stores)});
	defs.append({"known_copies", "--known-copies", Tab::Gpu, "Barriers and readbacks", Kind::Bool,
	             "Know the result of GPU metadata copies",
	             "When a compute shader copies the color-metadata keys of one surface to another, "
	             "the GPU thread knows what the destination holds and does not drain the GPU to "
	             "read the clear colour back. Exact; only pays together with the other drain cuts.",
	             EMU_BOOL(known_copies)});
	defs.append({"image_lookup_memo", "--image-lookup-memo", Tab::Gpu, "CPU cost per draw", Kind::Bool,
	             "Remember texture lookups",
	             "A texture lookup remembers which cached image it found until an image is added "
	             "or removed, instead of walking the page table every draw. Exact; lowers the GPU "
	             "thread's CPU time per draw.",
	             EMU_BOOL(image_lookup_memo)});
	defs.append({"texture_binding_reuse", "--texture-binding-reuse", Tab::Gpu, "CPU cost per draw",
	             Kind::Bool, "Keep texture bindings between draws",
	             "A draw keeps a shader slot's texture binding while the descriptor and the set of "
	             "cached images are unchanged, instead of decoding and looking it up again. Exact; "
	             "lowers the GPU thread's CPU time per draw.",
	             EMU_BOOL(texture_binding_reuse)});
	defs.append({"resolved_draw", "--resolved-draw", Tab::Gpu, "CPU cost per draw", Kind::Bool,
	             "Use draw parts computed ahead of time",
	             "The pipeline lookahead computes parts of each draw ahead of time (needs SRT "
	             "plans); the GPU thread checks them against the live state and uses them instead "
	             "of computing them. Exact; lowers the GPU thread's CPU time per draw.",
	             EMU_BOOL(resolved_draw)});
	defs.append({"mapping_memo", "--mapping-memo", Tab::Gpu, "CPU cost per draw", Kind::Bool,
	             "Remember which guest ranges are fully mapped",
	             "The GPU thread remembers whether a guest range is fully mapped until the memory "
	             "map changes, instead of taking the map's lock for every buffer of every draw. "
	             "Exact; lowers the GPU thread's CPU time per draw.",
	             EMU_BOOL(mapping_memo)});
	defs.append({"tracker_lockfree", "--tracker-lockfree", Tab::Gpu, "CPU cost per draw", Kind::Bool, "Answer tracker queries without locks",
	             "The buffer cache asks whether a guest range is CPU- or GPU-modified through lock-free copies of the dirty bits instead of taking the tracker's region locks. Exact; lowers the GPU thread's CPU time per buffer binding.",
	             EMU_BOOL(tracker_lockfree)});
	defs.append({"clean_guest_reads", "--clean-guest-reads", Tab::Gpu, "Barriers and readbacks",
	             Kind::Bool, "Read clean guest memory without a drain",
	             "The GPU thread reads shader metadata and vertex tables straight from guest memory "
	             "when none of the bytes is GPU-written, with no GPU drain.",
	             EMU_BOOL(clean_guest_reads)});
	defs.append({"bda_sync_skip", "--bda-sync-skip", Tab::Gpu, "Barriers and readbacks", Kind::Bool,
	             "Skip the buffer-address sync when nothing changed",
	             "Skips the all-buffer synchronization before draws that address memory through raw "
	             "pointers while no page changed since the last full pass. Under 1% in the "
	             "measurements.",
	             EMU_BOOL(bda_sync_skip)});
	defs.append({"image_pool", "--image-pool", Tab::Gpu, "CPU cost per draw", Kind::Bool,
	             "Reuse freed images instead of allocating new ones",
	             "Render targets that the game recreates every frame cost a driver memory free and "
	             "allocate (about 270 microseconds) each time. With this, freed images are kept for "
	             "up to a second and a half (at most 256 MiB) and handed to the next image of the "
	             "same size and format. Exact; lowers CPU time on the GPU thread.",
	             EMU_BOOL(image_pool)});
	defs.append({"image_presence", "--image-presence", Tab::Gpu, "CPU cost per draw", Kind::Bool,
	             "Skip the texture cache lock for memory no image covers",
	             "Reading shader metadata and vertex tables, and every guest write fault on a "
	             "streaming buffer, used to take the texture cache lock and walk its page table to "
	             "find that no image is there. With this the cache keeps a count of images per page "
	             "and answers that without the lock. Exact; lowers CPU time on the GPU thread and "
	             "on game threads.",
	             EMU_BOOL(image_presence)});
	defs.append({"plan_prefetch", "--plan-prefetch", Tab::Gpu, "CPU cost per draw", Kind::Bool,
	             "Prefetch the next draw's precomputed data",
	             "The pipeline lookahead builds each draw's plan on another core out of many small "
	             "memory blocks; the GPU thread meets each one cold. With this it touches every "
	             "line of the next plan's blocks while it uses the current one. Only a hint to the "
	             "CPU cache; lowers the GPU thread's CPU time per draw.",
	             EMU_BOOL(plan_prefetch)});
	defs.append({"draw_state_memo", "--draw-state-memo", Tab::Gpu, "CPU cost per draw", Kind::Bool,
	             "Do not redo per-draw work while the GPU registers have not changed",
	             "Most draws of a frame set the same GPU state as the draw before. With this the GPU thread "
	             "remembers what it worked out from that state (checks, render targets, shader set-up) and "
	             "reuses it while the state is unchanged. Exact; lowers the GPU thread's CPU time per draw.",
	             EMU_BOOL(draw_state_memo)});
	defs.append({"cp_direct_stores", "--cp-direct-stores", Tab::Gpu, "CPU cost per draw", Kind::Bool,
	             "Write the GPU's fence and label values without a page fault",
	             "When a frame finishes the emulated GPU writes small values into game memory. Those pages "
	             "are usually write-protected for tracking, so each write took a page fault on the GPU thread. "
	             "With this the value is written through the emulator's own view of the memory and the tracker "
	             "is told directly. Exact; lowers the GPU thread's CPU time per frame.",
	             EMU_BOOL(cp_direct_stores)});
	defs.append({"tracker_sleeping_locks", "--tracker-sleeping-locks", Tab::Gpu, "CPU cost per draw", Kind::Bool,
	             "Let waiting game threads sleep instead of spinning in the memory tracker",
	             "The emulator write-protects game memory the GPU uses and unprotects a page on the game's first "
	             "write to it. Many game threads do that at once, and the ones that wait for the page lock spin "
	             "on a core for the whole protection call. With this they sleep after a short spin, which leaves "
	             "cores to the GPU thread and the draw workers.",
	             EMU_BOOL(tracker_sleeping_locks)});
	defs.append({"write_fault_around", "--write-fault-around", Tab::Gpu, "CPU cost per draw", Kind::Bool,
	             "Take the pages ahead of a sequential writer in one fault",
	             "The emulator write-protects the memory the GPU uses to learn when the game "
	             "changes it, and every first write to a page is a fault (about 17,000 a frame in "
	             "the heaviest scene tested). When a fault continues where the previous run ended, "
	             "the clean pages ahead are marked modified with it, so a streaming writer faults "
	             "once per run. Exact; costs a few redundant page uploads.",
	             EMU_BOOL(write_fault_around)});
	defs.append({"backing_read_cache", "--backing-read-cache", Tab::Gpu, "CPU cost per draw", Kind::Bool,
	             "Remember where guest memory is mapped between reads",
	             "The GPU thread reads small pieces of guest memory all the time (shader resources, "
	             "vertex metadata, clear codes). Each read took the memory-map lock and walked the "
	             "mapping tree; with this it remembers the mapping entry it found until the map "
	             "changes. Exact; lowers the GPU thread's CPU time per draw.",
	             EMU_BOOL(backing_read_cache)});
	defs.append({"lazy_op_poll", "--lazy-op-poll", Tab::Gpu, "CPU cost per draw", Kind::Bool,
	             "Ask the GPU timeline less often between draws",
	             "Before each draw the GPU thread checks whether deferred operations (resource "
	             "releases, readback publication) have finished, which costs a driver call while "
	             "any are in flight, and waits for the priority-operations thread when an "
	             "earlier guest-memory completion has not run. With this it asks at most every 100 "
	             "microseconds and never waits; the operations run a few draws later. Exact; "
	             "lowers the GPU thread's CPU time per draw.",
	             EMU_BOOL(lazy_op_poll)});
	defs.append({"bda_dirty_log", "--bda-dirty-log", Tab::Gpu, "CPU cost per draw", Kind::Bool,
	             "Synchronize only the buffers whose memory changed",
	             "Before draws that address memory through raw pointers the GPU thread used to "
	             "walk every buffer of every mapped range. With this it visits only the buffers "
	             "that overlap a range the game wrote or that were just created, from a log kept "
	             "by the memory tracker. Exact; lowers the GPU thread's CPU time per draw.",
	             EMU_BOOL(bda_dirty_log)});
	defs.append({"readback_queue", "--readback-queue", Tab::Gpu, "Barriers and readbacks",
	             Kind::Bool, "Readbacks on a second queue",
	             "GPU to guest buffer readbacks copy on a second queue and wait only for the work "
	             "that wrote the data. No gain measured.",
	             EMU_BOOL(readback_queue)});
	defs.append({"poll_before_wait_us", "--poll-before-wait", Tab::Gpu, "Barriers and readbacks",
	             Kind::Int, "Poll the GPU before blocking:",
	             "Microseconds to poll the GPU timeline before blocking in a wait. Small readbacks "
	             "finish in tens of microseconds; a blocking wait adds the sleep and wake-up. Off "
	             "until measured.",
	             EMU_INT(poll_before_wait_us), {}, 0, 1000, "Off", " us"});
	defs.append({"eager_readback", "--eager-readback", Tab::Gpu, "Barriers and readbacks",
	             Kind::Bool, "Write back pages the game reads right after the GPU writes them",
	             "Pages a game thread read-faulted on are copied out in the command buffer about to "
	             "be submitted and written back to guest memory when it completes, so the next read "
	             "needs no fault and no GPU drain. Off until measured.",
	             EMU_BOOL(eager_readback)});
	defs.append({"readback_futures", "--readback-futures", Tab::Gpu, "Barriers and readbacks",
	             Kind::Bool, "Let the GPU thread run on while a game thread waits for a readback",
	             "A game thread's read of GPU-written memory no longer stops the GPU thread: it "
	             "copies the bytes out and goes on, and only the reading thread waits for them. "
	             "Exact; part of Recommended for play.",
	             EMU_BOOL(readback_futures)});
	defs.append({"gpu_indirect_draws", "--gpu-indirect-draws", Tab::Gpu, "Draw path", Kind::Bool,
	             "GPU-built indirect draws",
	             "Indirect draws whose argument block a GPU pass wrote are built on the GPU instead "
	             "of being read on the CPU after a full drain (mesh-emulated draws too).",
	             EMU_BOOL(gpu_indirect_draws)});
	defs.append({"draw_key_cache", "--draw-key-cache", Tab::Gpu, "Draw path", Kind::Bool,
	             "Draw key cache",
	             "Remembers the graphics pipelines of the last few draws and finds them by comparing "
	             "keys before hashing; also enables the SRT table walk below.",
	             EMU_BOOL(draw_key_cache)});
	defs.append({"srt_plans", "--srt-plans", Tab::Gpu, "Draw path", Kind::Bool,
	             "Use look-ahead SRT plans (needs look-ahead)",
	             "The GPU thread uses the shader resource tables the look-ahead thread already "
	             "materialized for a draw, after checking that the guest memory they were read from "
	             "is unchanged; without a valid plan it materializes them itself.",
	             EMU_BOOL(srt_plans)});
	defs.append({"srt_table_walk", "--srt-table-walk", Tab::Gpu, "Draw path", Kind::Bool,
	             "SRT table walk",
	             "Refreshes a shader's flattened SRT from a precomputed table instead of walking the "
	             "value graph.",
	             EMU_BOOL(srt_table_walk)});

	// ---- VRAM and RAM ---------------------------------------------------------------------------
	defs.append({"vram_paging", "--vram-paging", Tab::Memory, "VRAM", Kind::Bool,
	             "VRAM paging (memory priority + pageable memory)",
	             "Uses VK_EXT_memory_priority and VK_EXT_pageable_device_local_memory so the driver "
	             "pages low-priority textures out instead of failing an allocation. A safety net "
	             "for GPUs with little VRAM; a no-op without both extensions.",
	             EMU_BOOL(vram_paging)});
	defs.append({"gc_buffer_own_share", "--gc-buffer-own-share", Tab::Memory, "VRAM", Kind::Int,
	             "Buffer GC share:",
	             "The buffer garbage collector only goes aggressive (downloading dirty buffers, a "
	             "GPU drain each) when the buffer cache itself holds this share of the critical "
	             "budget. 0 = device usage alone decides. 25 suits cards whose problem is those "
	             "drains and that run with VRAM paging off; with paging on it measured worse.",
	             EMU_INT(gc_buffer_own_share_pct), {}, 0, 100, "Off", " %"});
	defs.append({"gc_texture_trigger_mb", "--gc-texture-trigger-mb", Tab::Memory, "VRAM",
	             Kind::Int, "Texture GC starts above:",
	             "VRAM usage in MiB above which the texture GC starts. Auto = derived from the "
	             "device's VRAM budget (never a fixed size).",
	             EMU_INT(gc_texture_trigger_mb), {}, 0, 1048576, "Auto", " MiB"});
	defs.append({"gc_texture_pressure_mb", "--gc-texture-pressure-mb", Tab::Memory, "VRAM",
	             Kind::Int, "Texture GC evicts harder above:",
	             "VRAM usage in MiB above which the texture GC evicts harder. Auto = derived.",
	             EMU_INT(gc_texture_pressure_mb), {}, 0, 1048576, "Auto", " MiB"});
	defs.append({"gc_texture_critical_mb", "--gc-texture-critical-mb", Tab::Memory, "VRAM",
	             Kind::Int, "Texture GC aggressive above:",
	             "VRAM usage in MiB above which the texture GC is aggressive. Auto = derived.",
	             EMU_INT(gc_texture_critical_mb), {}, 0, 1048576, "Auto", " MiB"});
	defs.append({"gc_buffer_trigger_mb", "--gc-buffer-trigger-mb", Tab::Memory, "VRAM", Kind::Int,
	             "Buffer GC starts above:",
	             "VRAM usage in MiB above which the buffer GC starts. Auto = derived.",
	             EMU_INT(gc_buffer_trigger_mb), {}, 0, 1048576, "Auto", " MiB"});
	defs.append({"gc_buffer_critical_mb", "--gc-buffer-critical-mb", Tab::Memory, "VRAM",
	             Kind::Int, "Buffer GC aggressive above:",
	             "VRAM usage in MiB above which the buffer GC is aggressive. Auto = derived.",
	             EMU_INT(gc_buffer_critical_mb), {}, 0, 1048576, "Auto", " MiB"});
	defs.append({"mem_release_punch", "--mem-release-punch", Tab::Memory, "Host RAM", Kind::Bool,
	             "Return released guest memory to the host",
	             "Gives guest memory the game released (and no view maps) back to the host with "
	             "fallocate(PUNCH_HOLE). Whole host pages only. Off until a trace shows memory "
	             "released but still resident.",
	             EMU_BOOL(mem_release_punch)});
	defs.append({"mem_hole_reads", "--mem-hole-reads", Tab::Memory, "Host RAM", Kind::Bool,
	             "Read never-written guest memory without making it resident",
	             "Reads guest memory that was never written with pread instead of through the "
	             "writable mapping, so the read does not allocate zero pages in RAM (reads of "
	             "16 KiB or more).",
	             EMU_BOOL(mem_hole_reads)});
	defs.append({"upload_ring_rebar", "--upload-ring-rebar", Tab::Memory, "Host RAM", Kind::Bool,
	             "Keep the upload staging ring in video memory (ReBAR)",
	             "Puts the 512 MiB upload staging ring in device-local host-visible memory instead "
	             "of system RAM: 0.5 GiB less host RAM for 0.5 GiB of VRAM. Used only when the card "
	             "has Resizable BAR and at least 8 GiB of VRAM; otherwise the ring stays in RAM.",
	             EMU_BOOL(upload_ring_rebar)});
	defs.append({"skip_zero_uploads", "--skip-zero-uploads", Tab::Memory, "Host RAM", Kind::Bool,
	             "Clear, do not upload, images whose memory was never written",
	             "A color image whose guest memory the game never wrote is cleared on the GPU "
	             "instead of reading, staging and detiling the zeros. Saves RAM bandwidth and "
	             "upload time.",
	             EMU_BOOL(skip_zero_uploads)});
	defs.append({"heap_trim", "--heap-trim", Tab::Memory, "Host RAM", Kind::Bool,
	             "Return free emulator heap to the system",
	             "Calls malloc_trim when at least 64 MiB and 20% of the emulator's heap is free "
	             "but still resident (at most every 5 s).",
	             EMU_BOOL(heap_trim)});
	defs.append({"mem_residency", "--mem-residency", Tab::Memory, "Statistics", Kind::Bool,
	             "Log guest memory residency",
	             "Every 10 s logs how much guest memory is resident per GPU range class, which "
	             "allocations hold memory without a GPU copy, and what the emulator itself wrote. "
	             "Costs about 1% of frame time.",
	             EMU_BOOL(mem_residency)});
	defs.append({"mem_stats", "--mem-stats", Tab::Memory, "Statistics", Kind::Bool,
	             "Log guest memory statistics",
	             "Logs guest memory release, unmap and decommit counters and how much released "
	             "memory is still resident, every 5 s and on exit.",
	             EMU_BOOL(mem_stats)});
	defs.append({"vram_stats", "--vram-stats", Tab::Memory, "Statistics", Kind::Bool,
	             "Log VRAM statistics",
	             "Logs VRAM heap usage and its split every 5 s, and each pipeline compile time.",
	             EMU_BOOL(vram_stats)});

	// ---- Controller (global settings) -----------------------------------------------------------
	defs.append({"pad_speaker", "--pad-speaker", Tab::Controller, "", Kind::Bool,
	             "Route game audio to the DualSense speaker",
	             "On: the game's controller-speaker audio plays on the DualSense. Off: it plays on "
	             "the normal audio output.",
	             EMU_BOOL(pad_speaker_enabled)});
	defs.append({"pad_speaker_volume", "--pad-speaker-volume", Tab::Controller, "", Kind::Int,
	             "Speaker hardware level:",
	             "The DualSense speaker's own hardware level (the raw report volume). The default "
	             "is quiet.",
	             EMU_INT(pad_speaker_volume), {}, 0, 100, "", " %"});
	defs.append({"pad_speaker_os_sink", "--pad-speaker-os-sink", Tab::Controller, "", Kind::Bool,
	             "Show the DualSense speaker as an OS volume control",
	             "Linux: exposes a Bluetooth DualSense speaker as a PipeWire/PulseAudio sink so the "
	             "system volume control can adjust it.",
	             EMU_BOOL(pad_speaker_os_sink)});

	// ---- Debug ----------------------------------------------------------------------------------
	defs.append({"gpu_phase_stats", "--gpu-phase-stats", Tab::Debug, "Diagnostics", Kind::Int,
	             "Phase statistics interval:",
	             "Every N seconds print where the GPU thread spends its time (phase shares, submits "
	             "per frame, drains by cause) and write _gpu_phase.csv. Adds timers: never use it "
	             "for an fps comparison.",
	             EMU_INT(gpu_phase_stats_s), {}, 0, 3600, "Off", " s"});
	defs.append({"submit_stats", "--submit-stats", Tab::Debug, "Diagnostics", Kind::Int,
	             "Submit statistics interval:",
	             "Every N seconds print frames, Vulkan submits per frame and the coalescing "
	             "counters (no timers; safe next to fps measurements).",
	             EMU_INT(submit_stats_s), {}, 0, 3600, "Off", " s"});
	defs.append({"hang_watchdog", "--debug-hang-watchdog", Tab::Debug, "Diagnostics", Kind::Int,
	             "Hang watchdog:",
	             "When no frame was presented for this long, write a thread, worker pool and "
	             "look-ahead report to stderr and _hang_watchdog.txt.",
	             EMU_INT(hang_watchdog_seconds), {}, 0, 3600, "Off", " s"});
	defs.append({"shader_loop_limit", "--shader-loop-limit", Tab::Debug, "Diagnostics", Kind::Int,
	             "Shader loop guard:",
	             "Safety net, on by default: each shader invocation may run its loop headers this "
	             "many times in total, then its loops are left and the shader is reported "
	             "(\"LOOP GUARD TRIPPED\" in the log), so a loop over corrupt data cannot hang the "
	             "GPU. It hides an emulation bug rather than fixing it: a report is worth a bug "
	             "report. Off = no guard.",
	             EMU_INT(shader_loop_limit), {}, 0, 2147483647, "Off"});
	defs.append({"hle_trace", "--debug-hle-trace", Tab::Debug, "Diagnostics", Kind::Bool,
	             "HLE call trace",
	             "Records the last HLE calls of every guest thread (name, first arguments, caller, "
	             "result for file and memory calls) and prints them in the fatal fault report. "
	             "Linux x86-64 only.",
	             EMU_BOOL(hle_trace)});
	defs.append({"debug_gpu_ops", "--debug-gpu-ops", Tab::Debug, "Diagnostics", Kind::Choice,
	             "GPU operation ring:",
	             "Keeps a host-side ring of the last draws and dispatches and prints the ones of "
	             "unfinished submissions when the GPU stops answering. checkpoints also uses "
	             "VK_NV_device_diagnostic_checkpoints (NVIDIA).",
	             [](const C& c) {
		             return QVariant(c.gpu_ops_mode == 1 ? QStringLiteral("ring")
		                             : c.gpu_ops_mode == 2 ? QStringLiteral("checkpoints")
		                                                   : QStringLiteral("off"));
	             },
	             {"off", "ring", "checkpoints"}});
	defs.append({"texture_gc", "--debug-texture-gc", Tab::Debug, "Diagnostics", Kind::Choice,
	             "Texture garbage collector:",
	             "true: normal. false: disable the texture GC (tells whether a rendering difference "
	             "comes from an evicted GPU-only image). log: log every free of a GPU-written "
	             "image with the flags that decided it.",
	             [](const C& c) {
		             return QVariant(c.texture_gc_log ? QStringLiteral("log")
		                             : c.texture_gc_enabled ? QStringLiteral("true")
		                                                    : QStringLiteral("false"));
	             },
	             {"true", "false", "log"}});
	defs.append({"pipeline_cache_key", "--pipeline-cache-key", Tab::Debug, "Diagnostics",
	             Kind::Choice, "Driver pipeline cache key:",
	             "schema: the driver pipeline cache survives emulator builds (keyed on a schema "
	             "version and the GPU and driver). revision: keyed on the git revision, so every "
	             "commit drops it and dirty builds have none (stock).",
	             [](const C& c) {
		             return QVariant(c.pipeline_cache_key == Config::PipelineCacheKey::Revision
		                                 ? QStringLiteral("revision")
		                                 : QStringLiteral("schema"));
	             },
	             {"schema", "revision"}});
	defs.append({"lookahead_verify", "--debug-lookahead-verify", Tab::Debug, "Diagnostics",
	             Kind::Bool, "Verify look-ahead (slow)",
	             "Recompiles every prefetched program before its first use and compares the "
	             "SPIR-V; mismatches are logged.",
	             EMU_BOOL(lookahead_verify), {}, 0, 0, "", "", true});
	defs.append({"draw_key_verify", "--debug-draw-key-verify", Tab::Debug, "Diagnostics",
	             Kind::Bool, "Verify the draw key cache (slow)",
	             "With the draw key cache, also runs the full lookup on every memo hit and stops if "
	             "the two disagree.",
	             EMU_BOOL(draw_key_verify)});
	defs.append({"eager_readback_verify", "--debug-eager-readback-verify", Tab::Debug, "Diagnostics",
	             Kind::Bool, "Verify eager write-backs (slow)",
	             "With eager readback, re-reads every page from the GPU (a full drain) before it is "
	             "unprotected and reports any difference.",
	             EMU_BOOL(eager_readback_verify)});
	defs.append({"srt_plan_shadow", "--srt-plan-shadow", Tab::Debug, "Diagnostics", Kind::Bool,
	             "Measure SRT plans (needs look-ahead)",
	             "The look-ahead thread keeps the shader resource tables it materializes as plans and "
	             "the GPU thread compares each with its own result; prints lead time, validity and "
	             "cost. Nothing is adopted.",
	             EMU_BOOL(srt_plan_shadow)});
	defs.append({"vram_audit", "--vram-audit", Tab::Debug, "Diagnostics", Kind::Int,
	             "VRAM audit after:",
	             "Once, this many seconds after start, log the resident image mix and the "
	             "content-duplicate textures.",
	             EMU_INT(vram_audit_s), {}, 0, 3600, "Off", " s"});
	defs.append({"vram_ballast", "--vram-ballast-mb", Tab::Debug, "Diagnostics", Kind::Int,
	             "VRAM ballast:",
	             "Holds this much device-local memory to simulate a smaller GPU (for testing "
	             "VRAM paging).",
	             EMU_INT(vram_ballast_mb), {}, 0, 1048576, "Off", " MiB"});

	QList<Option> table;
	for (const auto& d: defs) {
		Option o;
		o.id          = QString::fromLatin1(d.id);
		o.flag        = QString::fromLatin1(d.flag);
		o.tab         = d.tab;
		o.section     = tr(d.section);
		o.kind        = d.kind;
		o.label       = tr(d.label);
		o.tooltip     = tr(d.tooltip);
		o.def         = d.emulator_default ? d.emulator_default(C {}) : d.launcher_default;
		o.choices     = d.choices;
		o.min         = d.min;
		o.max         = d.max;
		o.special     = tr(d.special);
		o.suffix      = QString::fromUtf8(d.suffix);
		o.needs_async = d.needs_async;
		o.flag_if_off = d.flag_if_off;
		o.env         = QString::fromLatin1(d.env);
		table.append(o);
	}
	return table;
}

} // namespace

const QList<Option>& All() {
	static const QList<Option> table = BuildTable();
	return table;
}

const Option* Find(const QString& id) {
	for (const auto& option: All()) {
		if (option.id == id) {
			return &option;
		}
	}
	return nullptr;
}

QVariant Parse(const Option& option, const QVariant& raw, bool* ok) {
	if (ok != nullptr) {
		*ok = true;
	}
	const auto fail = [&]() {
		if (ok != nullptr) {
			*ok = false;
		}
		return option.def;
	};
	if (!raw.isValid()) {
		return option.def;
	}
	switch (option.kind) {
		case Kind::Bool: {
			if (raw.typeId() == QMetaType::Bool) {
				return raw;
			}
			const auto text = raw.toString().trimmed().toLower();
			if (text == QLatin1String("true") || text == QLatin1String("1") ||
			    text == QLatin1String("on")) {
				return QVariant(true);
			}
			if (text == QLatin1String("false") || text == QLatin1String("0") ||
			    text == QLatin1String("off")) {
				return QVariant(false);
			}
			return fail();
		}
		case Kind::Choice: {
			const auto text = raw.toString().trimmed().toLower();
			return option.choices.contains(text) ? QVariant(text) : fail();
		}
		case Kind::Int: {
			bool      converted = false;
			const int value     = raw.toString().trimmed().toInt(&converted);
			return converted && value >= option.min && value <= option.max ? QVariant(value)
			                                                                : fail();
		}
	}
	return fail();
}

QVariant Value(const QVariantMap& values, const Option& option) {
	const auto it = values.constFind(option.id);
	return it == values.constEnd() ? option.def : Parse(option, it.value());
}

QString ToText(const Option& option, const QVariant& value) {
	switch (option.kind) {
		case Kind::Bool: return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
		case Kind::Choice: return value.toString();
		case Kind::Int: return QString::number(value.toInt());
	}
	return {};
}

QStringList Args(const QVariantMap& values, bool controller) {
	QStringList args;
	for (const auto& option: All()) {
		if (option.flag.isEmpty() || (option.tab == Tab::Controller) != controller) {
			continue;
		}
		const auto value = Value(values, option);
		switch (option.kind) {
			case Kind::Bool:
				if (option.flag_if_off) {
					if (!value.toBool()) {
						args << option.flag;
					}
				} else {
					args << option.flag << ToText(option, value);
				}
				break;
			case Kind::Choice: args << option.flag << ToText(option, value); break;
			case Kind::Int:
				if (value.toInt() != option.def.toInt()) {
					args << option.flag << ToText(option, value);
				}
				break;
		}
	}
	return args;
}

QStringList Environment(const QVariantMap& values) {
	QStringList env;
	for (const auto& option: All()) {
		if (!option.env.isEmpty() && Value(values, option).toBool()) {
			env << option.env + QStringLiteral("=1");
		}
	}
	return env;
}

QVariantMap Recommended() {
	// bench/config-play-next.json: async pipelines + look-ahead, submit coalescing with the frame
	// GC clock, the three drain cuts, memory statistics and a hang watchdog. Everything else of
	// that config is a classic setting (see Configuration::ApplyRecommendedForPlay).
	QVariantMap values;
	values.insert(QStringLiteral("async_pipelines"), QStringLiteral("stall"));
	values.insert(QStringLiteral("pipeline_lookahead"), true);
	values.insert(QStringLiteral("submit_coalesce"), true);
	values.insert(QStringLiteral("gc_clock"), QStringLiteral("frames"));
	values.insert(QStringLiteral("elide_barriers"), true);
	values.insert(QStringLiteral("known_fills"), true);
	values.insert(QStringLiteral("clean_guest_reads"), true);
	values.insert(QStringLiteral("srt_plans"), true);
	values.insert(QStringLiteral("image_lookup_memo"), true);
	values.insert(QStringLiteral("texture_binding_reuse"), true);
	values.insert(QStringLiteral("defer_draw_write_barriers"), true);
	values.insert(QStringLiteral("readback_futures"), true);
	values.insert(QStringLiteral("mem_stats"), true);
	values.insert(QStringLiteral("hang_watchdog"), 15);
	values.insert(QStringLiteral("splash"), false);
	return values;
}

} // namespace EmulatorOptions
