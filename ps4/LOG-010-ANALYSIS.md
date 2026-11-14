# Liberty.Recompiled_010.log analysis

User-reported hardware: PS4 Pro, firmware 13.52. Analysis concerns the supplied
runtime log only; there is no matching executable, ELF, configuration, or klog.
The raw log is deliberately not committed.

- SHA-256: `8E469C9723CD9CF2E268781387E0CA82D8198D60D95CD177835A9A43E879C807`
- 5,751 lines, 850,506 bytes.
- Timestamps: 2026-11-11 18:27:27.341 to 18:55:28.359 (28m 1.018s).

## Findings

1. **Mixed hitch causes; guest synchronization dominates the reported set.**
   There are 501 hitch reports, averaging 134.2 ms, maximum 1,312 ms. These are
   thresholded reports, not all frames: their average is NOT average frame time
   or a basis for calculating average FPS. Of these, 449 spend at least half
   their frame duration waiting on other guest threads. Mean recorded guest
   wait is 89.7 ms; mean renderer wait 7.7 ms. This does not identify which
   guest thread caused each wait or prove the CPU/GPU split for the whole run.
2. **There are also substantial recorder stalls.** At 18:29:25.066, frame
   2372 took 1,312 ms, including 1,234 ms waiting for the renderer; the last
   recorder frame took 1,317.2 ms. Other examples include frame 20109
   (395 ms, 339 ms renderer wait, last recorder 425.5 ms). The recorder number
   refers to its last frame, not necessarily precisely the same guest frame.
   Pipeline compilation, resource upload, and driver work need separate timers
   before assigning these stalls to any one cause.
3. **Audio production does not consistently keep up.** The 157 gap reports
   total 256,070 ms of silence; the worst reported interval has 2,437 ms.
   Many mixing callback means exceed the 5.333 ms budget for 256 samples at
   48 kHz. Increasing the existing 24-frame/128 ms queue would add latency and
   would not fix sustained underproduction. The first frame-submission count
   appears cumulative; do not treat it as a normal ten-second interval.
4. **Only six process cores are available.** The affinity report is `0x3f`,
   with audio assigned core 5; one of the initial affinity moves failed. This
   is not evidence of seven-core mode, nor proof that enabling it alone fixes
   the hitches. The existing handoff already reports limited benefit on a
   different console. Package metadata and actual runtime masks need checking.
5. **No demonstrated allocation exhaustion.** Memory snapshots have zero
   allocation failures/retries. The final snapshot has 170 MB accounted
   textures against a 384 MB limit, 168 MB flexible free, and a 582 MB largest
   free direct-memory extent (not total free direct memory). This does not
   justify raising texture limits or the GPU arena.
6. **No radar-specific diagnostic records.** There are zero `ps5 hud:`,
   `radar-draw`, or `radar-clear` lines. Surface-address alias reports exist,
   but address reuse alone is not proof of the missing-radar cause. Obtain a
   short HUD trace with GPS active before modifying depth/stencil or resolves.
7. **The tested binary cannot be assumed to match current source.** There
   are 3,429 `shadow-trace: heartbeat` lines, including with trace value zero.
   Current source excludes that heartbeat on PS4. The tested build may predate
   that change or differ in its platform configuration; establish provenance
   before attributing current-source behavior to this executable.

## Other warnings

- Missing multiplayer backend and unavailable DLC concern optional services or
  content; they do not establish a single-player rendering failure.
- Missing cache devices match the repository's explicit disabled-cache setting.
  Do not enable them merely to silence these messages: the handoff documents
  why this behavior was retained.
- Font replacement atlases were unavailable and stock textures were retained.
  Current code first tries the resources directory, then a build-host fallback;
  the host path in the warning is not by itself proof of a hardcoded-only lookup.
- The temporal shader archive is unavailable. Inspect the installed settings
  before concluding a temporal feature is requested or making packaging changes.
- Sampled CPU-detile work is often very small (including 0.0-0.1 ms near the
  end), though individual slow captures exist. Re-enabling GPU detile would be
  unsafe given the documented PS4 hangs and is not supported by this log.

## Next evidence

Obtain the installed `native.toml`, tested release/commit or EBOOT SHA-256, and
matching ELF if available. Repeat a short timed route with frame timeline and
capture-split diagnostics enabled, separately from a short HUD trace. Preserve
timestamps and klog. Compare identical save/weather/camera conditions with the
normal shadow interval and the opt-in shadow-off setting; do not change audio
priorities, queue depth, memory limits, and shadows simultaneously.

No new renderer or scheduler policy is justified solely by this capture. The
existing branch fixes shadow-off capture overhead and PS4 HUD failure logging;
it does not yet fix the demonstrated guest synchronization or audio production
bottlenecks. Full C++ build and hardware validation remain outstanding.
