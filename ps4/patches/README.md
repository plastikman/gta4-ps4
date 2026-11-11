# Submodule patches for the PS4 build

The submodules point at their upstream repositories, so the PS4 port's edits to
them live here as patches. After cloning and `git submodule update --init
--recursive`, apply each one from inside its submodule:

| Patch | Apply in |
|---|---|
| FFmpeg.patch (includes the new `config_ps5_x86_64.h`) | `glue/rexglue-sdk-main/thirdparty/FFmpeg` |
| ReXGlue.patch | repository root (fixes restoring an unset CMake project include) |
| glslang.patch | `glue/rexglue-sdk-main/thirdparty/glslang` |
| libmspack.patch | `glue/rexglue-sdk-main/thirdparty/libmspack` |
| sdl3.patch | `glue/rexglue-sdk-main/thirdparty/sdl3` |
| spdlog.patch | `glue/rexglue-sdk-main/thirdparty/spdlog` |
| XenosRecomp.patch | `tools/XenosRecomp` |
| XenosRecomp-zstd.patch | `tools/XenosRecomp/thirdparty/zstd` |

For example: `cd glue/rexglue-sdk-main/thirdparty/sdl3 && git apply ../../../../ps4/patches/sdl3.patch`.
Each patch was checked to reverse-apply cleanly against the tree it was taken
from (2026-11-04).
