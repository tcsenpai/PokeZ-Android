# Status on the AYN Thor (2026-11-14)

What works, what was measured, the build to play with, and what is still open. The reasons behind each change are in
[FINDINGS.md](FINDINGS.md); the switches named here are in [ENV.md](ENV.md).

## Device

| | |
|---|---|
| Device | AYN Thor Max: Snapdragon 8 Gen 2 (SM8550), Adreno 740, Android 13 |
| CPU (AYN performance mode 2: clocks pinned) | cpu7 Cortex-X3 3.19 GHz; cpu3–6 A715/A710 2.80 GHz; cpu0–2 A510 2.02 GHz |
| GPU clock in that mode | 615 MHz |
| Top panel | 1920×1080 AMOLED. It refreshes every **16.765 ms (59.65 Hz)**, although it reports 60.000004. The system setting `peak_refresh_rate = 60` caps apps at 60 Hz; an app cannot override it. |
| Bottom panel | a second display; the launcher entry starts the game on the top one (`ZALaunchActivity`) |
| Game | Pokémon Legends: Z-A 2.0.2 (exefs build ID `B1F12FD9…`) |
| Driver | Mesa Turnip 27.0.0 R2, packaged in the APK, loaded through libadrenotools |

## What works

- Boot to gameplay with a correct picture: menus, intro, train, Lumiose streets, characters, text, dialogue.
- A locked 30 fps in the late-game street (numbers below), with presents paced to the panel's real refresh.
- The Thor's built-in pad: movement, camera, lock-on and send-out, menus, map, bag, NPC dialogue (scripted action run).
- In-game save and load, with an existing save copied onto the device.
- Going to the home screen and back: the game pauses, its clock stops, and it shows its picture again on return.
- Thermal stepping, through the sysfs fallback (the Thor's thermal HAL never becomes ready).
- HD texture packs (PNG or BC7 DDS) and texture dumps: see [MODDING.md](MODDING.md).
- RSS about 2.1–2.5 GB. The first game screen appears 6–12 s after PLAY.

## Measured performance

All runs: Light profile (75 % + SGSR, 30 fps, shadows 0.5), Turnip 27 R2, AYN performance mode 2, warm pipeline cache
unless noted. "fps" is the frame clock unless "on screen" says SurfaceFlinger present times.

| Scene / run | Result |
|---|---|
| New-save street walk, GPU | **12.0 ms a frame** (p95 13.9). Was 23.6 ms when Turnip 27 first rendered correctly; 15.6 after the profiler fix; 12.4 after the translator loop fix; 12.0 after NMin/NMax. |
| Late-game street, defaults, 100 s camera + walk | **29.9 fps, frame p50 33.3 / p95 33.4 ms**. GPU 21.0 ms, render thread 9.4 ms CPU, game waits 1.2 ms for translation. 7 frames over 50 ms, 4 of them the in-app screenshots the run asked for. |
| Late-game street, same scene before the thread work | 27.7 fps, p95 41.5 ms, 34 frames over 50 ms per 100 s |
| **Soak**: late-game street, 24 min of walking and camera turns | **No device lost**, no corrupted draw parameters. 2 s samples: **mean 29.9 fps, p5 29.4, min 27.3**. 86 frames over 50 ms in 24 min. End temperatures: CPU 94 °C, GPU 83 °C, skin 68 °C. (Build before the pacing fix.) |
| On-screen cadence, late-game street walk, after the pacing fix | 29.82 fps on screen (the panel's real half rate), **every frame shown for exactly 2 refreshes**: 0 one- or three-refresh intervals, 0 cadence changes a minute. Before: 32 one-refresh and 12 three-refresh intervals and 81 cadence changes a minute (one hiccup every 2.9 s). |
| Background and back (playable build) | about 2 % of one core while in the background; on return a new surface and 29.8–29.9 fps to the end of the run; no hang, no device lost |
| 60 fps, new-save street, 50 % resolution | 57.4 fps, p95 21.4 ms: does not hold. Measured **before** the translator fixes (−3.6 ms of GPU since); not re-measured. |

## The playable build

The build to keep on the device is `main` at the commit that adds the background clock freeze (the build was named
`pokez-final4`). `android-dev/device/restore.sh` puts the device back in this state.

- **APK:** built from `main` with `android-dev/build.sh`, Turnip 27.0.0 R2 packaged as the default driver
  (`android-dev/setup/adrenotools.sh` stages it).
- **No `env.txt`.** Every default listed as "default on Android" in [ENV.md](ENV.md) applies.
- **`larecomp.ini`** (`android-dev/presets/ini-thor.ini`): `resolution = 75%`, `upscaler = auto` (SGSR),
  `fps60 = 0`, `shadow_resolution = 0.5`, `shadow_distance = 0`, `present_mode = mailbox`, `fullscreen = 1`,
  `tex_hd = 1`, `dynamic_res = off`, `za_lod = 0`. An ini made by an older build keeps `fullscreen = 0`: flip it.
- **Device:** AYN performance mode 2, display at 60 Hz.
- After a test session, leave the playable APK installed, the player's save and ini in place, no `env.txt`, the app
  stopped.

## Open issues

### 1. Device lost in the late-game street (root cause open; a safety net is on)

**Symptom.** "ZA Recomp stopped because of an error" 10–60 s after spawning, even standing still. The kernel log has
`adreno-gen7-gmu: gpu timeout`: a submission ran too long, not a page fault.

**Mechanism (established).** A culling compute pass (three dispatches a frame) appends visible instances to a list
in a 256 KB storage buffer that also holds the indirect draw parameters it feeds. Sometimes the list's counter is not
reset: the list grows frame after frame, overwrites the parameter blocks, a draw asks for about 10⁹ instances, and the
GPU times out. With `LARECOMP_GPU_WRITES=1` the parameter blocks hold repeated instance ids and floats instead of draw
arguments, and `[ibguard]` lines report skipped draws and "instances over 16M".

**Reproduction history.** 6/6 runs crashed before a certain hour and 0/5 after it, on the same save, settings and
warm cache. The crashing runs ran at 19–23 fps after spawn, the later ones at 24–26 fps, so the scene itself differed
a little (crowd, time of day) [inference].

**Ruled out:** the translator's shader form (`LARECOMP_SHADER_FLAT=1` still crashed), CPU-cache coherency (an
uncached mapping still crashed), shader-cache warmth, the per-frame `UpdateUniformBuffer` (its real target is a
16-byte UBO, never a compute-storage range), fences returning early (`LARECOMP_TEST_FENCE_EARLY=26`: no crash),
`ClearBuffer` and `ReportValue` (Z-A never calls them in these scenes), and `LARECOMP_UBO_FLUSH=1` (it crashed too:
the run that survived with it was luck).

**Safety net (default on):** `LARECOMP_ARGS_CLAMP` (65536). Before each render pass that follows compute work, a small
compute pass clamps the GPU-filled instance counts. A bad frame then draws wrong for a frame instead of killing the
device. `LARECOMP_ARGS_CLAMP=1` makes every street tree and lamp post vanish, which proves it reaches the real blocks.

**Best lead (unchecked) [inference]: skipped compute dispatches.** `runtime/nvn/vulkan/exec.c` skips a compute
dispatch whose pipeline is still building (`SKIP_CS(... SK_CS_BUILDING)` in the dispatch path; the reason codes are
the `SK_*` enum). If the skipped dispatch is the pass that resets the list's counter, the counter keeps growing.
Against it: one warm-cache run crashed with 0 skips reported. Next steps:
1. Log every `SK_CS_BUILDING` / `SK_CS_FAILED` skip with its program and frame (`LARECOMP_LOG_SKIPS=1` is the
   starting point), and correlate with the first `[ibguard]` line.
2. Try waiting for the compute pipeline instead of skipping it (a test switch in that path), and check whether the
   `[ibguard]` pattern disappears.
3. Driver-side bisect if the above clears nothing: `TU_DEBUG=flushall`, `syncdraw`, `sysmem`, `nobin` in `env.txt`.
   Also try a Turnip build whose Mesa contains MR 44838 ("tu/kgsl: prevent indefinite wait times for 0 timeout
   waits", merged 2026-10-08).

**Related:** distant objects that vanish for one frame (seen in play) are probably the same list. In-app capture
drains the GPU and hides the race, so they were not classified further.

### 2. Audio cuts in very loud scenes (not reproduced)

**Report.** In very loud scenes the volume drops to zero or stutters.

**What was checked.** In every scene reached by script (canal, running, menus, map, fast travel, the crowded plaza),
the audio path never failed: 0 device underruns, 0 ms of output starvation, 0 clipped samples, peaks −8.7 to −10 dBFS,
about 48 000 frames/s delivered with 3–4 buffers queued. AudioFlinger counted 0 underruns on the track. The game mixes
in its own Wwise engine (recompiled) and hands finished s16 stereo to `audout`; the runtime only copies it. No scene
reached Wwise's limiter threshold, so the report is still open.

**Candidates, and how to tell them apart:**
1. Wwise's peak limiter / compressor misbehaving as recompiled (a NaN or reciprocal-estimate path gives silence or
   pumping): `[audstat]` shows silent buffers or a level collapse with **0 underruns and 0 starved**, and the raw
   capture shows the zeros in the game's own output.
2. The Wwise worker threads starved in heavy scenes (shader builds on `za-build` at the same nice level): `gap max`
   well over 40 ms, `starved` > 0, `underruns` > 0. Fix would be generic: raise the audio threads' priority or the
   release target.
3. The device itself (speaker protection, the smart-amp limiter at high volume, a Bluetooth route): every counter
   clean while the cut is heard.

**How to capture it.** Put `LARECOMP_AUDIO_STATS=1` in `env.txt` (one log line a second, negligible cost), and
ideally `LARECOMP_AUDOUT_WAV=<absolute path under files/>` (about 11 MB a minute). Play to the scene, note the time,
pull `lasrecomp.log` and the capture, then run `android-dev/analysis/audstat.py <log> -v` and
`android-dev/analysis/audraw.py <capture>`. Preset: `android-dev/presets/env-audio.txt`; scripted scenarios:
`android-dev/scenarios/audio.sh`, `audio-wz.sh`. Also note: speaker or headphones, and the system volume.

### 3. Hitches: world load and first-use pipelines

- **World load:** 400–600 ms once per area load. The game thread is blocked while its loader threads read the romfs
  (about 500 reads / 93 MB in that window). The big upload frame costs the render thread only about 18 ms of it.
  Faster uploads or hashes would save under 20 ms per load, inside a fade: low priority.
- **First-use pipeline builds: the one real gameplay hitch class.** About 4 hitches per 15 minutes of play, 96–256 ms
  each ("1 pipeline built (172 ms…)"). At boot the log says `prepared 0 + N seen before, not found 1585`: the
  game's shipped shader list never matches on Android, because the romfs index is empty there. Lead: find why they
  are "not found" (likely guest code not yet resident at that point) and prepare them lazily on a worker once seen.
- **Cold pipeline cache:** for about 10 s after spawning, 20–40 k draws per 2 s are skipped (objects or sky missing).
  A test that clears `files/cache` gives the player this pop-in again; playing once re-warms it.
- Menu-screen hitches of 60–120 ms are the game's own waits ("render cpu 0"), not ours.

### Smaller open items

- **60 fps:** not re-measured since the translator fixes. Test inis for 50 % and 75 % at 60 fps are a quick start;
  below 60 the game runs in slow motion, and the dynamic mode falls back to 30 by itself.
- **Render thread:** push descriptors / update templates (descriptor sets cost 3.3 ms of about 11.5 ms of draw
  time) and texture change checks (1.5 ms a frame: this 5.15 kernel has no `PAGEMAP_SCAN`, so every texture is hashed
  once a frame) are not done.
- **Window chain:** SGSR straight into the rotated swapchain image when no overlay is drawn would save about 0.5 ms
  of GPU [inference]. Not done.
- **PC upstream fixes not ported:** the eye fixes (they switch on only for NVIDIA/AMD), a texture-check budget, the
  Pokémon Center lightmap squares. The Android fork branched from the PC code of late October.
- **120 Hz:** only through the Thor's own display setting. That is the player's choice.
