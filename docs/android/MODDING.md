# Modding Z-A on Android

What can be changed, from the cheapest to the hardest: settings, texture packs, romfs file mods, and code hooks. None
of these touches the game files you imported: packs and mods are loose files beside them, and code hooks are compiled
into your own build.

> Everything made from the game (texture dumps, upscaled textures, extracted or modified game files) is yours to use,
> not to share or commit. Only scripts and source belong in git.

On the Thor the app's data folder is `/storage/emulated/0/Android/data/org.recomp.za/files/` ("`files/`" below). It is
external storage: a file manager, the USB cable, or `adb push` reach it. The game runs with `files/` as its working
folder, so the relative paths below (`textures/hd/`, `mods/`) are inside it.

## What can be changed

| You want | Use | Rebuild? |
|---|---|---|
| Resolution, frame rate, shadows, LOD, depth of field, FXAA, bloom | [Settings](#settings) (F1 menu or `larecomp.ini`) | no |
| A texture to look different (sharper, recoloured, a custom picture) | [Texture packs](#texture-packs) | no |
| A game file replaced or added (models, layouts, data tables) | [romfs mods](#romfs-mods) | no |
| A value the game computes in code (a multiplier, a branch) | [Code hooks](#code-hooks) | yes (`build_za.sh`) |
| New gameplay mechanics | [Mechanics mods](#mechanics-mods): reverse engineering first | yes |

## Settings

The in-game menu (F1, the Back gesture, or Minus + Plus on a pad) writes `files/larecomp.ini`. The keys that matter on
the Thor, with the playable values, are in [ENV.md](ENV.md#larecompini-keys-that-matter-on-the-thor). The graphics
tweaks among them apply at once, mid-game:

- `za_lod`: 0 the game's level of detail; 1 "improved" (a LOD bias of −1: fewer models swapped for simpler ones at a
  distance, less pop-in, more GPU time); 2 performance (bias +1).
- `za_dof_off`, `za_fxaa_off`, `za_bloom_off`: turn depth of field, FXAA, bloom off.

They are code hooks (see [Code hooks](#code-hooks)) on sites taken from Fl4sh9174's exefs mods for 2.0.2, made
switchable.

## Texture packs

The runtime replaces a game texture with your file **when the file is named after the texture's hash**. The hash is
computed over the texture's bytes as the game wrote them, so it is the same in every run. Nothing in the game's files
changes.

### Quick version

1. Find the **hash** of the texture you want to change: 16 hex digits, e.g. `71edbc176ee795f9`.
2. Make an image with the same name: `71edbc176ee795f9.png` (RGBA) or `71edbc176ee795f9.dds` (BC7 / BC1 / BC3).
3. Copy it into `files/textures/hd/` on the device.
4. In the game: **F1 → Textures → HD** (setting `tex_hd`, on by default). The folder is scanned when the game starts.

### Formats

| File | Content | Size | Colour space | GPU memory (measured) |
|---|---|---|---|---|
| `<hash>.png` | RGBA8. The runtime makes the mips (linear blits). | any; usually 2× or 4× the original | the game texture's own (sRGB if the game's is) | 1024² = 5.35 MB, 16–60× the game's compressed original |
| `<hash>.dds` | BC7 (DXGI 98 / 99), BC1 (71 / 72) or BC3 (77 / 78), DX10 header or legacy DXT1 / DXT5. Mips are read from the file (any count; the full chain is best). | any; multiples of 4 are best | the game texture's own, **whatever the DXGI sRGB flag says** | 1024² BC7 = 1.33 MB, 4–8× the original |

When both exist, the `.dds` is used. A few dozen PNGs are fine for one-off edits. For packs of hundreds of textures
use BC7 DDS. Measured on the Thor with the same 29 textures:

| Pack | GPU memory | Load time on the render thread (mean / max / total) |
|---|---|---|
| PNG RGBA8 | 136.5 MB | 21.0 / 51.9 / 610 ms |
| BC7 DDS | 35.6 MB | 0.4 / 3.0 / 12–13 ms |

The game's own data for those 29 textures was 3.7 MB. On screen, the PNG and the DDS pack differ by a mean of
0.21/255 on a wall patch, with identical mean brightness.

### Step 1: find the hash (dump)

1. Set `tex_dump = 1` in `larecomp.ini` (or `LARECOMP_TEX_DUMP=1` in `env.txt`).
2. Play through the places you want. While an area loads the game drops to 1–3 fps: it waits for the GPU on every
   texture. This is a one-off cost of making a pack, not of playing with one.
3. Set `tex_dump = 0` again.
4. Pull the dump (`files/textures/dump/`, one `<hash>.png` per texture; one session gave about 3,000 files, 200 MB):

   ```sh
   adb exec-out "tar -cf - -C /storage/emulated/0/Android/data/org.recomp.za/files/textures dump" | tar -xf -
   ```

   or `android-dev/device/pull-texdump.sh <dest>`.
5. Look through it. The file name is the hash. Blue / purple images are normal maps and greyscale ones are masks:
   leave those alone (an upscaler's "detail" there reads as lighting noise). Contact sheets help:
   `android-dev/analysis/texmod/texsheet.py`. To find which dumped texture is behind a patch of a screenshot:
   `android-dev/analysis/texmod/match.py`.

Known hashes (2.0.2), from a Lumiose dump: the canal walls at a canal spawn are `71edbc176ee795f9`. Window and
facade albedo textures:
- 256×256: `2cef7c85879af987`, `34ffbf94499e511d`, `36a8647f4e7a2c7c`, `40714ad8ceb8d000`, `acc03b6d9112e71a`,
  `b59f78f2be6006c8`, `c9f08a1a0822f6bc`, `d24764300a0fe907`, `d7ad27b9715f04ce`, `e22166bfba88347a`,
  `e5a3809a7c8c4545`, `e8c32466b5486fd7`, `ebb11d5de5b294ea`, `ee9e22daf643953f`, `f9d0c5ba7783c8db`,
  `fe1d06ef80c3ccd7`;
- 256×512: `105592b919985e7c`, `23b97106aec48ad2`, `37027604507331c2`, `4f487c466d7fb54b`, `900f28490dc47363`,
  `a304a5360cd97f8a`, `c0c072a304c2d05a`;
- 512×512: `094b3b272f59f7b1`, `bf46bb5024d3a1a6`.

### Step 2a: edit one texture by hand

Example: a custom picture on a window.

1. Open the original `<hash>.png` in GIMP or Krita. A texture often holds **several windows side by side** (an atlas).
2. Keep the original's **aspect ratio**. The size can grow: a 256×256 original can become 1024×1024, but not
   1024×512; a 256×512 stays twice as tall as it is wide.
3. Paint over **one window's area** and leave the rest of the atlas as it is. If you fill the whole image, the picture
   is smeared over every window that uses a piece of it.
4. **Transparency matters:** transparent parts of the original stay transparent in the game (glass, cut-outs). Make
   your area opaque if you want it solid.
5. Export as **RGBA PNG** with **the original's name**. No mipmaps needed: the runtime makes them.
6. Push it:

   ```sh
   adb shell mkdir -p /storage/emulated/0/Android/data/org.recomp.za/files/textures/hd
   adb push <hash>.png /storage/emulated/0/Android/data/org.recomp.za/files/textures/hd/
   ```

### Step 2b: upscale many textures

```sh
python tools/za/upscale_textures.py --src dump --dst hd --scale 2 [--esrgan <realesrgan-ncnn-vulkan>]
```

- It upscales colour textures only and skips normal maps and masks by their content.
- With Real-ESRGAN (realesrgan-ncnn-vulkan, a 47 MB download, any Vulkan GPU) it uses the model; without it, Lanczos
  plus an unsharp mask. Colour and alpha are resized separately (a combined RGBA resize blanks the colour under
  alpha 0).
- Pick the textures you want first: a whole dump at 2× RGBA8 would be several GB of GPU memory, far too much for the
  Thor's shared memory.
- ESRGAN gives clean edges and sharp mouldings but **erases fine ornament** (a balcony railing's scrollwork becomes
  a plain bar) and flattens stone grain a little. Lanczos keeps the ornament but is only softer. Review per texture,
  or blend the original's detail back (`tools/za/hd_pack.py`'s blend stage).

### Step 3: encode to BC7 DDS

```sh
pip install etcpak          # 2.6 MB, its BC7 encoder
python tools/za/png_to_dds.py hd --dst hd_dds
```

This writes BC7 with every mip level, about 0.5 s per 1024² texture, on any OS (no texconv). The encoded wall differs
from its PNG by a mean of 0.4–0.9 per channel: good enough here, though not the highest-quality BC7 (no RDO, no
exhaustive mode search).

### Step 4: install and check

Push the files into `files/textures/hd/` and restart the game. `files/lasrecomp.log` confirms:

- `[tex] N textures in textures/hd`: the files found;
- `[tex] HD <hash>: 1024x1024 (11 levels, 5.35 MB) for the game's 256x256 (…), 23.8 ms`: one line per texture
  really replaced, with running totals.

Turning HD on mid-game re-checks every live texture: about 2 s at 17–20 fps.

For an A/B of one still view, `LARECOMP_TEX_HD_FLIP=<frame>,<every>` flips packs on and off like F1 would. Combined
with in-app screenshots (`LARECOMP_SCREENSHOT_EVERY` / `_FROM`), choose a flip offset that is not a multiple of the
shot interval (e.g. `630,120` with shots every 60): a shot on the flip frame still shows the old state.
`android-dev/device/texmod-ab.sh` runs that A/B in one device-lock hold; `android-dev/analysis/texmod/ab.py` pairs
the shots.

### Offline pack from the game files

`tools/za/hd_pack.py` builds a whole BC7 pack straight from the romfs, without playing (stages: extract, upscale,
blend, encode). It needs Pillow, numpy, texture2ddecoder, ooz (see [romfs mods](#romfs-mods)), and Windows' texconv
for the encode stage. Running `png_to_dds.py` on the upscaled PNGs instead should replace that stage on Linux
[inference]. Not exercised on the Thor.

### Limits

- Only plain 2D colour textures the CPU fills qualify: target 2D, one layer, not a render target, not "dynamic".
  Arrays, cubemaps and textures the game rewrites while running keep the game's own image.
- One texture is often **reused on many buildings**: your change appears everywhere it is used.
- **A game update, or another mod changing the source file, changes the hashes:** the pack silently stops replacing
  (no error, no line in the log).
- PNG packs are decoded on the render thread at each texture's first upload (10–50 ms each): big PNG packs add load
  hitches. DDS avoids both the decode and the 4× memory.
- The fixes that make packs and dumps work on Android are on `main` (relative paths resolved by SDL to internal
  storage or APK assets; a dump queue nobody drained; DDS colour space). Upstream `android-port` does not have them.

Where: `runtime/nvn/vulkan/textures.c` (pack scan, `hd_load_dds`, the dump queue drained from `vk_hd_switch_frame`).

## romfs mods

The runtime layers folders over the game's files, inherited from upstream (`runtime/services/fs.c`, "layered
files"):

- each `mods/<name>/romfs/` folder (in `files/` on Android) mirrors the romfs. A file there replaces the game's file
  with the same path; a new path is added, with any folders it needs;
- order: `mods_order` in `larecomp.ini` (`|`-separated names), the rest by name; **the first layer that has a file
  wins**;
- off: a name in `mods_off`, or a folder name starting with `_` or `.`;
- up to 15 mods; the log shows `[fs] mod: <name>` and `[fs] N romfs files replaced (mods / randomizer)`.

**Z-A specifics.** Z-A keeps nearly all its files in packs (`arc/data.trpfs`, Oodle-compressed, listed by
`arc/data.trpfd`) and reads a loose romfs file only when the pack list doesn't have it. So the runtime serves the
game's `data.trpfd` without the entries your mod replaces ("files the mods have taken out of the packs' list" in the
log). A mod's own `data.trpfd` is never used. A loose file needs its **full path**: the game's list keeps only FNV-1a 64
hashes, so a mod tool has to reconstruct the path (for example from the pack's name and the file's own name) and check
it against the hash.

**Tools** (Python, run on the PC against your extracted game files):

| Tool | What it does |
|---|---|
| `tools/za/romfs_extract.py <romfs.bin> <out> [prefix]` | Copies files out of `romfs.bin`; by default `arc/` (the packs and their list). |
| `tools/za/trinity.py` | A library: reads the ONEPACK file system (`data.trpfs` / `data.trpfd`), flatbuffers, SARC archives (UI layouts) and BNTX texture containers. Oodle through ooz built as a library. |
| `tools/za/lod_mod.py --ooz <libooz> [--romfs <dir>] [--out mods/MaxLOD] [--levels N]` | **The worked example.** Builds `mods/MaxLOD`: every model (`.trmdl`) keeps its full-detail mesh at every distance (or N levels more detail). It rewrites only the mesh-slot offsets, finds each model's path from its pack name and checks it against the hash. Costs GPU time and memory in crowds. |
| `tools/za/ooz_posix/` | Headers to build ooz (`third_party/ooz`) as `libooz.so` on Linux; the command is in its README. |

To use a mod on the Thor, copy the folder to `files/mods/<name>/romfs/…` and restart the game. Toggle it with
`mods_off` or by renaming it to `_<name>`. romfs mods have not been exercised on the Thor yet; `lod_mod.py`'s
MaxLOD is the first candidate to try, against `za_lod = 1` (the code hook) for GPU cost.

## Code hooks

The game's code is recompiled to C by `tools/gen.py`. A hook replaces one instruction of the game with C that calls
into the runtime. For Z-A the hooks live in `tools/za_patches.py`:

- `ZA_BUILDS`: per exefs build ID, the address of the frame-mode read in the game's frame clock. It is hooked to
  `rt_za_frame_cfg` (`runtime/hle/framepace.c`), which is how the 60 fps option works. Known builds:
  1.0.0 `7222e13ecf6adb32`, 2.0.0 `8a1c86c437394b69`, 2.0.2 `b1f12fd919eae86a`.
- `ZA_TWEAKS`: per build ID, `{address: (expected instruction word, patch)}`. Each patch emitter returns a C snippet
  that loads the value the game would use, passes it through `rt_za_tweak(which, value)`, and puts the result back
  in the register. `_checked` asserts the instruction word at the address, so a wrong version fails the generator
  instead of corrupting the game. Only 2.0.2 has tweaks.
- `runtime/hle/za_tweaks.c` `rt_za_tweak`: `switch (which)` returns the game's value or the tweak's, from
  `g_settings`, read at every call (so F1 changes apply at once).

**Adding a tweak:**
1. Find the site: the instruction that loads or computes the value (from a known exefs mod, or by reverse
   engineering; `tools/za/symaddr.py` and `tools/za/plt.py` name nearby symbols and call targets).
2. Add it to `ZA_TWEAKS['b1f12fd919eae86a']` with its instruction word and an emitter (`_ld_s0` covers
   `ldr s0, [base, #off]`).
3. Add a `which` case in `rt_za_tweak`, a setting in `runtime/settings.h` / `settings.c` (load and save), and a menu
   entry in `runtime/ui/menu.mm`.
4. Regenerate and rebuild the game code (`build_za.sh`: about 1.5 min of generation and 12 min of compiling at -j16),
   then `za_apk.sh`. With `android-dev/`, `android-dev/build.sh` wraps these.

Addresses are written as the desktop's (main at `0x7000000000`). On Android the guest window is elsewhere, and
everything is moved by `GUEST_SHIFT` (`runtime/rt.h`), the generator's sites included.

`runtime/game_addresses/` (`versions.h`, `patches_<ver>.h`, `runtime_<ver>.h`) and `tools/game_addresses.py` are the
per-version address tables of **Link's Awakening** (versions 1.0.0 and 1.0.1 / 1.1.0, keyed by main build ID; an
unknown build is refused). Z-A has no table there: its sites are in `za_patches.py`, keyed by build ID. Supporting
another Z-A version means finding each site again in that build and adding its entries; the instruction check
catches a wrong address.

## Mechanics mods

Nothing in this fork changes gameplay yet. What a gameplay mod needs:

- **Data-driven changes** (stats, tables, spawns, text, layouts) are game files: unpack the right pack with
  `trinity.py`, change the flatbuffer, and ship it as a romfs mod at its full path. The work is mapping the file
  formats (flatbuffer schemas) and finding the file; `lod_mod.py` is the template for a tool that edits a file in
  place and checks the path against the list's hash.
- **Code-driven changes** (a formula, a timer, a branch) are code hooks: find the function and the instruction,
  then hook it as above. Expect real reverse-engineering work per change, and new sites for every game version.
- **Hard example, for scale:** a correct 40 or 45 fps mode needs fractional world steps. The game steps its world in
  whole 1/60 s units and every subsystem takes that step, so it means patching each subsystem's dt
  ([FINDINGS.md](FINDINGS.md#the-45-fps-mode-removed)).
- The randomizer infrastructure in this tree (`runtime/rando.c`, `tools/rando/`) is Link's Awakening's and does not
  apply to Z-A.
