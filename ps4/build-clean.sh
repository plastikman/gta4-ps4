#!/usr/bin/env bash
# Run from any checkout. Build output stays outside the source tree by default.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
sdk="${PS4_SDK_BUNDLE:-$HOME/ps4-gladius/sdk/orbis-sdk-v1}"
build_dir="${PS4_BUILD_DIR:-$HOME/codex_scratch/gta4-ps4-clean/build}"
jobs="${PS4_BUILD_JOBS:-1}"

[[ -f "$sdk/toolchain/orbis-sdk.cmake" ]] || { echo "Missing SDK bundle: $sdk" >&2; exit 1; }
expected_bundle=b86155980a3f47d48f2dec7b55559afc41dcb6039020358ec001f5a0b6ec5639
actual_bundle="$(sha256sum "$sdk/BUNDLE.txt" | cut -d' ' -f1)"
[[ "$actual_bundle" == "$expected_bundle" ]] || { echo "Unexpected SDK bundle: $actual_bundle" >&2; exit 1; }
[[ "$jobs" =~ ^[1-4]$ ]] || { echo 'PS4_BUILD_JOBS must be 1-4' >&2; exit 1; }
command -v cmake >/dev/null
command -v ninja >/dev/null

mapfile -t submodules < <(git -C "$repo" config -f .gitmodules --get-regexp '^submodule\..*\.path$' | awk '$2 ~ /^glue\/rexglue-sdk-main\/thirdparty\// {print $2}')
git -C "$repo" submodule update --init -- "${submodules[@]}" tools/XenosRecomp
git -C "$repo/tools/XenosRecomp" submodule update --init -- \
  thirdparty/zstd thirdparty/smol-v thirdparty/dxc-bin

for name in FFmpeg glslang libmspack sdl3 spdlog XenosRecomp XenosRecomp-zstd; do
  case "$name" in
    XenosRecomp) sub="$repo/tools/XenosRecomp" ;;
    XenosRecomp-zstd) sub="$repo/tools/XenosRecomp/thirdparty/zstd" ;;
    *) sub="$repo/glue/rexglue-sdk-main/thirdparty/$name" ;;
  esac
  patch="$repo/ps4/patches/$name.patch"
  if git -C "$sub" apply --reverse --check "$patch" 2>/dev/null; then
    echo "Patch already applied: $name"
  else
    git -C "$sub" apply --check "$patch"
    git -C "$sub" apply "$patch"
    echo "Applied patch: $name"
  fi
done

cmake -S "$repo/ps4" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/orbis-sdk.cmake"
cmake --build "$build_dir" --target gta4 --parallel "$jobs"

eboot="$build_dir/glue/gta4-recomp/eboot.bin"
elf="$build_dir/glue/gta4-recomp/gta4"
[[ -s "$eboot" && -s "$elf" ]] || { echo 'Build finished without EBOOT or ELF' >&2; exit 1; }
sha256sum "$eboot" "$elf"
