# Resolution and fps controls (branch `res-fps-controls`, forks/decision.md K51)

User request: "a res and fps slider if doable, and 1080p 60 fps for now".

## What the branch adds

| Flag (launcher control) | Default | What it does |
|---|---|---|
| `--gpu-timestamps legacy\|gpu` (launcher: "GPU timestamps") | `legacy` | Source of the 64-bit GPU timestamps a guest reads (EVENT_WRITE_EOP and COPY_DATA clock sources). `legacy` is the host clock at the moment the emulator parses the packet. `gpu` writes a Vulkan timestamp query at that point of the command stream and rewrites the guest's value when the query completes, mapped onto the guest GPU clock (100 MHz reference) through one `VK_EXT_calibrated_timestamps` pair. Falls back to `legacy` per packet if the query ring is full or unavailable. |
| `--gpu-time-scale 0.1-16` (launcher: "Dynamic resolution bias", 0.5x-4.0x slider) | `1.0` | Multiplies the GPU durations a guest observes. A title that sizes a dynamic-resolution target from them renders lower above 1.0 and higher below it. Times are stretched from the latest guest flip, so durations inside a frame scale exactly and the clock snaps back at each flip (a stamp is never more than one frame's stretch ahead; one frame's stretch is capped at 100 ms). |
| `--fps-limit 0-1000` (launcher: "FPS limit", 0-240 slider, 0 = off) | `0` | Presents at most this many frames per second. It runs on the virtual vblank (`--vblank-frequency`, default 60): the presenter skips vblanks with an even Bresenham spread, the guest still sees every vblank, and flips wait in the flip queue, which is what throttles the game. A limit at or above the vblank frequency is off, so to go above 60 raise `--vblank-frequency`. |
| `--bench-frames-csv`, `--bench-render-csv`, `--bench-exit-after-frames` | off | Observation-only benchmark instrumentation: steady-clock time per presented frame; render-pass sizes per guest flip; clean exit after N frames. |

Pure logic with unit tests (`ctest`: `gpu_clock`, `frame_pacer`): `renderer/gpuClock.h` (TSC to reference clock, device-tick to TSC with wrap handling, `TimeScaler`) and `presentation/framePacer.h`.

The launcher tooltip for the bias slider says what is true: it does not force a resolution, and a game with a fixed resolution ignores it.

Ideas and credit: the scale model is from BryanKAdams/BryKytyPS5 `b44c4edeb`; the GPU-true timestamp query ring is from ogeidexe/KytyPS5 `1936f6169` (both GPL-2.0), re-implemented here with different clock-domain, wrap and 32-bit-target handling and with tests.

## What Astro Bot does with them (measured)

All numbers are Astro Bot on the HEADLESS-BENCH virtual output (2560x1440, 60 Hz), skip2 route, fresh caches, one run each. Never pool them with DP-3 runs (200 Hz VRR). Run dirs are in `bench/runs/*rf-*`; summaries come from `bench/resfps_analyze.py`.

**The route is bimodal and these runs are not scene-matched.** In three runs (rf-gpu, rf-gpu-s2, rf-legacy-s1.25) the route reached the desert from about s08-s09. In the other four (rf-legacy, rf-legacy-s2, rf-gpu-s4, rf-pin-*) the checkpoint screenshots show the space/planet scene for the whole run. fps must only be compared within a scene, and with one run per setting even that is a rough guide.

Desert (gameplay), window s10 to the end (about 20 s):

| run | timestamps | scale | fps | frame p50 / p99 ms | frames > 20 ms | GPU util | busiest thread CPU | scene size | Mpix/flip |
|---|---|---|---|---|---|---|---|---|---|
| rf-gpu | gpu | 1.0 | 47.0 | 16.9 / 36.8 | 26 % | 65 % | 56 % | 1920x1080 | 109 |
| rf-legacy-s1.25 | legacy | 1.25 | 45.7 | 16.8 / 37.3 | 16 % | 64 % | 54 % | 1920x1080 | 112 |
| rf-gpu-s2 | gpu | 2.0 | 44.4 | 16.8 / 38.2 | 21 % | 68 % | 56 % | 1920x1080 | 112 |

Space scene, window s04 to the end (28-44 s, includes recovery from loading hitches, so lower and noisier):

| run | timestamps | scale | fps | frame p50 / p99 ms | frames > 20 ms | GPU util | busiest thread CPU | scene size |
|---|---|---|---|---|---|---|---|---|
| rf-legacy | legacy | 1.0 | 37.0 | 16.8 / 82 | 25 % | 48 % | 63 % | 1920x1080 |
| rf-legacy-s2 | legacy | 2.0 | 35.3 | 16.7 / 100 | 25 % | 51 % | 61 % | 1920x1080 |
| rf-gpu-s4 | gpu | 4.0 | 36.8 | 16.8 / 81 | 26 % | 54 % | 62 % | 1920x1080 |
| rf-pin-auto | gpu | 1.0 | 46.3 | 16.7 / 79 | 19 % | 59 % | 58 % | 1920x1080 |

(rf-gpu over the full s04 window, which starts in the space scene: 38.3 fps, p99 128 ms.)

### Finding 1: the scene is already 1080p, and no scale changes it

