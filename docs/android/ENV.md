# Switches: environment variables and ini keys

The runtime's switches are environment variables. An Android app has no environment of its own, so the runtime reads
`env.txt` from its data folder at start (`runtime/platform/android.c` `load_env_file`):

- path: `/storage/emulated/0/Android/data/org.recomp.za/files/env.txt`;
- one `NAME=value` per line, `#` starts a comment line;
- each line is logged to logcat (tag `ZARecomp`) as `env.txt: NAME=value`;
- a variable already set wins. Driver variables (`TU_DEBUG`, `MESA_*`) work too, because the file is read before the
  driver loads.

**The playable build runs with no `env.txt`.** Every default below that says "on" is what the player gets. Delete the
file after a test session. Presets for test runs are in `android-dev/presets/env-*.txt`.

Kinds: **feature** changes behaviour for players; **diagnostic** only logs or dumps; **test** deliberately changes
behaviour to probe a hypothesis (can give wrong results).

Why each feature exists, with numbers: [FINDINGS.md](FINDINGS.md).

## Added by this fork

### Driver and shader translation

| Variable | Default | Effect | Kind | Where |
|---|---|---|---|---|
| `LARECOMP_DRIVER` | the packaged `libvulkan_turnip.so` (`VOLK=1` builds) | Vulkan driver: a file name beside `libmain.so`, an absolute path to a driver unpacked in the app's internal storage (e.g. `/data/user/0/org.recomp.za/files/drv/<pkg>/libvulkan_freedreno.so`), or `system`. A missing or failing driver falls back to the system's. | feature | `vulkan/device.c` `custom_driver` |
| `LARECOMP_GPU_TURBO` | off | `1`: libadrenotools' KGSL turbo (GPU at its top clock; thermal limits still apply). | test | `device.c` |
| `LARECOMP_VK_INT64` | from the device's `shaderInt64` | `0` / `1` forces 32-bit pairs / 64-bit integer math in translated shaders. | test | `exec.c` `vk_exec_init`, `maxwell/translate.c` |
| `LARECOMP_SHADER_FLAT` | **2 on Android** (upstream: 1; 0 on AMD) | Translator form. `2`: structured loops and bounded jump tables; `1`: forward branches only (old form); `0`: state machine. Existing upstream switch, new value 2. | feature | `translate.c`, `shader.h` |
| `LARECOMP_SHADER_NMIN` | **on on Android**, off elsewhere | fmin / fmax / saturate as GLSL.std.450 NMin / NMax / NClamp. `0` / `1` forces. | feature | `exec.c` `vk_exec_init`, `translate.c` |
| `LARECOMP_SHADER_FLAT_LOG` | off | `1`: BRX tables and the reason a shader keeps the state machine. | diagnostic | `translate.c` |
| `LARECOMP_BRX_ALL_LABELS` | off | `1`: label every instruction as a possible jump-table target (the old, unbounded behaviour). | test | `translate.c` |
| `LARECOMP_SAVE_BLOB` | off | `1`: each translated shader's input (code plus 64 KB after it) to `build/glsl/*.mxb`, for the offline translator. `[mxb] prepared …` maps blobs to shaders. | diagnostic | `translate.c` `save_blob`, `exec.c` |
| `LARECOMP_SAVE_MSL` | off | Upstream switch; now also writes the SPIR-V given to the driver: `build/glsl/<addr>_<stage>.spv`. | diagnostic | `exec.c` |
| `LARECOMP_LOG_BUILDS` | off | `1`: `[build] +` / `[build] -` at every pipeline build's start and end: a driver compiler crash is the `+` without a `-`. | diagnostic | `exec.c` |
| `LARECOMP_VK_EXEC_STATS` | off | `1`: the driver compiler's statistics per pipeline (instructions, registers, spills, waves) as `[stats]` lines. | diagnostic | `device.c` `vk_exec_stats_log` |
| `LARECOMP_VK_GMEM_BDA` | off | `1`: guest memory through the AHardwareBuffer's device address instead of one storage buffer. Hangs the stock driver. | test | `device.c`, `exec.c`, `shaders.c` |

### Vulkan backend correctness

