# PS4 Pro gameplay investigation

## Scope and status

Reported hardware: PS4 Pro, firmware 13.52. Boot works; a roughly 30-minute
session exhibited video delay, stutters (especially in sunlit areas), and a
blank radar/navigation display. The session log has
now been supplied as `Liberty.Recompiled_010.log`; the tested release is upstream
`v0.1.0-ps4`, superseded by `v0.0.1`. The installed configuration and exact
binary hash remain unknown. See `LOG-010-ANALYSIS.md` for the release comparison.
These changes are source-level fixes, not a verified
performance or radar fix. The existing handoff measurements used another
console/firmware and must not be substituted for this run's measurements.

## Changes

- With `gta4_native_shadow_interval = 0`, the PS4 indexed-draw hook now skips
  sun-shadow casters before capture, including large casters. Previously only
  small casters were skipped at the hook; the recorder discarded the others
  after the CPU had captured them. Clears and resolves remain enabled to
  produce the cleared shadow map. The existing depth-only, square-surface,
  minimum-size, and active-capture guards remain in effect.
- `ps5_hud_trace = true` now also enables sampled draw-failure diagnostics on
  PS4, matching PS5. Previously the diagnostic counter returned its disabled
  sentinel on PS4 unless the native-trace category was also enabled. This
  helps explain dropped radar draws; it does not make the radar visible.

The normal `native.toml` is unchanged. In particular, GPU detile remains off
because the handoff records GPU hangs with it enabled.

## Console validation

Host-side policy checks can be run on Windows with
`powershell.exe -NoProfile -ExecutionPolicy Bypass -File ps4/test-gameplay-policy.ps1`.
They cover 1,040 interval/frame/device/threshold combinations, verify intervals
1-4 keep their previous policy, and check the diagnostic source gate. These
model checks do not compile or execute the C++ renderer.

Build using `ps4/BUILD.md` and package using `AGENTS.md` and
`ps4/package/README.md`. Keep the matching unstripped ELF, EBOOT hash, commit,
and configuration with each run. A full Orbis build is required before use;
the Windows editing environment has no PS4 SDK or compiler installed.

1. First repeat the reported route with the normal configuration. Capture
   runtime logs and klog, noting timestamps for sunlit stutters and radar
   failures. Keep the full logs, not only filtered excerpts.
2. Back up the installed configuration. Change only
   `gta4_native_shadow_interval = 0` for a shadow-off A/B run of the same route,
   weather, camera, and save. This deliberately removes sun shadows: it is an
   opt-in diagnostic/performance tradeoff, not a visual-quality fix.
3. Confirm there are no stale shadows when switching off. Compare frame and
   recorder times, not just displayed FPS. Restore interval 2 afterward.
4. For a separate short radar test, add top-level `ps5_hud_trace = true` while
   keeping `log_level = "warn"`. Capture both `ps5 hud:` and
   `gta4-native-diag:` lines, with the radar visible and a GPS destination set.
   Disable HUD tracing after the short capture; it can increase logging cost.
5. Check boot, loading, interiors, sunlight, driving, audio continuity, and
   at least another 30-minute session for regressions.

Do not raise frame queues, memory limits, or thread priorities based solely
on the symptom report. The supplied run's timings and allocation/error logs
are needed to select the next change safely.
