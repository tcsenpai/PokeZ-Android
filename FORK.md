# This fork

PokeZ-Android with work for the **AYN Thor** (Snapdragon 8 Gen 2, Adreno 740). The goal is *Pokémon Legends: Z-A* playable
at a steady 30 fps with a real late-game save.

Upstream: [MrSw4gGuy/PokeZ-Android](https://github.com/MrSw4gGuy/PokeZ-Android), branch `android-port`. Everything in
the upstream [README](README.md) still applies. This file covers only what the fork adds.

## Branches

| Branch | What |
| --- | --- |
| `main` | Our line: upstream `android-port` plus the Thor work. Build from here. |
| `android-port` | A plain mirror of upstream. Never commit to it. |
| `android-adreno-drivers`, `vulkan-translator-fixes`, `android-threads-power`, `android-launcher-icon` | Upstream pull requests, cut from `main`'s work and rebased on `android-port`. Change them only to answer review. |
| `pr-clean` | Local reference: `main` without the diagnostics. Not for pushing. |

Sync with upstream:

```sh
git fetch origin
git branch -f android-port origin/android-port   # (or: git checkout android-port && git merge --ff-only origin/android-port)
git checkout main && git merge android-port
```

## Where things are

- [android-dev/](android-dev/README.md): build, device and analysis tooling. Covers setup, building an APK, the device
  rules (lock, save backups, restore), scripted runs, in-app screenshots, pacing/audio/GPU analysis and texture packs.
- [docs/android/](docs/android/README.md): the knowledge base. Current status, findings, the `LARECOMP_*` switches,
  modding, and what was sent upstream.
- `eval/`: local scratch space, never committed (listed in `.git/info/exclude`).

## Quickstart

After the one-time setup in [android-dev/README.md](android-dev/README.md#1-setup-once):

```sh
android-dev/build.sh pokez-mytest              # APK from this checkout
android-dev/device/save.sh backup              # back up the save first, always
android-dev/device/install.sh pokez-mytest     # end the session with android-dev/device/restore.sh
```

## Legal

The same rules as upstream apply:
- No game files, game code, assets or keys are in this repo, and none may be added.
- `generated/`, `extracted/` and the build trees are never committed.
- The APK contains code recompiled from your own dump: never share it.
- Texture dumps and screenshots are the game's art. They stay in your work directory and are not committed.