| Variable | Default | Effect | Kind | Where |
|---|---|---|---|---|
| `LARECOMP_ARGS_CLAMP` | **65536** (all Vulkan platforms) | GPU-side clamp of GPU-filled indirect draw parameters before each render pass after compute: instance counts above the cap become the cap; blocks with a vertex / index count over 4M draw nothing. `0`: off; `1`: proof run (street trees vanish). The device-lost safety net. | feature | `exec.c` `ac_note`, `ac_run` |
| `LARECOMP_CLEAR_BUFFER` | on | `nvnCommandBufferClearBuffer` executed in GPU order. `0`: skipped, as upstream. | feature | `exec.c` `clear_buffer` |
| `LARECOMP_REPORT_VALUE` | on | `nvnCommandBufferReportValue` recorded and executed as a 32-bit write in GPU order. `0`: the old no-op. | feature | `cmdbuf.c`, `exec.c` `report_value` |
| `LARECOMP_UBO_GPU_ORDER` | 1 | Uniform updates into ranges a compute dispatch bound as storage in the last 300 frames become GPU copies. `0`: memcpy only; `2`: both. Never fires on Z-A. | test | `exec.c` `ubo_gpu_write` |
| `LARECOMP_LOG_UBO` | off | `1`: every 300 frames, the distinct `UpdateUniformBuffer` targets and how they were taken. | diagnostic | `exec.c` |
| `LARECOMP_TEST_FENCE_EARLY` | 0 | `<n>`: fence waits return once the submission n before is done. Test only. | test | `exec.c` `renderer_wait_seq` |
| `LARECOMP_VK_PASS_BARRIER` | on | `0`: drops the barrier before each pass: an upper bound of what barrier trimming could gain. Results can be wrong. Not measured. | test | `exec.c` |
| `LARECOMP_VK_LAZY_PASS` | off | `1`: a render pass begins at its first command; a full-area clear before any draw becomes the load op. Measured no gain. | test | `exec.c` |
| `LARECOMP_VK_SEQ_POLL` | off | `1`: a host-visible completion marker instead of timeline semaphores (EoWRecomp 99834cd). Measured no difference. | test | `exec.c` |
| `LARECOMP_VK_SEQ_POLL_US` | 500 | Poll interval for `LARECOMP_VK_SEQ_POLL`, in µs. | test | `exec.c` |

### Threads and render-thread CPU (Android)

