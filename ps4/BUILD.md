# Building GTA IV for PS4

The PS4 target uses the Graine/ReXGlue application and the Orbis RADV bundle
already used by `ps4-gladius`. The bundle keeps the OpenOrbis SDK, matching
Mesa headers, RADV archive, and Vulkan entry point shim together.

The supported clean-build host is Linux x86-64 with Git, CMake 3.25 or newer,
Ninja, NASM 2.16 or newer, Python 3, and `sha256sum`. The Orbis SDK and host
shader compiler are external inputs; their pinned locations and fingerprints
are checked by the script. Packaging additionally needs OpenSSL 1.1 for
`PkgTool.Core`.

From a fresh checkout, run:

```sh
ps4/build-clean.sh
```

This initializes the glue submodules and XenosRecomp's required dependencies,
applies the committed PS4 patches, configures CMake, and builds with one job
by default. Set `PS4_SDK_BUNDLE`,
`PS4_BUILD_DIR`, or `PS4_BUILD_JOBS` (1 to 4) to override the defaults. On this
7 GB host, keep `PS4_BUILD_JOBS=1` for a clean build. The toolchain creates
`$PS4_BUILD_DIR/glue/gta4-recomp/eboot.bin` and the matching unstripped `gta4`
ELF used for symbolization.
The script also downloads and verifies zlib 1.3.1 headers into the build tree;
the Orbis bundle supplies the matching `libz.a` but does not include those
headers.
The build also uses the pinned host DXC under `~/codex_scratch/gta4-ps4-deps/dxc`
(`PS4_DXC_ROOT` to override). Temporary shader files are placed under the
build directory, outside `/tmp`.

GitHub Actions runs a NASM assembly check on pull requests. The full Orbis
build and game-free package validation run on pushes to `ps4-port` using the
`gta4-ps4-build` Kubernetes runner. The workflow checks out the pinned
Orbis-Win runner setup, attaches its `/orbis-cache` SDK and dependencies,
downloads the pinned DXC archive, and builds the OpenOrbis stubs. Fork pull
requests run only on GitHub-hosted runners.

To make a package, see [package/README.md](package/README.md). The installed
Xbox 360 game data lives separately at `/data/LibertyRecomp/game` on the
console. Never package the host `gta4` ELF or any `game/` files. See
[the current PS4 handoff](HANDOFF-2026-11-02.md) for console status.
