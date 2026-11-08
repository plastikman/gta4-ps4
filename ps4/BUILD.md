# Building GTA IV for PS4

The PS4 target uses the Graine/ReXGlue application and the Orbis RADV bundle
already used by `ps4-gladius`. The bundle keeps the OpenOrbis SDK, matching
Mesa headers, RADV archive, and Vulkan entry point shim together.

From a fresh checkout, run:

```sh
ps4/build-clean.sh
```

This initializes the glue submodules and XenosRecomp's zstd dependency, applies the committed PS4 patches,
configures CMake, and builds with one job by default. Set `PS4_SDK_BUNDLE`,
`PS4_BUILD_DIR`, or `PS4_BUILD_JOBS` (1 to 4) to override the defaults. On this
7 GB host, keep `PS4_BUILD_JOBS=1` for a clean build. The toolchain creates
`$PS4_BUILD_DIR/glue/gta4-recomp/eboot.bin` and the matching unstripped `gta4`
ELF used for symbolization.

To make a package, see [package/README.md](package/README.md). The installed
Xbox 360 game data lives separately at `/data/LibertyRecomp/game` on the
console. Never package the host `gta4` ELF or any `game/` files. See
[the current PS4 handoff](HANDOFF-2026-11-02.md) for console status.
