# android-dev

Tools to build, run and measure the Android port on the **AYN Thor** (Snapdragon 8 Gen 2, Adreno 740, Android 13).
They wrap the upstream build scripts in `tools/linux/android-port/` and add what device work needs:
- a lock;
- save backups;
- scripted runs;
- in-app screenshots;
- analysis of pacing, audio and GPU time.

The knowledge behind them (findings, `LARECOMP_*` switches, status) is in [docs/android/](../docs/android/README.md).

> [!IMPORTANT]
> The APK, the game files, texture dumps and in-app screenshots all come from your own copy of the game. Keep them in
> `PZ_WORK`, outside the repo. Never commit or share them.

## Layout

| Path | What |
| --- | --- |
| `config.example.sh` | Every path and setting. Copy it to `config.sh` (gitignored). |
| `build.sh` | Builds an APK from the current checkout. |
| `setup/` | `pz.sh` (the build-root wrapper), `adrenotools.sh`, `push-game-files.sh`. |
| `device/` | `save.sh`, `install.sh`, `restore.sh`, `run.sh`, `pull-frames.sh`, `pull-texdump.sh`, `texmod-ab.sh`. |
| `scenarios/` | On-device scripts that drive the game through the built-in pad, plus their `lib.sh`. |
| `presets/` | `ini-thor.ini` (the default playable settings) and `env-*.txt` (sets of `LARECOMP_*` switches). |
| `analysis/` | Python scripts that read a run's output directory. |
| `lib/` | `common.sh`, which every script sources, and `build-inner.sh`. |

Every script prints its usage with `--help`.

## 1. Setup (once)

Host: Fedora (any x86-64 Linux works) with `bwrap`, `rsync`, `cmake`, `ninja`, `adb`, Python 3 with Pillow and numpy
(OpenCV for `analysis/texmod/match.py`).

1. Copy the config and edit it:
   ```sh
   cp android-dev/config.example.sh android-dev/config.sh
   ```
   Set `PZ_WORK`, `ADB_SERIAL`, and the toolchain paths.
2. Toolchain: the Android SDK with `sdkmanager "ndk;29.0.14206865" "platforms;android-35"`, a JDK 21 at `PZ_JAVA_HOME`,
   and the .NET SDK for `tools/zaimport`.
3. The build root. The upstream scripts hard-code `/root/lasrecomp`, `/opt/android-ndk` and `/opt/android-sdk`.
   `setup/pz.sh <command>` runs a command in a user namespace where those paths are `PZ_BUILD_ROOT`, `PZ_NDK` and
   `PZ_SDK`. No root and no script edits are needed. Inside it, `SRC` is this checkout. Run the upstream steps in order
   (details in [tools/linux/android-port/README.md](../tools/linux/android-port/README.md)):
   ```sh
   S=android-dev/setup/pz.sh
   $S bash tools/linux/android-port/deps_android.sh all   # SDL3, shaderc, FFmpeg, headers (~2 min)
   $S bash tools/linux/android-port/import_za.sh <dir with the .nsp files + prod.keys>   # -> za-extracted/
   $S bash tools/linux/android-port/build_za.sh           # game code: lasgame.so (~12 min at -j16)
   ```
   `deps_android.sh` needs the pinned source tarballs from `tools/linux/build_deps.sh` first (see the upstream README).
