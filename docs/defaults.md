# Option defaults

Rule (repo.md, "Proper emulator principle"): anything a game needs to run correctly (accurate
emulation, crash and hang fixes, safety nets) is **on by default and tested**; only performance
features start off until measured. A safety net that hides a bug logs loudly when it fires and
never replaces the real fix. Every switched default keeps its opt-out flag. Pinned by
`tests/ConfigDefaultsTests.cpp` (`config_defaults`) and the `emulator_*_cli` ctests.

Categories: **correctness** (what a game needs, or a safety net), **perf** (speed only),
**debug** (diagnostics), **host** (hardware, UI, input, audio).

## Switched to on (branch `correct-defaults`)
| option | was | now | category | rationale and evidence |
|---|---|---|---|---|
| `--gpu-timestamps` | legacy | **gpu** | correctness | the guest reads the host GPU's own execution time for EOP and COPY_DATA clocks instead of the parse time (K51); falls back to parse time, logged once, without VK_EXT_calibrated_timestamps. fps-neutral on Astro Bot (48.75 fps in the final smoke). The "lemon desert in 6 of 7 runs with gpu timestamps" does not hold: 53 of 80 gpu runs vs 27 of 41 legacy runs are lemon (66 % both); lemon tracks the route's pause-menu sync time (93 % below 60 s, 33 % above 90 s; `bench/lemon_story.py`) and is a two-state colour (G/R 0.77 or 0.99), not a lighting ramp. Test `guest_gpu_timestamps` |
| `--pipeline-cache-key` | revision | **schema** | correctness (cache validity) | the driver pipeline cache survives emulator builds and works in dev/dirty builds; dropped on another GPU, driver, schema version, damage, or over 512 MiB (new cap). Rules pinned in `pipeline_cache_file`; the driver addresses entries by the whole creation input, so a changed build only misses |
| `--vram-paging` | false | **on** | correctness (safety net) | memory priority + pageable device-local memory: an allocation that would fail makes the driver page low-priority textures out. Normal play 48.75 fps (on) vs 48.32 (off); at a forced 5.7 GB budget 35.2 vs 17.0 fps in the last 45 s, 31 s vs 96 s of frames over 50 ms, 0 vs 47 allocation failures caught by the reclaim path. The allocation ladder (log, reclaim, retry, host fallback) is always on underneath. `--vram-stats` is no longer implied |

