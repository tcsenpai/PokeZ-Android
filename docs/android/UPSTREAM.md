# Upstream pull requests

Four pull requests were opened on [MrSw4gGuy/PokeZ-Android](https://github.com/MrSw4gGuy/PokeZ-Android) against
`android-port` on 2026-11-08. As of 2026-11-14 all four are **open, with no comments**. Their branches are pushed to
our GitHub fork (git remote `fork`) and also exist locally under the same names; nothing else of ours is pushed.

Each PR is one commit on `android-port`, rebuilt from `main` without the diagnostics. A local branch `pr-clean`
(`main` minus diagnostics, not for pushing) was the reference they were cut from. While the PRs are open, keep their
branches as they are; answer review on those branches, not on `main`.

## The four PRs

| # | Branch | Title | Contents |
|---|---|---|---|
| [#1](https://github.com/MrSw4gGuy/PokeZ-Android/pull/1) | `android-adreno-drivers` | Android: custom Vulkan driver (Turnip via volk + libadrenotools), 32-bit integer math without shaderInt64 | `VOLK=1` / `ADRENOTOOLS=` build; `LARECOMP_DRIVER` (packaged, file name, absolute path, `system`); 32-bit integer pairs when the device has no `shaderInt64` (`LARECOMP_VK_INT64` forces); `GENARCH` on Android; README notes on staging volk and libadrenotools. |
| [#2](https://github.com/MrSw4gGuy/PokeZ-Android/pull/2) | `vulkan-translator-fixes` | Vulkan: translator loops and bounded jump tables, NMin / NMax, ClearBuffer / ReportValue, indirect-args clamp | `LARECOMP_SHADER_FLAT=2` (structured loops, bounded BRX tables, one SSY / PBK stack), default on Android; NMin / NMax / NClamp (`LARECOMP_SHADER_NMIN`, now in the translation cache key); ClearBuffer and ReportValue in GPU order; the GPU-side indirect-args clamp (`LARECOMP_ARGS_CLAMP`); RGB565 as B5G6R5; a profiler mark at each submission's end; absolute path in `vk_save_png`. |
| [#3](https://github.com/MrSw4gGuy/PokeZ-Android/pull/3) | `android-threads-power` | Android: thread placement, recorder wake threshold, render fast paths, UI-thread poll, sysfs thermal fallback | `LARECOMP_UI_POLL`; `LARECOMP_AFFINITY` (+ `_RECORDER`, `_WORKERS`) and thread names; `LARECOMP_VK_RECORDER_WAKE_KB`; `LARECOMP_RENDER_OPT`; sysfs thermal fallback (`LARECOMP_THERMAL*`); pipeline cache saved every 30 s; fullscreen by default on Android; `ZALaunchActivity` (main-display launch). |
| [#4](https://github.com/MrSw4gGuy/PokeZ-Android/pull/4) | `android-launcher-icon` | Android: launcher icon (optional) | A "ZA" monogram drawn by `tools/linux/android-port/icon/make_icon.py` (Pillow, a SIL OFL font), its committed output, and `make_project.sh` copying it over SDL's icon. No game assets. Marked optional. |

The numbers in the PR descriptions are the ones in [FINDINGS.md](FINDINGS.md). The shared test note: all four were
run on the device together as one build, plus off-by-default diagnostics; each branch alone and the four merged in
order build `libmain.so` with NDK r29 against `android-port` with upstream's warnings; no device run of the exact
branches, no Windows or macOS build.

## Left out on purpose

**Diagnostics and tests** (listed in each PR as "available on request"):

- #1: `LARECOMP_GPU_TURBO`, `LARECOMP_LOG_BUILDS`, `LARECOMP_VK_EXEC_STATS`, the SPIR-V dumps beside
  `LARECOMP_SAVE_MSL`, `LARECOMP_VK_GMEM_BDA` (hangs the stock driver).
- #2: `LARECOMP_SHADER_FLAT_LOG`, `LARECOMP_SAVE_BLOB`, `LARECOMP_VK_LAZY_PASS` (no gain),
  `LARECOMP_GPUPROF_EVERY`, `LARECOMP_GPUPROF_DRAWS` and the longer pass labels, `LARECOMP_VK_PASS_BARRIER`,
  `LARECOMP_UBO_GPU_ORDER` and `LARECOMP_LOG_UBO` (never fired on Z-A), `LARECOMP_TEST_FENCE_EARLY` and the
  device-lost report's "last seen" submission, `LARECOMP_VK_SEQ_POLL` (no difference).
- #3: `LARECOMP_DISPLAY_RATE` (no difference), `LARECOMP_VSYNC_LOCK` (inert on Z-A), `LARECOMP_FPS45_HZ` (since
  removed), the measured panel period, the AYN `Settings.System` logging, `LARECOMP_SLOW_LINES` and wall-clock times in
  `[slow]` lines.

**Work that landed on `main` after the PRs were cut** (candidates for later PRs, not yet offered):

- Pacing to the display's measured refresh (`objects.c` `present_pace`, `LARECOMP_PRESENT_LOCK`): fixes the 2.9 s
  judder on a 59.65 Hz panel.
- Background pause, surface rebuild and the game-clock freeze: without them the game never shows a picture again after
  a trip to the home screen.
- Removal of the 45 fps mode: it is upstream's own feature, so this one needs a conversation first (it is identical to
  30 on Z-A and would run at 1.5× if it worked).
- HD texture packs and dumps on Android (paths, the undrained dump queue), `tools/za/png_to_dds.py`, and the DDS
  colour-space fix.
- `LARECOMP_AUDIO_STATS` (audio diagnostics).

Check FINDINGS for the evidence before offering any of these.

## The PC upstream (Hexveil404/PokeZ-ARecomp)

The Maxwell → GLSL translator is shared with the PC version, [Hexveil404/PokeZ-ARecomp](https://github.com/Hexveil404/PokeZ-ARecomp),
which the Android fork branched from in late October. **PR #2 is relevant there:** the jump-table bound and the loop
form probably help on PC too [inference]; `LARECOMP_SHADER_FLAT=2` and `LARECOMP_SHADER_NMIN=1` would be the switches
to try. No PR has been opened there.

The other way round, PC fixes this fork lacks: the eye fixes (they switch on only for NVIDIA / AMD), a texture-check
budget, and the Pokémon Center lightmap squares (see [STATUS.md](STATUS.md#smaller-open-items)).

## Keeping up with upstream

- `android-port` is a plain mirror of upstream: never commit on it. Fetch it, then merge it into `main`.
- If a PR is merged, the same change already exists on `main` in a different shape: expect conflicts in the touched
  files (`exec.c`, `translate.c`, `android.c`, `device.c`), and keep upstream's form where they are equivalent.
- If review asks for changes, make them on the PR branch, then bring the result back to `main`.
