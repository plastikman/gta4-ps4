# Renderer rework: scope (2026-11-04)

Goal: more frame rate by cutting the per-draw CPU work of the two threads that
set the pace. Everything below comes from the sampling profiles of log 497
(build CIX, renderer with line tables, settings 0.5/0.5, ~900-1500 draws) and
the capture-split timers of the earlier runs. Nothing here is measured on a
prototype yet: the savings are estimates from profile shares.

## Where a frame goes now

| Thread | Per frame (idle, ~1000 draws) | Notes |
|---|---|---|
| Game render thread | ~18 ms game code + ~11 ms our hand-off + ~10 ms waiting on the game's main thread | hand-off ~11 us per draw |
| Our recorder thread | ~40-50 ms, nearly a full core | Vulkan recording + RADV + submit |
| Our worker thread | ~30% busy | spare capacity |
| Game main thread | ~37% busy, ~48% waiting | not the limit on its own |

A frame takes as long as the slower of the render thread and the recorder, so
both have to come down together; either alone moves little.

## What the time is spent on

**Recorder (share of its samples in game-side code):**
`RecordNativeFrame` 21% (much of it inlined per-draw work), `BindCommonDrawState`
6%, `PrepareFrameTextures` 4.6%, RADV state emission and draw calls ~6%,
**string-to-number parsing (`strtoull`/`strtox`/`wcstoul`/`wcstox`) ~8.6%**,
a per-frame log call 0.7%, the frame-constant arena (`Rehash`, capacity) ~2%,
anisotropic-filtering policy per draw ~0.9%.

**Hand-off on the game's render thread:** `ValidateAndCopyCommand` 9%,
`SubmitTitleCommand` 8.8% (lock, staging, publish), command-slot alloc/free ~2.4%,
dirty-state delta and constant delta ~4%, render-state decode 1.8%,
**diagnostic hooks still on the hot path** (bulb source capture 1.7%, TV/fire/phone
trace checks ~2%), surface/virtual-resource lookups ~2%. About 4,450 of the ~5,600
commands per frame are small state changes folded into the next draw; each one
still crosses from game code into ours and takes the capture lock.

## Work packages

### 0. The "string parsing" (done 2026-11-04, build CXI)
It was not string parsing. Log 497 came from build CVIII's eboot, and was
symbolized against CIX's ELF: the anchor matched but libc had moved by 0x440
bytes. Against the right ELF, those samples are `memcmp`, `memcpy` and `memset`.
The SDK's musl `memcmp` compares one byte per loop iteration and was ~21% of
the worker and ~6% of the recorder, mostly from the constant-bank updates
(`stateful_constant_state.h`, compare-before-copy per range). CXI links an SSE2
`memcmp`/`bcmp` into the eboot (`src/core/orbis_link_compat.cpp`).
Lesson: symbolize with the ELF of the eboot that actually ran.

### 1. Clear the remaining diagnostics off the hot paths
Bulb source capture per indexed draw, the TV/fire/phone trace envelope checks
per command, per-draw anisotropy policy lookups, the per-frame log call on the
recorder. Gate each behind a flag read once per frame.
*Expected:* ~1-1.5 ms a frame on the render thread, ~1 ms on the recorder.
*Effort:* 1-2 days. *Risk:* low (diagnostics only).

### 2. Move recording preparation from the recorder to the worker
The worker is ~30% busy and already walks every command before the recorder.
Move per-draw work that does not need the command buffer to it: pipeline lookup
(`GetOrCreateDrawPipeline`), descriptor and texture preparation
(`PrepareFrameTextures`, prepared bindings), constant-bank placement. The
recorder then only emits Vulkan commands from precomputed data.
*Expected:* 20-30% of the recorder. *Effort:* 1-2 weeks. *Risk:* medium (ordering
and lifetime of the precomputed data across the two threads; the frame pipeline
already hands frames between them).

### 3. Stop sending state changes as commands
The draw capture already reads the game's device structure (constants, fixed
state, texture and stream handles). Read the rest of what the state commands
carry (shaders, vertex declaration, render targets) there too, and drop the
~4,450 state commands a frame at the hook layer: no game-to-host crossing, no
lock, no coalescing for them.
*Expected:* a large share of the ~11 ms hand-off, plus the hook glue counted in
"game code" (estimate 3-5 ms a frame on the render thread). *Effort:* 1-2 weeks.
*Risk:* medium-high (every state the worker applies from `pending_state` must
come from the device structure instead; mistakes show as wrong rendering).

### 4. Cheaper per-draw recording (later)
Skip redundant Vulkan state between consecutive draws (pipeline, descriptor
sets, vertex buffers), batch constant uploads per frame instead of per draw.
*Expected:* 10-15% of the recorder. *Effort:* 1-2 weeks. *Risk:* medium.

## Expected result

0 + 1 together: a few ms on each thread, about +1-3 FPS. With 2 and 3, the
recorder could drop to ~30 ms and the render thread to ~30-33 ms at ~1000 draws:
roughly 27-30 FPS in normal driving and the low-to-mid 20s in the busiest
streets. That assumes the render thread's ~10 ms wait on the main thread shrinks
as the render thread gets faster; if the main thread then becomes the limit, the
ceiling is lower.

## How to proceed

In order 0, 1, 2, 3, 4, each with one console run and the same measurements
(`ps4_capture_split = true` for the hand-off, `ps4_cpu_sampler = true` for the
profiles, the frame timeline for the recorder), and a stop/go decision after
each from the numbers. 0 and 1 are cheap and safe; 2 and 3 are where the real
gain is and where the risk is.