| Variable | Default | Effect | Kind | Where |
|---|---|---|---|---|
| `LARECOMP_UI_POLL` | **on** | The SDL UI thread polls and sleeps 4 ms instead of `SDL_WaitEventTimeout`, which spins a core on Android. `0`: SDL's wait. | feature | `vulkan/device.c` |
| `LARECOMP_AFFINITY` | **`render`** | Core placement, cores sorted by top frequency. `render`: render thread on the fastest core, game thread on the next tier. `x3`: game thread alone on the fastest core. `big`: game and render threads on fast + big. `0`: Android decides. | feature | `platform/android.c` `aff_scan`, `plat_thread_perf_core` |
| `LARECOMP_AFFINITY_RECORDER` | big cores (never the render thread's core) | `fast`: back on the fastest core (as before); `big`; `0`: Android decides. | feature | `android.c`, `vulkan/record.c` |
| `LARECOMP_AFFINITY_WORKERS` | **on** | The render thread's helpers (`za-build`, `za-shader`, `za-unpack`, `za-pcache`) on every core but the fastest. `0`: inherited, as before. | feature | `android.c` `plat_thread_worker` |
| `LARECOMP_VK_RECORDER_WAKE_KB` | **64 on Android**, 0 elsewhere | A sleeping recorder is woken once this many KB of packets wait (always at submissions, presents, waits, ring full). | feature | `record.c` `publish_at` |
| `LARECOMP_RENDER_OPT` | **15 on Android**, 0 elsewhere | Bits: 1 fragment-shader hash once per program; 2 index scan once per range and frame; 4 command-record prefetch; 8 `stencil_slots` memo. | feature | `exec.c` |
| `LARECOMP_SLOW_LINES` | 20 | `[slow]` lines per minute; each line now carries its wall-clock time. | diagnostic | `nvn/slowframe.c` |

### Pacing, display, power

| Variable | Default | Effect | Kind | Where |
|---|---|---|---|---|
| `LARECOMP_PRESENT_LOCK` | **on** | On a display within ±5 % of 60 Hz, presents are paced to the display's measured refresh (Choreographer), phase-locked. `0`: the fixed 16.667 ms grid. | feature | `nvn/objects.c` `present_pace`, `android.c` `plat_display_vsync` |
| `LARECOMP_DISPLAY_RATE` | the game's rate (30 at 30 fps) | Frame-rate hint to the display (`ANativeWindow_setFrameRate`). `<hz>`: always that rate; `0`: no preference at 30 (upstream). | feature | `android.c` `plat_frame_rate_hint` |
| `LARECOMP_VSYNC_LOCK` | off | `<µs>`: locks vi.c's guest vsync timer to the display's. **Inert on Z-A**: the game never starts that timer. vi.c logs whether it engaged. | test | `runtime/hle/vi.c` |
| `LARECOMP_THERMAL` | headroom, then sysfs after 5 s of NaN | `0`: off; `hal`: Android's headroom only; `sysfs`: the kernel zones only. | feature | `android.c` `tz_scan`, `tz_headroom` |
| `LARECOMP_THERMAL_CPU` / `_GPU` / `_SKIN` | lowest passive trip per group (Thor: 108 / 95 / 78 °C) | Another severe point, in °C. | test | `android.c` |
| `LARECOMP_PCACHE_SAVE_S` | 30 | Pipeline-cache save interval while it grows, in seconds (was 180). | feature | `device.c` `pipeline_cache_saver` |

### Textures, audio, capture, GPU profiling

| Variable | Default | Effect | Kind | Where |
|---|---|---|---|---|
| `LARECOMP_TEX_DUMP` | off (ini `tex_dump`) | Upstream switch, fixed on Android: dumps each 2D colour texture to `files/textures/dump/<hash>.png`. | feature | `vulkan/textures.c` |
| `LARECOMP_TEX_HD_FLIP` | off | `<frame>[,<every>]`: switches HD packs at that frame, as F1 would, then every `<every>` frames. For A/B shots. | test | `textures.c` |
| `LARECOMP_TEX_LOG_LATE` | off | `1`: `[texlate]` lines for textures whose data changed after their first upload. | diagnostic | `textures.c` `tex_note_late` |
| `LARECOMP_AUDIO_STATS` | off | `1`: one `[audstat]` line per audio out per second: buffers and frames appended, longest append gap, game queue depth, output starvation, SDL device underruns, stream margin, peak / clipped / RMS / silent buffers. Costs one branch when off. | diagnostic | `services/audio.c` |
| `LARECOMP_GPUPROF_EVERY` | 0 | `<n>`: GPU time per pass every n-th frame (`[gpuprof]`). Drains the GPU: hides GPU-order races. | diagnostic | `device.c`, `exec.c` |
| `LARECOMP_GPUPROF_DRAWS` | off | `<n>` (with `_EVERY`): a row per draw in passes with at least n colour targets. Use with `TU_DEBUG=sysmem`. | diagnostic | `exec.c` |
| `LARECOMP_GPUPROF_AT` | off | Upstream switch; rows now end at a "(submission end)" mark and labels carry area, layers, attachments, load ops and shader ids. | diagnostic | `device.c`, `exec.c` |
| `LARECOMP_SHOT_AT`, `LARECOMP_SCREENSHOT_EVERY`, `LARECOMP_SCREENSHOT_FROM` | off | Upstream in-app screenshots to `files/build/frames`; fixed on Android (the path is made absolute). Each shot costs 200–260 ms on the render thread. | diagnostic | `device.c` `vk_save_png` |

### Build-time

| Switch | Default | Effect | Where |
|---|---|---|---|
| `VOLK=1`, `ADRENOTOOLS=<src dir>` (make) | off | Build `libmain.so` on volk and load the driver through libadrenotools. Defines `LARECOMP_VOLK`. | `Makefile`, `device.c`, `volk_impl.c` |
| `GENARCH=<flags>` (make) | empty (generic) | Game-code CPU tuning, now applied on Android too. armv8.5 / X3 tuning showed no gain and hung once. | `Makefile` |

## Upstream switches we rely on

| Variable | Use |
|---|---|
| `LARECOMP_LOG_PACE` | `[pace]` stats per interval: fps, frame times, game / render CPU, GPU ms, queue waits. The basis of most numbers in these pages. |
| `LARECOMP_VK_PROF` | Render-thread phase times (exec, draws, sets, textures, uploads). Adds overhead. |
| `LARECOMP_FG_TIMING` | On-screen times through `VK_GOOGLE_display_timing`. |
| `LARECOMP_LOG_SKIPS` | Skipped draws and dispatches per interval by reason (`compute building`, `pipeline building`, …) and the top offenders. The starting point of the device-lost lead. |
| `LARECOMP_GPU_WRITES` | `1`: remembers which GPU operations (compute storage, copies, transform feedback, counters) may have written each guest range, so an overwritten indirect-parameter block is logged with the operations that wrote over it. It showed the culling list overrunning the draw parameters. |
| `LARECOMP_UBO_FLUSH` | Drains the GPU before each uniform update. Not a device-lost workaround (see FINDINGS). |
| `LARECOMP_SKIP_CS` | `<hex>`: skips one compute shader by code address. |
| `LARECOMP_AUDOUT_WAV` | `<absolute path>`: raw capture of the game's audio output (about 11 MB a minute). |
| `LARECOMP_FPS`, `LARECOMP_FPS60_DYNAMIC` | Force 30 / 60; turn off the 60 fps mode's fallback to 30. |
| `LARECOMP_TOUCH` | `0`: no touch overlay. |

## Driver variables (Turnip)

Set them in `env.txt` like the others.

| Variable | Use |
|---|---|
| `TU_DEBUG=sysmem` / `gmem` / `nobin` / `flushall` / `syncdraw` / `perf` | Force a rendering mode or serialise work, for hang bisects and A/B. `perf` logs why a pass went sysmem. |
| `TU_DEBUG_FILE=<path>` | Flip some `TU_DEBUG` options at run time without a relaunch. |
| `TU_AUTOTUNE_ALGO=prefer_sysmem` / `prefer_gmem` | Bias the per-pass choice. Autotune was already right on the street. |
| `MESA_GPU_TRACES=print`, `MESA_GPU_TRACEFILE=<path>` | Turnip u_trace per-pass GPU timings (`android-dev/analysis/utrace.py`). |

## `larecomp.ini` keys that matter on the Thor

The file is `files/larecomp.ini`, written by the in-game menu (F1, the Back gesture, or Minus + Plus). The playable
values are in `android-dev/presets/ini-thor.ini`; see [STATUS.md](STATUS.md#the-playable-build).

| Key | Playable value | Notes |
|---|---|---|
| `resolution` | `75%` | Internal 1440×810. `1x` is the game's 1280×720 (lower). `50%`, `25%`, `1`…`4`, `dynamic`, `auto` also exist. |
| `upscaler` | `auto` (SGSR on Vulkan) | `bilinear` saves 0.5 ms of GPU. |
| `fps60` / `fps60_lock` | `0` / `0` | 60 fps mode, and keeping it below 60 (slow motion). Does not hold 60 on the late-game street. |
| `fps45` | (ignored) | **Removed by this fork**: read and dropped, no longer written. |
| `fullscreen` | `1` | **Default changed to 1 on Android** by this fork. An older ini keeps `0`: flip it. |
| `present_mode` | `mailbox` | `fifo` is the other choice. |
| `shadow_resolution` | `0.5` | `0.5`, `0.75`, `1`–`4`. 1.0 costs about 1 ms on the late-game street for no visible gain. |
| `shadow_distance` | `0` | 0 full, 1 reduced, 2 near cascade only, 3 characters only, 4 off. |
| `thermal_profile` | `0` | 0 cooler, 1 no cooler: the render-scale curve against thermal headroom. |
| `dynamic_res` | `off` | `game`: the game's own dynamic resolution. |
| `za_lod` | `0` | 0 the game's LOD; 1 "improved" (less pop-in, more GPU); 2 performance (−0.2 ms). |
| `za_dof_off`, `za_fxaa_off`, `za_bloom_off` | `0` | Turn the effect off; applied at once (`runtime/hle/za_tweaks.c`). |
| `tex_hd` | `1` | Use HD texture packs from `files/textures/hd/` when present. |
| `tex_dump` | `0` | `1`: dump textures (1–3 fps while an area loads). |
| `mods_order`, `mods_off` | empty | romfs mod order and disabled mods (`|`-separated names). See [MODDING.md](MODDING.md). |