Per guest flip, `render.csv` lists the render passes by size. In every run, in the desert and in the space scene alike, the scene is drawn at 1920x1080 (about 70 passes per flip, 66-78 % of the pixels written by 16:9 passes), plus two 2432x1368 passes, then a single 3840x2160 pass into the 4K flip buffer (7-12 % of the pixels). The median per flip is 108-125 Mpixel of 16:9 pass area in every run. That is identical at scale 1.0, 1.25 and 2.0 in the desert and at 1.0, 2.0 and 4.0 in the space scene, and with legacy and gpu timestamps. At scale 4 the game sees GPU times four times as long, so a game that sized a target from them would have collapsed to its minimum.

So Astro Bot does not do dynamic resolution from GPU timestamps in this scene. There is no scale that "settles at about 1080p", because it is 1080p at every scale; the slider changes nothing for this title. It stays useful for titles that do size targets from GPU timing (not yet tested on one), which is why it is labelled a bias and not a resolution.

The odd sizes that appear only in transitions (3328x1872, 1216x684, 864x486) are loading and cutscene passes, not a resolution control.

### Finding 2: 60 fps is not reached; the limiter is a CPU/GPU serialisation, not a saturated unit

- Frame times are bimodal: in the desert 74-84 % of frames take one vblank (16.7 ms) and 16-26 % take two (33 ms); under 1 % are above 50 ms. In the space scene runs 1-3 % of frames were hitches above 60 ms, which were 13-28 % of the time. The median is exactly one vblank, which is why p50 looks perfect while the average is 44-47 fps in the desert and 35-46 in the space runs.
- In the desert the game runs at 44-47 fps (49-53 fps in the quietest seconds of rf-gpu-s2, worst frame 34-38 ms), while the busiest thread is at 54-56 % of a core and GPU utilisation is 64-68 %. Neither is saturated, yet 55 % + 65 % is above 100 %: at 45 fps (22 ms per frame) that is roughly 12 ms of busiest-thread CPU plus 14 ms of GPU time. They overlap only partly, so the guest GPU thread's CPU work and the host GPU's execution are largely serial per frame, and 60 fps needs that sum down from about 26 ms to under 17 ms. That is consistent with the plan in forks/decision.md (kyGpu time must roughly halve; submit coalescing K13 and barrier work K14 are the candidate levers). Resolution is not a lever for this title: the scene is already 1080p.
- Seconds at 60 fps do occur (up to 8 of 44 in some runs), so the frame work is not far above one vblank.
- The busiest thread is named `kyty_emulator.r...` in these runs and is identified as the guest GPU thread by its CPU profile (the `kyGpu` name exists only in the gpu-incremental branch). Treat the per-frame split above as an estimate: GPU util is `nvidia-smi` time-fraction, not busy time.
- Environment noise is large and explains the p99: every rf run had MemAvailable down to 0.2-0.7 GiB (emulator RSS 9 GB on a 14 GB box with other apps open), 1.9-2.7 GiB of swap I/O, and software thermal throttle flags on the GPU. A run where the host load average was 43 (other builds) aborted as "frozen". Read the fps column as a ceiling-ish indication, not as a baseline, until a run is taken on a quiet machine.

### Dropped experiment: `--output-resolution 1080p`

The idea was to answer `sceVideoOutGetOutputStatus` with a 1080p class so that a title picks a 1080p flip buffer instead of 4K. It was implemented with a pure function and tests, and measured (rf-pin-1080out vs rf-pin-auto): the flip buffer stayed 3840x2160 and the emulator never logged a call to the function, so Astro Bot picks its 4K output buffer without asking. The 4K pass is only 7-12 % of pass pixels anyway. The two commits were not kept (no title shown to use it, and a launcher claim that "most games" would honour it would be untrue). The patch is in `bench/agent-logs/res-fps-output-resolution-dropped.patch` (not in git).

## Reproducible pin for fps A/Bs

An fps comparison is only like-for-like if both sides render the same amount of work. The pin is:

```
--gpu-timestamps gpu --gpu-time-scale 1.0      # no --fps-limit
--bench-frames-csv frames.csv --bench-render-csv render.csv
```

`bench/config-resfps-pin.json` has it (skip2 route, `--stop-after s10`, about 160 s). `run.py` writes `res_pin` into `meta.json` (settings plus the scene size, flip buffer and median megapixels per flip taken from `render.csv`), the `res_pin` control in `bench/CONTROLS.md` warns when the resolution was not recorded or an fps limit is set, and `analyze.py` prints **RESOLUTION CONFOUNDED** when the pin or the rendered size differs inside a group or between the groups of a comparison. For Astro Bot the pin is the identity scale with GPU timestamps; the scene is 1080p regardless, so the check mostly guards against a future change (a title or build that does react to timing).

## Verification

- Unit tests `gpu_clock` and `frame_pacer` in `ctest`. The GPU-timestamp query path and the launcher controls are verified by the runs below and by building the launcher, not by unit tests.
- Runs: `bench/runs/*rf-*` (config `bench/config-resfps-*.json`). One run each; differences under about 10 % between them are inside the noise described above and are not claims.

## Known limits

- No real "resolution slider": a generic render-scale for titles with a fixed resolution (scaling guest render targets, viewports and scissors) is a large separate feature and is not attempted here.
- `gpu-time-scale` has been measured only against Astro Bot, which ignores it.
- `--gpu-timestamps gpu` needs `VK_EXT_calibrated_timestamps`; without it the legacy path is used. Check it with K13 (submit coalescing) on and off, since batching may move EOP writes.
- The frame limiter works at vblank granularity (60 Hz gives 60, 30, 20, ... or an even mix such as 45 as 3 of 4).