## Audited, stays off, with the measurement
| option | default | category | decision |
|---|---|---|---|
| `--readback-linear-images` | off | correctness (too costly now) | downloads every GPU-written linear image on submit; fixes games whose CPU reads GPU-written linear memory (GTA V vehicle deformation, TheCruZ/KytyPS5-GTA@fafc5cf9 claimed 0.35 ms per 10 s). On Astro Bot, same binary: **19.3 fps vs 48.5 fps**, median frame 49.9 vs 16.7 ms (runs `cd-bis-rest`, `cd-bis-readback`). Not acceptable as a default. The real fix is to download only what the CPU touches (enrol on a CPU read fault of the image pages); until then a title that needs it passes the flag. Pinned by `config_defaults` |
| `--gc-buffer-own-share` | 0 | correctness (K42, hurts with paging) | the 25 % gate stops the buffer GC going aggressive for pressure only textures cause. At a forced 5.7 GB budget it changes nothing with paging off (16.6 vs 17.0 fps) but with paging on (the new default) falls from 35.2 to 12.8 and 12.2 fps (2 runs) because ~370 MiB more buffers stay resident and the driver pages continuously. Opt-in (`25`) for cards without paging whose problem is the drains |
| `--tessellation` | off | correctness (incomplete) | the HS/DS lowering is not complete (upstream PRs #1032/#1245 are per-game fixes, a non-affine HS address is a hard EXIT here, 6 vs 9 fps on a title screen). On by default it would trade missing geometry for crashes. A skipped patch draw now logs a WARNING (1st, 2nd, 4th, ... with the count). Astro Bot never drew a patch (no warning in any run, and no change with the flag on: 48.5 fps) |
| `--sync-raw-image-buffers` | off | correctness (cost) | raw storage-buffer reads of GPU-written image memory synchronise from the image (fixes missing textures and transparency in some games per upstream; always on for one day upstream, made opt-in as "may reduce performance"). Astro Bot: 46.73 fps vs 48.3-48.75, 9.3 s vs 7.2-7.6 s of frames over 50 ms, no visible difference at the checkpoints, so this title does not need it and pays 3-4 %. Left opt-in until a title that needs it is identified |
| `--playgo-hack` | off | host compat | invents PlayGo chunk data for dumps without `playgo-chunk.dat`; a workaround for a bad dump, not emulation |
| `--amd-cpu` | off | host | patches AMD-only guest instructions for hosts without them (SSE4a, RDPID, CLWB); a no-op on an AMD host. Follow-up: enable automatically from `GetGuestInstructionHostFeatures()`; not verifiable on this AMD machine |
| `--redzone` | off (Windows) | host | Windows SysV red-zone protection |

## Always on (no flag, or on with an opt-out)
| behaviour | opt-out | category | note |
|---|---|---|---|
| image-allocation ladder: log VkResult and heap usage, texture-GC reclaim, one retry, host-memory last resort, then exit | none | correctness (safety net) | logs loudly when it fires (`vram-level-entry`, vram.md) |
| LDS shrink for oversized compute workgroups | none | correctness | `lds_span` test |
| K02 short-read fallback in guest file reads (a kernel copy into a GPU-protected page stops at the page) | `KYTY_SYSFILE_NO_READ_FALLBACK=1` (A/B debugging only) | correctness | the level-loader crash #2 root cause fix; counted and logged |
| `--debug-texture-gc` | `false` | correctness | the texture garbage collector; `false` and `log` are debugging aids |
| `--trophy-notifications` | `false` | host | toasts and sound |

## Performance features (off until measured, then switched in their own commit)
All have tests and measurements in `bench/RESULTS.md`; the recommended play set in
`bench/config-play-next.json` passes the first group. Defaults stay off until a pad play session
confirms them (forks/decision.md).
| option | default | measured |
|---|---|---|
| `--async-pipelines` | off (play: stall) | stall + lookahead: GPU-thread wait on pipeline jobs 16.9 s to 8.4 s, stutter after 30 s 13.1 s to 1.0 s |
| `--pipeline-lookahead` | false (play: true) | as above; needs `--async-pipelines` stall or skip |
| `--submit-coalesce`, `--gc-clock frames` | false, runs (play: true, frames) | K13+K43: submits per frame 360 to 47, fps +5.8 % |
| `--elide-barriers`, `--known-fills`, `--clean-guest-reads` | false (play: true) | drain cuts K14/K22/K20: fps +6.2 % (3 vs 3) |
| `--readback-queue`, `--bda-sync-skip`, `--pipelines-unoptimized-first`, `--skip-ir-validation` | false | no gain / under 1 % / no gain on NVIDIA / 0.6 s per run |
| `--pipeline-workers`, `--pipeline-lookahead-kinds`, `--submit-coalesce-max/-us` | auto, all, 16, 0 | tuning knobs |
| `--driver-cache-checkpoint <seconds>` | 0 (off: the driver pipeline cache is written only at exit) | durability, not speed: a background thread writes the cache every N s while pipelines are created, so a crash or hang keeps them. 9-42 ms per write (11-34 MB), no frame-time difference at 1 vs 1, fresh and warm cache (bench/RESULTS.md, K122) |
| `--shader-optimization-type` | None | compile time against shader speed |
| `--mem-release-punch` | off | ram.md: only with a trace showing at least 1 GB released but resident |
| `--fps-limit`, `--gpu-time-scale` | 0 (off), 1.0 | user controls |
| `--gc-texture-*-mb`, `--gc-buffer-*-mb` | 0 = derived from the device budget | tunable thresholds, never per-game |

## Debug and measurement (off)
`--vulkan-validation`, `--shader-validation`, `--gpu-assisted-validation`, `--command-buffer-dump*`,
`--graphics-debug-dump`, `--printf-direction/-output-file`, `--shader-log-*`, `--spirv-debug-printf`,
`--profile`, `--rd`, `--vram-stats`, `--mem-stats`, `--vram-audit`, `--vram-ballast-mb`,
`--debug-hang-watchdog` (play config passes 15), `--debug-lookahead-verify`, `--gpu-phase-stats`,
`--submit-stats`, `--shader-timing-log`, `--bench-*`; env `KYTY_IR_VALIDATE`, `KYTY_ALLOW_PTRACE`,
`KYTY_PIPELINE_FAST_BUILD` (K33, NVIDIA only), `KYTY_SYSFILE_CHAOS_EFAULT` (fault injection),
`KYTY_BORDERLESS` (macOS).

## Host, UI, input, audio
`--screen-width/-height` 1280x720, `--user-name` Kyty, `--user-id` 1000, `--mic` none,
`--controller-color`, `--controller-volume` 50, `--controller-vibration` 100, `--pad-speaker` on,
`--pad-speaker-volume` 15, `--pad-speaker-os-sink` on, `--present-mode` Mailbox, `--gpu` auto,
`--fullscreen` off, `--hide-cursor` off, `--no-splash` (splash on), `--vr` off,
`--vblank-frequency` 60, `--console-language` 1, `--skip-notice-screen` false, `--keymap`.
