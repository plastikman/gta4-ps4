# Building GTA IV for PS4

The PS4 target uses the Graine/ReXGlue application and the Orbis RADV bundle
already used by `ps4-gladius`. The bundle keeps the OpenOrbis SDK, matching
Mesa headers, RADV archive, and Vulkan entry point shim together.

Configure from the repository root, pointing CMake at that bundle's toolchain:

```sh
cmake -S ps4 -B out/ps4 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$HOME/ps4-gladius/sdk/orbis-sdk-v1/toolchain/orbis-sdk.cmake"
cmake --build out/ps4 --target gta4 --parallel 4
```

The toolchain creates the Orbis executable at
`out/ps4/glue/gta4-recomp/eboot.bin` after linking. Package that EBOOT with
the game-free GP4 stage; the installed Xbox 360 game data lives separately at
`/data/LibertyRecomp/game` on the console. Never package the host `gta4` ELF
or any `game/` files. See [the current PS4 handoff](HANDOFF-2026-11-02.md)
for the exact package, console status, and next diagnostic run.
