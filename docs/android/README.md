# Android on the AYN Thor: knowledge base

This folder records what this fork learned while making Pokémon Legends: Z-A playable on the AYN Thor Max at a
locked 30 fps: what works, what was measured, what did not work, and how to change things. Start with the page that
matches your question. Each page can be read on its own.

The fork's layout is described in [FORK.md](../../FORK.md). The device and build scripts are in
[android-dev/](../../android-dev/). The upstream Android build itself (toolchain, `import_za.sh`, `build_za.sh`,
`za_apk.sh`) is documented in [tools/linux/android-port/README.md](../../tools/linux/android-port/README.md).

> **Personal builds only.** The APK carries `lasgame.so`, the game's code recompiled from your own dump. Never share
> an APK, game files, saves, texture dumps or screenshots of the game, and never commit them.

## Pages

| Page | Read it when you want to know |
|---|---|
| [STATUS.md](STATUS.md) | What works today, the measured frame rate on the Thor, the playable build's configuration, and the open issues with their best leads. |
| [FINDINGS.md](FINDINGS.md) | Why each change was made, with the numbers behind it and where it lives in the code. Also what was tried and did **not** work, so nobody retries it blindly. |
| [ENV.md](ENV.md) | Every `LARECOMP_*` switch this fork added or relies on, and the `larecomp.ini` keys that matter on the Thor: default, effect, diagnostic or feature. |
| [MODDING.md](MODDING.md) | What can be customised and how: settings, HD texture packs, romfs mods, code hooks, and what a gameplay mod would need. |
| [UPSTREAM.md](UPSTREAM.md) | The four pull requests sent to MrSw4gGuy/PokeZ-Android, what they contain, and what was left out of them on purpose. |

## The setup in one paragraph

The base is [PokeZ-Android](https://github.com/MrSw4gGuy/PokeZ-Android) (branch `android-port`), a static
recompilation of the game for Android. This fork's line is the branch `main`. The target is the AYN Thor Max:
Snapdragon 8 Gen 2 (Adreno 740), Android 13, a 1920×1080 top panel capped at 60 Hz by the system setting. The game
version is 2.0.2. The GPU driver is Mesa Turnip 27.0.0 R2, loaded through libadrenotools, because Qualcomm's stock
driver corrupts the 3D picture. Runtime switches are environment variables, set on the device through
`files/env.txt` (one `NAME=value` per line, see [ENV.md](ENV.md)). The playable build runs with no `env.txt` at all.

## Words used in these pages

- **Light profile**: the game's first-start video profile on Android: 75 % resolution, SGSR upscaler, 30 fps,
  shadows 0.5.
- **New-save street**: the first Lumiose street of a new game, a light scene (about 140–190 G-buffer draws a frame).
- **Late-game street**: a late-game save spawning in a Lumiose street, the heavy reference scene (about 2000–2200
  draws a frame). Most hard numbers on these pages come from it.
- **Render thread**: the thread that translates the game's NVN command buffers (`runtime/nvn/vulkan/exec.c`).
  **Recorder**: the thread that records the resulting Vulkan commands (`runtime/nvn/vulkan/record.c`).
- **Game thread**: the game's main thread (`za-game`); **guest threads**: its other threads (`za-guest`).
- **X3**: the Cortex-X3 core (cpu7, 3.19 GHz); **big cores**: the A715/A710 cores (cpu3–6, 2.80 GHz).
