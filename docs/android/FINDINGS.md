# Findings: what we learned on the Thor, and why the code is the way it is

Each item says **what** was found or changed, the **evidence** (measured numbers), and **where** it lives (file and
function, or switch). Unless a run says otherwise: AYN Thor Max, AYN performance mode 2, Light profile (75 % + SGSR,
30 fps, shadows 0.5), Turnip 27.0.0 R2. The scenes are described in [README.md](README.md#words-used-in-these-pages).
The switches are listed in [ENV.md](ENV.md). Current numbers and open issues are in [STATUS.md](STATUS.md).

The last section, [What did not work](#what-did-not-work), lists the ideas that were measured and dropped.

## Contents

- [Driver choice: Turnip 27.0.0 R2](#driver-choice-turnip-2700-r2)
- [Integer math without shaderInt64](#integer-math-without-shaderint64)
- [Thread placement: the biggest win](#thread-placement-the-biggest-win)
- [Recorder wake threshold](#recorder-wake-threshold)
- [Render-thread fast paths](#render-thread-fast-paths)
- [Translator: loops and bounded jump tables](#translator-loops-and-bounded-jump-tables)
- [GPU profiler: idle time was charged to passes](#gpu-profiler-idle-time-was-charged-to-passes)
- [NMin / NMax / NClamp](#nmin--nmax--nclamp)
- [ClearBuffer and ReportValue](#clearbuffer-and-reportvalue)
- [Indirect-args clamp](#indirect-args-clamp)
- [Pacing: the panel is 59.65 Hz](#pacing-the-panel-is-5965-hz)
- [The 45 fps mode: removed](#the-45-fps-mode-removed)
- [Background pause and the clock freeze](#background-pause-and-the-clock-freeze)
- [Thermal](#thermal)
- [Pipeline cache](#pipeline-cache)
- [Display: fullscreen, top screen, frame-rate hint](#display-fullscreen-top-screen-frame-rate-hint)
- [Textures: what is and isn't a texture problem](#textures-what-is-and-isnt-a-texture-problem)
- [Audio path](#audio-path)
- [GPU cost breakdown](#gpu-cost-breakdown)
- [Working with the device](#working-with-the-device)
- [What did not work](#what-did-not-work)

## Driver choice: Turnip 27.0.0 R2

**What.** Upstream links `-lvulkan` and so always uses Qualcomm's stock driver. The fork builds on volk
(`VOLK=1`) and loads the driver through libadrenotools, so any Turnip build or Qualcomm package can be used without
root. The packaged default is Turnip 27.0.0 R2 (StevenMXZ build, Mesa main, Vulkan 1.4.367).

**Evidence.** New save, 75 %, 30 fps:

| Driver | Result |
|---|---|
| Stock Qualcomm 1.3.128 | 3D corrupted: checkerboard and stripe blocks, no characters, smeared textures. White text boxes until the 32-bit math below. 30.0 fps, street GPU 18.7 ms on a broken picture. No `shaderInt64`, no `shaderOutputLayer`. |
| Qualcomm 863.1 package | **Correct picture**, so the stock corruption is a stock-driver bug. But the GPU hangs (device lost) after about 3 minutes in the street. |
| Turnip 26.0.0 R8 | Correct UI, then SIGSEGV in Mesa's ir3 register allocator (`ir3_ra` → `interval_insert`) at the first 3D scene, every run, compiling the largest culling compute shader (26.6k instructions). |
| Turnip 27 (The412Banner, Mesa `c0235ef`) | Correct, no crash in 232 s. Same GPU cost as R2, worse pacing in that pair of runs (p95 35.8–40.2 ms). |
| **Turnip 27.0.0 R2** | **Correct picture**, no crash. 10-minute acceptance run: 30.0 fps in every minute, p95 33.4 ms, RSS 2.14 GB. Mesa main has several ir3 RA fixes since R8. |

**Where.** `runtime/nvn/vulkan/device.c` `custom_driver`, `runtime/nvn/vulkan/volk_impl.c`; switch
`LARECOMP_DRIVER` (unset: the packaged `libvulkan_turnip.so`; a file name beside `libmain.so`; an absolute path to a
driver unpacked in the app's internal storage; `system`). `make_project.sh` packages
`build/android/deps/adrenotools/*.so` when that folder exists; `android-dev/setup/adrenotools.sh` stages it.

**Untested:** whether R2's Mesa base contains MR 44838 (KGSL zero-timeout waits). See STATUS, device lost.

## Integer math without shaderInt64

**What.** The stock driver has no `shaderInt64`. Without it, the translator now writes every 64-bit integer operation
as 32-bit pairs: global-memory addresses (LDG / STG / ATOM: a carry into the high word, inside the window when it is
0), IADD32I / IADD3 carry chains (`uaddCarry`), and SHF (prelude `fsr` / `fsl`). The form is chosen from the device
feature and is part of the translation cache key.

**Evidence.** On the stock driver it fixed the white text boxes (labels, subtitles, banners, dialogue); the 3D
corruption stayed, which is the driver's. Offline: 102 synthetic op shaders × 3 stages validate with glslang and
spirv-val (Vulkan 1.3) and none uses the Int64 capability; `fsr` / `fsl`, carries and address checks agree with a
64-bit reference on 500k random cases; 331 dumped Z-A shaders translated with the 32-bit form validate.

**Where.** `runtime/nvn/maxwell/translate.c` `emit_gaddr`, global `maxwell_int64` (set in `exec.c` `vk_exec_init`);
`shader.h` preludes. Switch `LARECOMP_VK_INT64=0/1` forces either form.

## Thread placement: the biggest win

Four changes, measured one by one. Together they took the late-game street from 27.7 fps (p95 41.5 ms) to 29.9 fps
(p95 33.4 ms).

**1. The UI thread polls instead of busy-waiting.** On Android, `SDL_WaitEventTimeout` never sleeps: each pump queues
a sentinel that wakes it at once, so one `SDLThread` sat at 100 % of a big core all session, even in menus. Now:
`SDL_PollEvent` plus a 4 ms sleep (from EoWRecomp).
- Evidence: the UI thread 100 % → 2 % of a core; app CPU −92 points in menus, −116 in the street; hottest CPU zone in
  menus 92 → 80 °C. fps unchanged.
- Where: `runtime/nvn/vulkan/device.c` (UI loop); `LARECOMP_UI_POLL=0` restores SDL's wait.

**2. Render thread on the X3** (`LARECOMP_AFFINITY=render`, the default). Cores are sorted by top frequency: the
render thread goes on the fastest core, the game thread on the next tier.
- Evidence: render CPU per frame 6.1 → 3.7 ms (intro) and 8.1 → 5.3 ms (street), reproduced in a second run. The X3
  is about 1.45× an A715 per thread. Same app and system CPU; game thread +0.4 ms. Mode `x3` (game thread on the X3)
  gained nothing.
- Where: `runtime/platform/android.c` `aff_scan`, `plat_thread_perf_core`.

**3. The recorder off the X3.** A simpleperf profile (20 s, off-CPU included) of the late-game street showed that the
render thread, the recorder and every helper the render thread started all ran on the single X3 (affinity is
inherited). The render thread was runnable-but-not-running 33 % of the time; 64 % of its off-CPU time was a futex wake
of the recorder, which then preempted it on the same core (about 21k context switches a second). The game thread waited
12.3 s of 20 for translation. Per-draw cost (about 6.5 µs) was not the problem; the latency was.
- Evidence: recorder on the big cores alone: 25.5 → 28.3 fps, the game's wait for translation 29.5 → 13.1 ms a frame.
- Where: `android.c` `plat_thread_perf_core` role `PLAT_PERF_RECORDER`, `record.c` `run()`;
  `LARECOMP_AFFINITY_RECORDER=fast|big|0`.

**4. The render thread's helpers off the X3** (pipeline and shader build workers, texture unpack helpers, the
pipeline-cache saver).
- Where: `android.c` `plat_thread_worker`, called (weakly linked) from `exec.c` workers, `textures.c`
  `unpack_helper`, `device.c` `pipeline_cache_saver`; `LARECOMP_AFFINITY_WORKERS=0` leaves them inherited.

Combined A/B, late-game street, 100 s of camera turns and walking, warm cache:

| Run | fps | p50 / p95 ms | game waits for translation | render CPU | frames > 50 ms |
|---|---|---|---|---|---|
| before | 27.7 | 35.2 / 41.5 | 13.7 ms | 13.1 ms | 34 |
| recorder big + wake 64 KB + workers off | 29.7 | 33.3 / 35.7 | 2.2 ms | 10.2 ms | 9 |
| + render fast paths | **29.8** | **33.3 / 33.4** | **1.1 ms** | **9.3 ms** | **6** |

Hitch causes in the same windows (frames over 50 ms): translation latency 29 → 3, texture uploads on the render thread
4 → 1. Threads are named (`za-game`, `za-render`, `za-recorder`, `za-vsync`, `za-guest`, `za-build`, `za-shader`,
`za-unpack`, `za-pcache`) so `top -H` and simpleperf can tell them apart.

## Recorder wake threshold

**What.** A sleeping recorder is woken only once 64 KB of packets wait. Submissions, presents, waits and a full ring
still wake it at once.

**Evidence.** Alone: 25.5 → 27.6 fps in the cold-cache chain. Part of the combined row above.

**Where.** `runtime/nvn/vulkan/record.c` `publish_at` (`wake_bytes`); `LARECOMP_VK_RECORDER_WAKE_KB` (64 on Android,
0 elsewhere).

## Render-thread fast paths

**What.** Four render-thread shortcuts, bits of `LARECOMP_RENDER_OPT` (15 on Android, 0 elsewhere): 1 the 512-byte
fragment-shader hash once per program instead of on every shader change; 2 `ib_overrun`'s index scan once per index
range and frame; 4 prefetch of the next command records in `exec_list`; 8 `stencil_slots`' answer kept while a stage's
texture handles, pool and list repeat.

**Evidence.** Render CPU 10.2 → 9.3 ms a frame, p95 35.7 → 33.4 ms, frames over 50 ms 9 → 6 (table above). The
profile behind it: `exec_list` 28 %, futex wakes 11.5 %, the fragment-shader hash 3.5 %, `stencil_slots` 4.3 %, the
index scan 4.6 %.

**Where.** `runtime/nvn/vulkan/exec.c` (`Prog.fsh`, `ib_overrun`, `exec_list`, `stencil_slots`).

## Translator: loops and bounded jump tables

**What.** Two translator bugs kept the three culling compute shaders in the slow `switch (pc)` state machine:
- each BRX jump table was read "until a word isn't an instruction", so it ran on through every later table: a switch
  got the cases of all later switches (23 targets instead of 3). Tables now end where the next BRX's table starts;
- the straight-line form had no loops. A backward branch (BRA, SYNC / BRK / CONT, a jump-table entry) now becomes
  `do { ... } while (tgt == header && ++lguard < 100000u)`; overlapping regions merge into one multi-header loop;
  PCNT / CONT are handled; SSY / PBK are one ordered stack, as on the hardware (a BRK out of an SSY region drops the
  stale SSY entry).

**Evidence.** New-save street walk:

| | GPU per frame | culling (3 dispatches) |
|---|---|---|
| before (`LARECOMP_SHADER_FLAT=1`) | 15.6 ms (p95 19.0) | 3.61 ms |
| after (`=2`) | **12.4 ms** (p95 14.3) | **0.54 ms** |

The largest culling shader went from 26,591 to 14,475 instructions and stopped spilling (99 / 822 spill stores /
loads before). Same picture, same draw counts (G-buffer 138 / 139, shadows 119 / 119). Offline, all 9 state-machine
shaders of a dump flatten and validate.

**Where.** `runtime/nvn/maxwell/translate.c` `brx_table_off`, `glsl_flatten`; default set in `exec.c`
`vk_exec_init` (`maxwell_flat_default = 2` on Android). `LARECOMP_SHADER_FLAT=1` gives the old form;
`LARECOMP_SHADER_FLAT_LOG=1` prints the tables and why a shader keeps the state machine. Offline translator: dump
inputs with `LARECOMP_SAVE_BLOB=1`.

## GPU profiler: idle time was charged to passes

**What.** `[gpuprof]` rows run from one mark to the next. The last pass of each submission was charged with the GPU's
idle gap until the next submission. The profiler now puts a "(submission end)" mark at every submit.

**Evidence.** The apparent 18.6 ms depth-only pass and 13.2 ms colour pass of the first profiles were idle time. The
real street walk is 15.5–15.7 ms of GPU busy, which the pace counter and Turnip's u_trace agree on.

**Where.** `exec.c` (marks at submit), `device.c` (`LARECOMP_GPUPROF_AT`, `LARECOMP_GPUPROF_EVERY`);
`LARECOMP_GPUPROF_DRAWS` adds a row per draw.

## NMin / NMax / NClamp

**What.** The translator wrote fmin / fmax / saturate with explicit NaN tests. They are now GLSL.std.450 NMin / NMax /
NClamp (through `GL_EXT_spirv_intrinsics`), which have the same NaN rules; Mesa lowers them to exact min / max.

**Evidence.** Street GPU 12.4 → **12.0 ms** (p95 14.5 → 13.9): lighting 2.50 → 2.34 ms, final post 1.33 → 1.18 ms.
Same screenshots. Offline: lighting −9.8 % instructions, 109 fragment shaders −5.3 %.

**Where.** `translate.c` `glsl_minmax_nmin`, global `maxwell_nmin` (set in `exec.c` `vk_exec_init`; part of the
translation cache key). `LARECOMP_SHADER_NMIN=0` gives the old form.

## ClearBuffer and ReportValue

**What.** `nvnCommandBufferClearBuffer` was recorded (`cmdbuf.c`) but the executor had no case for it: it was
silently skipped. `nvnCommandBufferReportValue` was a no-op. Both now write in GPU order inside the guest memory
import (a copy in the command stream with barriers on both sides), and on the CPU elsewhere.

**Evidence.** Correctness only: Z-A did not call either in the scenes logged (a log line marks the first call), so
neither was the device-lost cause.

**Where.** `runtime/nvn/vulkan/exec.c` `clear_buffer`, `report_value` (→ `gpu_fill`); `runtime/nvn/cmdbuf.c`,
`nvn.h` `CMD_REPORT_VALUE`. `LARECOMP_CLEAR_BUFFER=0`, `LARECOMP_REPORT_VALUE=0` give the old behaviour.

Also audited, the other translation-time guest writes: `UpdateUniformBuffer` (memcpy into a per-frame 16-byte UBO),
`ReportCounter` (CPU timestamps), `CopyBufferToBuffer` (flush, then CPU memmove). Still no-ops in `cmdbuf.c`:
ResetCounter, Pause/ResumeTransformFeedback, SetShaderScratchMemory, DrawTexture, ResolveDepthBuffer.

## Indirect-args clamp

**What.** The device-lost safety net. Every parameter block a draw reads from GPU memory is remembered. When compute
work ran since the last pass, a small compute pass before the next render pass clamps, in place, instance counts over
the cap to the cap, and gives blocks with a vertex / index count over 4M zero instances.

**Evidence.** GPU 20.5 ms a frame with it vs 19.1–20.1 without (within noise). `LARECOMP_ARGS_CLAMP=1` makes every
street tree, lamp post and their shadows vanish (one instance each), which proves it reaches the real blocks. With the
default cap, no visible change. 24-minute soak: no device lost. Root cause: see [STATUS.md](STATUS.md#1-device-lost-in-the-late-game-street-root-cause-open-a-safety-net-is-on).

**Where.** `exec.c` `ac_note`, `ac_run`; `LARECOMP_ARGS_CLAMP=<cap>` (65536; 0 = off).

## Pacing: the panel is 59.65 Hz

**What.** The panel refreshes every 16.765 ms (SurfaceFlinger present times), not the 16.667 ms it reports. The
game's only frame clock on Z-A is `present_pace` in `objects.c`, which slept to an exact 33.333 ms grid. Each frame
therefore arrived 0.2 ms early relative to the panel and crossed a refresh boundary every **2.9 s**; at each crossing
one frame was shown for one refresh, often in a 1-3-1 burst. That was the "bit of judder at 30".

**Fix.** On a display within ±5 % of 60 Hz, `present_pace` waits on the display's own period from
`plat_display_vsync` (Android Choreographer: the measured period once 120 refreshes are in, the mode's before), with
its phase held where it first locked and slewed at most 0.25 ms a frame. Without a display vsync it keeps 16.667 ms.

**Evidence.** Late-game street walk, SurfaceFlinger present times:

| | fps on screen | shown 1 refresh | shown 2 | shown 3 | cadence changes / min | ≥ 50 ms |
|---|---|---|---|---|---|---|
| before | 30.00 | 32 | 1709 | 12 | 81 | 12 |
| after | 29.82 | **0** | 1350 | **0** | **0** | **0** |

29.82 fps is the panel's real half rate. The world steps per frame are unchanged, so the game runs 0.6 % slower in
wall-clock time: imperceptible [inference].

**Where.** `runtime/nvn/objects.c` `present_pace`; `runtime/platform/android.c` `plat_display_vsync` (Choreographer
tracker). `LARECOMP_PRESENT_LOCK=0` restores the fixed grid.

**Why the old vsync lock never worked.** Z-A's SDK is guest code: only 13 imports are bound to host stubs, and
`nn::vi` isn't one. `nn::vi::GetDisplayVsyncEvent` is never called, so `runtime/hle/vi.c`'s timer thread never starts,
and `LARECOMP_VSYNC_LOCK` (which locks that timer) had nothing to lock. Earlier "inconclusive" lock runs were runs with
the lock off. `vi.c` now logs whether its lock engaged.

## The 45 fps mode: removed

**What.** The upstream 45 fps option (`fps45`) ran the game's 30 fps machinery over a 90 Hz vi.c vsync. It is gone:
the menu offers 30 / 60, an old ini's `fps45` line is read and dropped.

**Why.**
- It never produced 45 on Z-A: `present_pace` caps presents at 30 a second, and vi.c's 90 Hz timer never runs (see
  above). Measured identical to 30 (fps, cadence, speed) on the street walk: 30.00 fps on screen either way.
- It could not be right even if it did: the game's world step is a whole number of 1/60 s units per frame (the
  frame clock compares elapsed time against 16.67 / 33.33 / 50 / 66.67 ms thresholds). At 2 steps a frame a real 45
  would run the world at 1.5×, and 40 at 1.33×. A correct 40 or 45 needs fractional steps: patching every subsystem's
  dt in the game.
- A steady 45 on a 60 Hz panel is impossible anyway: 22.2 ms frames alternate 1 and 2 refreshes.
- It was not the cause of the audio report either: audio is clocked by `audout` buffer releases, not by frames.

**Where.** `runtime/hle/framepace.c` `rt_za_frame_cfg`, `runtime/settings.c`, `runtime/ui/menu.mm`. The game's
frame-mode hook is `tools/za_patches.py` `ZA_BUILDS`.

## Background pause and the clock freeze

**What.** Before: in the background the game kept running at 9–16 fps with `VK_ERROR_SURFACE_LOST_KHR` every frame
(36 ms of render CPU a frame), and after coming back it **never showed a frame again**: the Vulkan surface of the
destroyed Android window was never remade. Now:
- an SDL event watch (WILL/DID_ENTER_BACKGROUND / FOREGROUND) sets `g_app_background` (`runtime/hle/vi.c`);
- the game thread waits in `nvnQueuePresentTexture` before its next present (`objects.c` `background_wait`);
- the audio feeders stop (`runtime/services/audio.c`); the hang watchdogs stay quiet;
- the game's clock `rt_ticks` stops, and resumes from the same tick (`runtime/kernel.c` `rt_pause_clock`);
- on return, `vk_frame_begin` sees the window's new `ANativeWindow` and makes a new surface and swapchain
  (`device.c` `recreate_surface`); it does the same on `VK_ERROR_SURFACE_LOST_KHR`.

**Evidence.** In the background: about 1–2 % of one core (114 CPU ticks in 50 s), against about 200 % in the
foreground. **The clock freeze is required:** without it, 2/2 runs came back at 30 fps and then stopped presenting
about 4 s later ("[hang] no new frame for 20 s"), the game reacting to its clock jumping 45–120 s [inference]. With
it: 29.8–29.9 fps to the end of the run, no hang, no device lost.

## Thermal

**What.** On the Thor, `dumpsys thermalservice` says "HAL Ready: false": `AThermal_getThermalHeadroom` returns NaN
and upstream's resolution stepping never ran (a 4-minute run sat at 93–95 °C CPU). After 5 s of NaN the runtime now
computes the same headroom from the kernel's thermal zones: 18 CPU zones (`cpu-*`, `cpuss-*`), 8 GPU (`gpuss-*`) and
1 skin (`xo-therm`). The severe point of each group is its lowest passive trip (CPU 108, GPU 95, skin 78 °C), on
Android's scale: (T − (severe − 30)) / 30. CPU and GPU use a 5 s mean, the skin a 10 s least-squares forecast.

**Evidence.** The street sits at headroom 0.54–0.57 (CPU 94–95 °C), below the 0.8 step-down point, which matches the
kernel not throttling there. A first version also forecast the CPU zones: an 88 → 95 °C load spike read 0.84 and
stepped down for nothing; hence the mean. In a 15-minute play log the scale stayed at 100 %.

**Where.** `runtime/platform/android.c` `tz_scan`, `tz_headroom`. `LARECOMP_THERMAL=0|hal|sysfs`,
`LARECOMP_THERMAL_CPU / _GPU / _SKIN=<°C>`. The log also prints the AYN `Settings.System` modes
(`performance_mode`, `fan_mode`, `fan_speed`, refresh rates), read through JNI.

## Pipeline cache

**What.** Saved every 30 s while it grows, instead of every 3 minutes (EoWRecomp's interval, all platforms), on its
own thread, off the X3.

**Evidence.** A save costs 0.2–8.6 ms on the saver thread (12 KB → 4.3 MB); p95 unchanged. A session killed after
about 100 s left 18 KB with the 3-minute save and 3.3 MB with the 30 s save. The next start's hitch window was the same
in both (1 % of frames over 34 ms, against 7–10 % cold): Turnip's own disk cache covers relaunches, so the shorter
interval mainly protects against losing work when the app is killed.

**Where.** `runtime/nvn/vulkan/device.c` `pipeline_cache_saver`; `LARECOMP_PCACHE_SAVE_S=<s>`.

## Display: fullscreen, top screen, frame-rate hint

- **Fullscreen by default on Android** (`runtime/settings.c`): the swapchain is 1920×1080 instead of 1920×970 with a
  pillarboxed picture. An existing ini keeps its stored `fullscreen = 0`.
- **Top screen only:** a windowless `ZALaunchActivity` is the launcher entry and starts the game with
  `setLaunchDisplayId(DEFAULT_DISPLAY)` (`tools/linux/android-port/make_project.sh`). Launching from the bottom screen
  was not tested.
- **Frame-rate hint:** at 30 fps the game now asks `ANativeWindow_setFrameRate(30)` (`android.c`
  `plat_frame_rate_hint`). Measured no difference on the Thor (60 Hz cap); harmless. `LARECOMP_DISPLAY_RATE=0`
  restores upstream's no-preference.
- **Resolution names:** the game's "1x" is 1280×720, *lower* than 75 % (1440×810). "Match display" (1080p) holds 30
  fps on the new-save street at 20.2 ms GPU.

## Textures: what is and isn't a texture problem

- **"Textures load in pieces" is not an upload problem.** `LARECOMP_TEX_LOG_LATE=1` over 8400 frames of the late-game
  street: 11,538 first uploads and **0 later data changes** of non-dynamic textures. About 18 % of first uploads are
  mostly zero, and since none change later, that is their real content.
- The visible pop-in is the cold pipeline cache (draws skipped while pipelines build) and the game's own LOD and draw
  distance (`za_lod`, see [MODDING.md](MODDING.md)).
- This kernel (5.15) has no `PAGEMAP_SCAN`, so page write tracking is off: every texture is hashed at its first use in
  a frame (1.5 ms a frame on the late-game street), and the vertex cache's page tracking is off too.
- HD texture packs and dumps were broken on Android (relative paths that SDL resolves to internal storage or APK
  assets; a dump queue nobody drained). Fixed: details in [MODDING.md](MODDING.md#texture-packs).
- NVN RGB565 (0x3C) is mapped to `B5G6R5_UNORM_PACK16` (`textures.c` `rgb565()`, from EoWRecomp 37f152d). SUST.B
  (bindless surface stores) is not used by Z-A's shaders, so it was not ported.

## Audio path

As measured: the game's Wwise engine (three guest worker threads on the big cores, 2.5–5.7 % CPU each) →
`AppendAudioOutBuffer` (1024 frames s16 stereo 48 kHz, 3–4 queued) → `audout_thread` (`runtime/services/audio.c`, puts
one buffer into an SDL stream and waits until it holds ≤ 2 device periods) → SDL3 AAudio (960 frames a period, 40 ms
release target) → AudioFlinger's normal mixer (not the FastMixer; about 122 ms latency). No limiter or ducking on
Android's side. The game does not use audren, and never opens an Opus decoder. Our side only copies s16 at gain 1.0,
so it cannot wrap or produce NaN. `[audout] device stalled` appears only at focus loss or an output-route change
(`AAUDIO_ERROR_DISCONNECTED`, then a reopen). Diagnostics: `LARECOMP_AUDIO_STATS=1`; see
[STATUS.md](STATUS.md#2-audio-cuts-in-very-loud-scenes-not-reproduced).

## GPU cost breakdown

New-save street walk, per frame, after the fixes above (12.0–12.5 ms total): G-buffer 4.0 ms (GMEM, 9 bins, 4 render
targets + depth), lighting 2.3–2.5 ms (one full-screen draw, sysmem), final post 1.2–1.3 ms, window chain 0.8–0.9 ms
(a clear-only pass 81 µs, SGSR 229 µs, present copy 136 µs, pre-rotation into the non-UBWC portrait swapchain image
372 µs), UI 0.65 ms, AO 0.6 ms, sun-shadow cascades 0.45–0.6 ms, culling 0.5 ms.

- Turnip's autotune already picks GMEM or sysmem correctly (`TU_DEBUG=sysmem`: 15.7 ms, `=gmem`: 16.7, default 15.5).
- UBWC is on for 81 % of passes; the rest are 8-bit targets (the A740 has no 8bpp UBWC), the swapchain and tiny mips.
- Concurrent binning is disabled in the R2 driver, so barriers block no binning overlap.
- The late-game street costs about 21 ms of GPU at 75 %: 30 fps has about 12 ms of headroom.
- Shadow resolution: 0.5 vs 1.0 on the late-game street is hardly visible and costs about 1 ms; on the new-save
  street 0.75 / 1.0 cost nothing measurable. `za_lod = 2` saves 0.2 ms.

## Working with the device

- `adb push` straight into `Android/data/org.recomp.za/files/` is refused (`secure_mkdirs: Operation not
  permitted`). Push to `/data/local/tmp`, `chmod a+rX`, then `run-as org.recomp.za cp -r`. The 6 GB romfs takes about
  2 min 40 s over USB. `android-dev/setup/push-game-files.sh` does this.
- After a GPU timeout, `adb screencap` and `screenrecord` can return all-black frames until the device reboots. Use
  in-app capture instead: `LARECOMP_SHOT_AT` or `LARECOMP_SCREENSHOT_EVERY` (+ `_FROM`) write to `files/build/frames`
  (`android-dev/device/pull-frames.sh`). Each in-app shot costs 200–260 ms on the render thread: keep shot frames out
  of timing windows.
- On-screen timing needs SurfaceFlinger's present times (`dumpsys SurfaceFlinger --latency`, sampled every few
  seconds and joined): the game-side frame clock cannot see the panel's real cadence. `android-dev/analysis/pace.py`.
- When an A/B flips something every N frames and also screenshots every M frames, pick offsets that are not
  multiples of each other: a shot taken on the flip frame still shows the old state.
- One device user at a time: scripts take the device lock (`PZ_DEVICE_LOCK` in `android-dev/config.sh`); builds take
  `/tmp/pokez-build.lock`.

## What did not work

Measured and dropped, or proven inert. Do not retry these without a new reason.

| Idea | Result | Where / switch |
|---|---|---|
| Lazy render passes (begin at the first command; full-area clears as load ops; drop empty passes) | About 7,200 clears became load ops, no pass dropped: GPU 15.6 vs 15.6 ms. No gain. | `exec.c`, `LARECOMP_VK_LAZY_PASS=1` (opt-in, kept) |
| Completion marker instead of timeline semaphores (EoWRecomp 99834cd) | 30 fps identical; 60 fps within noise. | `exec.c`, `LARECOMP_VK_SEQ_POLL=1` (opt-in, kept) |
| `LARECOMP_UBO_FLUSH=1` as a device-lost workaround (drain the GPU before each uniform update) | One run survived 270 s, a later one crashed at frame ~1150 with the same pattern. Not a workaround; costs fps (22–28). | upstream switch, `exec.c` |
| Uniform-buffer updates in GPU order (into compute-storage ranges) | Never fired on Z-A: the real update target is a 16-byte UBO inside a render pass. The run that "survived" with it had `LARECOMP_GPUPROF_EVERY=4`, which drains the GPU. | `exec.c` `ubo_gpu_write`, `LARECOMP_UBO_GPU_ORDER` |
| Any run with `LARECOMP_GPUPROF_EVERY` as proof of stability | The profiler drains the GPU every N frames and hides GPU-order races. | — |
| The old vsync lock | Inert on Z-A: vi.c's timer never runs (see Pacing). Replaced by the present-pace lock. | `vi.c`, `LARECOMP_VSYNC_LOCK` |
| 45 fps mode | Identical to 30; would be 1.5× speed if it worked. Removed. | see above |
| 40 fps on a 60 Hz panel (`fps45` + an 80 Hz vsync) | Stays at 30.0 fps. At 120 Hz it would fit the GPU budget but run at 1.33× speed. Removed with the 45 mode. | was `LARECOMP_FPS45_HZ` |
| armv8.5-a / Cortex-X3-tuned game code (`GENARCH`) | Game thread 2.2 ms either way; one of one runs livelocked ("no new frame for 20 s"). Generic build kept. | `Makefile` `GENARCH` |
| `AFFINITY=x3` (game thread on the X3) | No gain over `render`. | `android.c` |
| Guest memory by device address (BDA) | Hangs the stock driver at the first 3D scene. | `device.c`, `LARECOMP_VK_GMEM_BDA=1` |
| Forcing Turnip's GMEM or sysmem | Autotune is already right: forced GMEM +1.2 ms, sysmem +0.2 ms. | `TU_DEBUG=gmem/sysmem` |
| Frame-rate hint off / on | No difference on the 60 Hz-capped panel. | `LARECOMP_DISPLAY_RATE` |
| Qualcomm 863.1 package | Correct picture, GPU hang after about 3 minutes. | `LARECOMP_DRIVER=<path>` |
| Turnip 26.0.0 R8 | ir3 register-allocator crash on the largest culling shader. | — |
| Blanket `mediump` on the lighting shader | +9.5 % instructions, 6 → 4 waves (host compile). Needs typed float temporaries; the shader already has 19 registers and 10 waves on the device. | not committed |
| Higher shadow resolution as a fix for "bad shadows" | 0.5 vs 1.0 hardly differs on the late-game street; costs about 1 ms. Not the cause. | ini `shadow_resolution` |
| Faster texture uploads or hashes for world-load hitches | The load is the game's own (romfs reads); uploads are under 20 ms of a 400–600 ms load. | `textures.c` |
| ESRGAN-only upscales for texture packs | Clean edges, but fine ornament (railings, carvings) is erased. Needs a detail blend (`hd_pack.py`) or per-texture review. | see MODDING |
| 60 fps at 50 % (before the translator fixes) | 57.4 fps, p95 21.4 ms: did not hold. Worth re-measuring now. | ini `fps60` |