4. libadrenotools and the Turnip driver that the APK ships as its default:
   ```sh
   android-dev/setup/adrenotools.sh <libvulkan_freedreno.so from a Turnip release>
   ```
   The current builds ship Turnip 27.0.0 R2 (StevenMXZ's builds). `LARECOMP_DRIVER=<path>` in env.txt overrides it.

## 2. Build

```sh
android-dev/build.sh pokez-mytest
```

- Builds HEAD plus your uncommitted `runtime/` changes. Other uncommitted files stay out.
- Holds `PZ_BUILD_LOCK` (`/tmp/pokez-build.lock`).
- Writes `PZ_APK_DIR/pokez-mytest.apk` and `.patch` (the runtime diff it was built with).
- Only libmain.so is recompiled (about a minute after a small change). lasgame.so comes from `build_za.sh`.
- The make log is in `PZ_TMP/build-<name>.log`.

## 3. Device rules

The device is the player's console. Every script that touches it follows these rules:

- **One user at a time.** Device scripts hold `PZ_DEVICE_LOCK` (`/tmp/thor.lock`). Other agents use it too. For manual
  adb work, take it yourself: `flock /tmp/thor.lock -c '...'`.
- **Never reboot the device or uninstall the app.** Uninstalling deletes the save. Leave other apps alone.
- **Back up the save before anything else:** `device/save.sh backup`. It checks the md5 of the copy against the device.
  `device/save.sh verify <backup>` and `device/save.sh restore <backup>` take the backup's name or path.
- **Put everything back after a run.** `run.sh` restores the save and the ini, and removes env.txt, even on failure.
- **End every session with the playable build:** `device/restore.sh`. It installs `PZ_PLAYABLE_APK`, pushes the
  player's ini, removes env.txt, stops the app, and prints the save md5 to compare with your backup.

```sh
android-dev/device/save.sh backup              # -> PZ_SAVE_BACKUPS/save-<date>-<time>/
android-dev/device/install.sh pokez-mytest
# ... play / test ...
android-dev/device/restore.sh
android-dev/device/save.sh verify save-<date>-<time>
```

First install on a new device:
1. `device/install.sh <apk>`.
2. Start the app once so it creates its folders.
3. `setup/push-game-files.sh` (6 GB, about 3 min).

## 4. Run a scenario

```sh
android-dev/device/run.sh pokez-mytest pace walk-1 env-pace.txt ini-thor.ini
```

Arguments:
- `<apk-name>`: always explicit. The APK is installed first.
- `<scenario>`: a file in `scenarios/` (`.sh` optional) or a path.
- `<outdir>`: under `PZ_RUNS`.
- `<env>` and `<ini>`: a path or a file in `presets/`, or `-` for none / the device's own.
- Optional 6th argument, `keepcache`: keep the pipeline cache.

Variables:
- `SAVE_IN=<tar>|new`: the save the run starts from (default `PZ_RUN_SAVE`).
- `SCEN_ENV="PLAY=60 PAD=xbox"`: variables for the scenario.
- `PULL_FILES="audout.raw"`: extra files under `files/` to pull.
- `SAVE_OUT=1`: also keep the save as it was at the end of the run.

Output in `<outdir>/`:
- `events.txt`: seconds since start, then the event;
- `ts.txt`: CPU, thermal, GPU and battery samples;
- `lasrecomp.log`;
- `frames/`;
- `sf_*.txt`;
- `save-before.tar`, `ini-before.ini`, file listings.

| Scenario | What it does (all start from the installed save: title, continue, spawn) |
| --- | --- |
| `idle` | Standing with camera turns for `IDLE` s, then walking for `PLAY` s. |
| `recon` | Walk, run, camera, menu, map. |
| `act` | Real actions, each logged as `ACT <name>`: lock-on, send-out, menus, battle attempt. |
| `look` | Standing, slow camera pans, walking. For image comparisons. |
| `pace` | Speed test, standing, walking. Compositor present times every 4 s (`analysis/pace.py`). |
| `speed` | Camera turn at a fixed stick input. The angle reached measures game time (`analysis/montage.py`). |
| `bg` | HOME for `BG` s, then back. The app's CPU time in the background. |
| `audio`, `audio-wz` | Loud scenes: menus and moves, or a fast travel to a wild zone and a battle. Per-thread schedstat. |
| `texmod` | Four still-camera holds, for texture A/B. |
| `perf` | 10 s `simpleperf` of the whole app, reported on the device. |
| `intro` | New save (`SAVE_IN=new`): language, appearance, name, intro cutscenes. |

Buttons: `presets/ini-thor.ini` keeps the Nintendo layout (A east). With an ini that binds A to south (`bind_pad_a = south`),
pass `SCEN_ENV="PAD=xbox"`. Scenarios wait for the board to cool down first (`COOLXO`, `COOLMAX`), so runs start alike.

## 5. Screenshots

`adb screencap` and `screenrecord` return **black frames on the Thor**. They have done so since a GPU timeout, and a
reboot is not an option. Use the app's own capture instead. In env.txt:
- `LARECOMP_SHOT_AT=<frame,frame,...>`; or
- `LARECOMP_SCREENSHOT_EVERY=<n>` (with `LARECOMP_SCREENSHOT_FROM=<frame>`).

The app writes `files/build/frames/frame_<n>.png`. `run.sh` pulls them into `<outdir>/frames/`. After manual play:

```sh
android-dev/device/pull-frames.sh my-session --clear
python3 android-dev/analysis/sheet.py "$PZ_RUNS/my-session/frames" 4   # contact sheet
```

## 6. Analysis

| Script | Input | Output |
| --- | --- | --- |
| `pace.py <run>...` | `pace` run: `sf_*.txt` + `[pace]` lines (`env-pace*.txt`) | Display-side and game-side frame intervals, judder. |
| `pace-log.py <log> [s]` | `[pace]` lines | fps, period p50/p95/max, CPU/GPU ms per time bucket. |
| `sweep.py <run>...` | Runs with a `walk` event | One line per run: fps, periods, GPU, temperatures, off-cadence. |
| `sf.py <sf_*.txt>` | SurfaceFlinger latency dumps | Present intervals in vsyncs. |
| `samples.py <run> [a-b]` | `ts.txt` | CPU per thread, temperatures, GPU busy and clock, battery per window. |
| `hitch.py <run>...` | `[slow]` lines (`env-hitch.txt`) | Frames over 50 ms, each with its main cause. |
| `gpuprof.py <run>` | `[gpuprof]` lines (`env-gpuprof.txt`) | GPU ms per frame for each kind of pass. |
| `utrace.py <utrace.txt>` | Turnip u_trace (`env-utrace.txt`, `PULL_FILES=utrace.txt`) | Render passes: GMEM or sysmem, cost. |
| `audstat.py <log> [-v]` | `[audstat]` lines (`env-audio.txt`) | Underruns, starvation, clipping, peak level. |
| `audraw.py <audout.raw>` | The game's raw audio output (`PULL_FILES=audout.raw`) | Peak and RMS per second, silence runs. |
| `schedpar.py <sched.txt> [tid]` | `audio*` scenarios | Run and runqueue-wait time per thread. |
| `montage.py <out.jpg> <png>...` | Frames from several runs | Side-by-side sheet. |

## 7. Texture packs: dump and A/B

1. Dump. Run any scenario (or play) with `presets/env-texdump.txt`, then pull the dump:
   ```sh
   android-dev/device/pull-texdump.sh "$PZ_WORK/texdump"
   ```
   Make contact sheets with `analysis/texmod/texsheet.py <dump> <out prefix>`. To find which texture is on screen:
   `analysis/texmod/match.py <frame.png> x0 y0 x1 y1 <dump>`.
2. Make the pack. Upscale the dumped textures and keep their names (`tools/za/upscale_textures.py`; PNG to BC7 DDS
   with `tools/za/png_to_dds.py`).
3. A/B. Push the pack, run `texmod` with HD switched on and off every 120 frames, then clean up and restore:
   ```sh
   android-dev/device/texmod-ab.sh pokez-mytest <pack dir> texab-1
   python3 android-dev/analysis/texmod/ab.py "$PZ_RUNS/texab-1" "$PZ_RUNS/texab-1/ab"   # side-by-side crops
   ```
