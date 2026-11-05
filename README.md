# GTA IV on PS4

**Grand Theft Auto IV (Xbox 360) running natively on a jailbroken PS4**, through
[Liberty Recompiled](https://github.com/OZORDI/LibertyRecomp)'s static
recompilation, built with the OpenOrbis toolchain and drawn with Mesa's RADV
Vulkan driver on the PS4's GPU. This is a port of
[gta4-ps5](https://github.com/elripalda/gta4-ps5) to the PS4. Work in progress.

> No game files are included. You need your own copy of the game.

## Status (2026-11-04, build CX)

**Try it:** [v0.1.0-ps4](https://github.com/plastikman/gta4-ps4/releases/tag/v0.1.0-ps4)
(pre-release: the PKG, the tested `native.toml` and install steps).

Playable, not yet smooth. From the start of a new game to Roman's apartment it
runs at 20-25 FPS while driving and 17-20 in busy streets, with sound that is
clean in cutscenes and slightly slow in busy driving.

## What has been tested

On one PS4 Pro (firmware 9.60, GoldHEN, boost mode), USA disc with Title Update 8:

| Area | Result |
|---|---|
| Boot, menus, starting a new game | works |
| Opening cutscenes | ~30 FPS (the frame limit); audio clean |
| Taxi ride from the docks to Roman's apartment | ~20-25 FPS; traffic and pedestrians present |
| On-screen text and help messages | works; a lot of text on screen costs a few FPS (it adds draws) |
| Busy city streets | ~17-20 FPS |
| Idle outside Roman's apartment | ~23-26 FPS |
| Driving audio (music, voices, traffic) | clear, but about 90% of real time with lots going on: slightly slow |
| 720p instead of 1080p | same frame rate (the build is CPU-bound) |
| Stability over the test runs | no crashes in the recent runs |

Not tested yet: missions past Roman's apartment, The Lost and Damned and The
Ballad of Gay Tony, Xbox 360 save import, multiplayer, a base (non-Pro) PS4, other
firmware versions, long play sessions.

Known gaps:
- the radar map is missing;
- the crisp vector fonts are not loaded (the game looks for the atlases at a
  host path);
- textures are detiled on the CPU (`gta4_native_gpu_detile = false`; the GPU
  path hung the PS4 GPU early in bring-up).

## Performance

Where it came from: on 2026-11-03 the same route ran at 8-15 FPS with garbled
audio. Removing leftover bring-up instrumentation (a range check on every
memory access of the recompiled code), overlapping the CPU's command recording
with the GPU's drawing, redrawing the shadow map every second frame and a series
of audio fixes brought it here (each step and its measurement is in
`ps4/HANDOFF-2026-11-02.md`).

Where the time goes now (per frame, idle, ~1000 draws; Jaguar at 2.13 GHz):

| Thread | Work |
|---|---|
| Game render thread | ~25 ms game code + ~10.5 ms handing draws to our renderer; the critical path (the game's main thread waits ~48% of the time on it) |
| Our recorder thread (Vulkan command recording) | ~35-41 ms at ~1000-1350 draws, all CPU (GPU wait ~0): ~25-30 us per draw |
| Game audio mixer (own core) | ~5.8 ms per 5.3 ms sound frame in busy driving |

So two threads are each near the limit of a Jaguar core, and making either one
faster only moves the limit to the other (measured on 2026-11-05: the CXII lock
fixes cut the render thread's hand-off by 1.5-3 ms and the time went to waiting
on the recorder). The game's own
code is mostly real work: in the hottest functions about 36% is loads and
stores of game data, 23% arithmetic, and only ~10% recompiler call overhead.
Already tried without a gain: 720p, asynchronous XMA decoding, moving our
capture work to another thread (it would save ~4 ms of a ~55 ms frame), a
faster `memcmp`, keeping the render thread, worker and recorder on one CPU
module. The recorder's time is spread over a very large per-command loop and is
mostly waiting on memory; `ps4/RENDERER-REWORK-SCOPE.md` has the measurements
and what a rework would take (estimate: +2-3 FPS for weeks of work).
For comparison, the PS5 port of the same code holds ~60 FPS on a CPU core
roughly three times faster, and the original Xbox 360 version capped at 30 with
dips into the 20s.

## Help wanted

This is a small hobby port and help from the community is very welcome,
especially with:

- **Renderer CPU cost per draw.** Our hand-off (~10 us per draw on the game's
  thread) and Vulkan recording (~25-30 us per draw, `graphics_system.cpp`) are
  spread over many small costs; `ps4/RENDERER-REWORK-SCOPE.md` has the per-stage
  timings (`ps4_capture_split = true` logs them) and the profiling tooling.
- **Recompiled code speed.** Ideas for cheaper guest memory access or calls in the
  generated code (ReXGlue's code generator).
- **Audio.** The game's mixer needs a little more than one Jaguar core in busy
  scenes; FFmpeg's x86 assembly is not built yet (needs NASM in the toolchain).
- **GPU texture detiling** on RADV for the PS4, the **radar map**, and the
  **vector fonts**.
- **Testing:** base PS4s, other firmware versions, the episodes and later missions.
  Logs from `/data/Liberty Recompiled/logs/` help most.

Issues and pull requests are welcome.

## What you need

- A PS4 running [GoldHEN](https://github.com/GoldHEN/GoldHEN). A PS4 Pro in boost
  mode is what this was tested on.
- Your own copy of GTA IV for Xbox 360 (USA) with Title Update 8, installed as
  game data on the console at `/data/LibertyRecomp/game`.
- A Linux build host with the ps4-gladius Orbis SDK bundle (the OpenOrbis SDK,
  matching Mesa headers, the RADV archive and the Vulkan entry-point shim).

## Building

```sh
git submodule update --init --recursive
# then apply the submodule patches: see ps4/patches/README.md

cmake -S ps4 -B out/ps4 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$HOME/ps4-gladius/sdk/orbis-sdk-v1/toolchain/orbis-sdk.cmake"
cmake --build out/ps4 --target gta4 --parallel 1
```

The Orbis executable is `out/ps4/glue/gta4-recomp/eboot.bin`. On a host with
little memory (7 GB here), build with `--parallel 1` whenever the renderer
rebuilds; `--parallel 2` is fine for the recompiled game code alone (see
`AGENTS.md`). `out/ps4/glue/gta4-recomp/gta4` is the unstripped ELF: keep it for
symbolizing logs, and **never package it as `eboot.bin`**.

## Packaging

The package staging directory is not in this repository yet. It looks like the
tree below. `libc.prx` and `libSceFios2.prx` are OpenOrbis's empty homebrew stub
modules, which point the loader at the system's own libraries and hold no Sony
code. [orbis-win](https://github.com/plastikman/orbis-win) builds them from the
OpenOrbis sources (GPL-3.0) with `scripts/build-openorbis-module-stubs.sh`; the
packages tested so far used OpenOrbis's prebuilt 3.4 KB versions.

```
stage/
  pkg.gp4                 GP4 project, title ID LBTY00001
  eboot.bin               out/ps4/glue/gta4-recomp/eboot.bin
  sce_sys/param.sfo, icon0.png
  sce_module/libc.prx, libSceFios2.prx
  Resources/button_prompts/, font_atlases/, aes_key.bin
```

Build and validate it with the SDK's `PkgTool.Core`:

```sh
PkgTool.Core pkg_build pkg.gp4 out/
PkgTool.Core pkg_validate out/IV0000-LBTY00001_00-GTA4PS4000000000.pkg
```

Keep game files out of the package: the GP4 must have no `game/` entries.
Install the PKG with GoldHEN's package installer.

## Configuration

Settings are read at start-up from `/data/LibertyRecomp/native.toml`.
`ps4/configs/native.toml` is the config used with build CX. The settings that
matter most on the PS4:

| Setting | Value used | Effect |
|---|---|---|
| `gta4_draw_distance_scale` | 0.5 | fewer draws, the main cost per frame (0.6 looks fuller, 1-2 FPS less) |
| `gta4_*_density_scale` | 0.5 | fewer cars and pedestrians: fewer draws and fewer sounds to mix |
| `gta4_native_shadow_interval` | 2 | redraws the sun shadow map every second frame |
| `gta4_frame_limit` | 30 | frame limit, kept on the game's thread |
| `ps4_audio_core` | -1 (core 5) | the audio threads' own core; -2 turns the layout off |
| `ps4_cpu_sampler` | false | the 100 Hz CPU profiler; true to profile |
| `ps4_capture_split` | false | timing of the render thread's hand-off; true to measure |
| `resolution` | (display) | e.g. `"720p"`; no faster on this CPU-bound build |

## Logs

The runtime log is `/data/Liberty Recompiled/logs/Liberty Recompiled_NNN.log`
(over FTP). Useful lines, all at warning level:

- `PS5 audio: N gaps in 10 s (… ms of silence) … frames submitted, … mixer callbacks`:
  1875 frames in 10 s is real time;
- `ps4 waits: render|main|audio|audio-mix thread waited …`: where each thread
  blocks, by the game code that waits;
- `PS4 cores: process 0x3f, audio core 5: …`: the audio core layout;
- `ps4 capture split: …` (with `ps4_capture_split = true`).

## How the PS4 build differs

The build history, with the measurement behind each change, is in
`ps4/HANDOFF-2026-11-02.md`. In short:

- **Recompiled code:** bring-up memory traces and the executable-page store
  guard made opt-in, no read-back after guest stores, no save/restore of the
  volatile XER/CTR/CR0/CR6 registers around calls, multiply-add without FMA3.
- **Renderer:** the PS5 port's capture paths and decoupled presentation, sun
  shadow rest frames, native shader-constant setters.
- **Audio:** the queued `sceAudioOut` driver, event-driven multi-object waits
  (the PS4's 1 ms sleeps last 1-2 ms), XMA decoding on the requesting thread,
  FFmpeg decoders opened ahead of time, the audio threads on their own core.
- **Memory:** malloc-family wrapping with a flexible-memory rescue path.
- **Submodules:** their PS4 edits are patches in `ps4/patches`.

## Credits

- **[Liberty Recompiled](https://github.com/OZORDI/LibertyRecomp)** by OZORDI
  and contributors: the recompilation of GTA IV this runs.
- **[LibertyRecompAndroid](https://github.com/vaduur/LibertyRecompAndroid)** by
  vaduur, and **[gta4-ps5](https://github.com/elripalda/gta4-ps5)** by
  elripalda, which this port starts from (its README is
  `README-gta4-ps5.md`).
- **[ReXGlue](https://github.com/rexglue/rexglue-sdk)**, derived in part from
  **[Xenia](https://xenia.jp)**.
- **[Mesa](https://mesa3d.org)** (RADV), the
  **[OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain)**
  and **[GoldHEN](https://github.com/GoldHEN/GoldHEN)**.
- [PS4Liberty](https://github.com/iHaiDeeZ/PS4Liberty), an earlier PS4 port on
  the same stack, was a useful reference.

This is a fan project with no ties to Rockstar Games, Take-Two Interactive,
Microsoft or Sony Interactive Entertainment. Grand Theft Auto is a trademark of
Take-Two Interactive. Please only play games you own.
