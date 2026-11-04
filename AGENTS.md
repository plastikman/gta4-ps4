# GTA4 PS4 build and packaging rules

Current console and diagnostic state: `ps4/HANDOFF-2026-11-02.md`.

## Package the PS4 executable, never the host ELF

- Build with `cmake --build out/ps4 --target gta4 --parallel 1` whenever the renderer (`src/graphics/gta4_native/*`, or a header it includes such as `frame_stats.h` or `ps5_diag.h`) rebuilds: `graphics_system.cpp` with line tables is OOM-killed at `--parallel 2` on this 7 GB host. A rebuild of only the generated `gta4_recomp.*.cpp` files may use `--parallel 2` (~11-13 min). Always grep the build log for `Killed|error:` and check the EBOOT's time before packaging.
- The linked host executable at `out/ps4/glue/gta4-recomp/gta4` is an x86-64 ELF for the host/emulator path. **Never copy it to the package as `eboot.bin`.** Doing so creates a package that installs but fails on console launch with an invalid SELF/header error.
- For hardware packages, stage only the Orbis executable produced by `create-fself`: `out/ps4/glue/gta4-recomp/eboot.bin`.
- Before building the PKG, verify the staged `eboot.bin` matches that generated Orbis EBOOT (for example, compare SHA-256 hashes) and is not the host `gta4` file.
- Build the package with `PkgTool.Core pkg_build` from the GP4 staging directory, then run `PkgTool.Core pkg_validate --verbose` on the resulting PKG. Do not upload if validation fails.
- Keep the console package named `GTA4.pkg` with title ID `LBTY00001`. After upload, verify the remote file size matches the local package size before asking for installation.
- Keep game files out of the PKG. The installed game data lives under `/data/LibertyRecomp/game`; the GP4 manifest and staging tree must have no `game/` entries. Check the manifest before every package build.
- Do not cap PS4 startup/world-step trace events. A finite event budget has repeatedly hidden later startup progress and forced unnecessary package iterations. Sampling frequency may keep logs readable, but the trace must continue until the run advances or ends.

## Crash dump limits

- `.sorbisdmp` files from this PS4 setup are not decodable in our workflow. Do not spend time trying to decrypt or parse them. Use the klog crash report, guest trace, and symbolized backtrace to diagnose crashes.
