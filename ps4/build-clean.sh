#!/usr/bin/env bash
# Run from any checkout. Use a disposable checkout and an external build tree.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
sdk="${PS4_SDK_BUNDLE:-$HOME/ps4-gladius/sdk/orbis-sdk-v1}"
build_dir="${PS4_BUILD_DIR:-$HOME/codex_scratch/gta4-ps4-clean/build}"
jobs="${PS4_BUILD_JOBS:-1}"
dxc_root="${PS4_DXC_ROOT:-$HOME/codex_scratch/gta4-ps4-deps/dxc}"

[[ -f "$sdk/toolchain/orbis-sdk.cmake" ]] || { echo "Missing SDK bundle: $sdk" >&2; exit 1; }
expected_bundle=b86155980a3f47d48f2dec7b55559afc41dcb6039020358ec001f5a0b6ec5639
actual_bundle="$(sha256sum "$sdk/BUNDLE.txt" | cut -d' ' -f1)"
[[ "$actual_bundle" == "$expected_bundle" ]] || { echo "Unexpected SDK bundle: $actual_bundle" >&2; exit 1; }
[[ "$jobs" =~ ^[1-4]$ ]] || { echo 'PS4_BUILD_JOBS must be 1-4' >&2; exit 1; }
command -v cmake >/dev/null
command -v ninja >/dev/null
command -v nasm >/dev/null
command -v curl >/dev/null
for spec in \
  'bin/dxc 3dfc856b76291ffc7e45e21fd37ab80e85b34101de76a03253df6608785c92a0' \
  'lib/libdxcompiler.so d40e4a7b3b46490313f4bfd08101946483a673831b3ea65c93d746de1c2f9216' \
  'lib/libdxil.so 730b31420ea0c38f711c1eb581613bfd0797c53371a55f721838b29d10881c8c'; do
  read -r relative expected <<< "$spec"
  [[ -f "$dxc_root/$relative" ]] || { echo "Missing DXC input: $dxc_root/$relative" >&2; exit 1; }
  [[ "$(sha256sum "$dxc_root/$relative" | cut -d' ' -f1)" == "$expected" ]] || {
    echo "Unexpected DXC input: $dxc_root/$relative" >&2; exit 1;
  }
done
export TMPDIR="${PS4_TMP_DIR:-$build_dir/tmp}"
mkdir -p "$TMPDIR"

# The Orbis bundle includes the matching zlib static archive but omits its
# headers. gta4-recomp's installer uses the public zlib API, so pair the
# archive with the headers from the same pinned upstream release.
zlib_root="$build_dir/deps/zlib-1.3.1"
zlib_archive="$build_dir/deps/zlib-v1.3.1.tar.gz"
zlib_sha256=17e88863f3600672ab49182f217281b6fc4d3c762bde361935e436a95214d05c
mkdir -p "$(dirname "$zlib_root")"
if [[ ! -f "$zlib_archive" ]]; then
  curl --fail --location --proto '=https' --tlsv1.2 --silent --show-error \
    -o "$zlib_archive" \
    https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz
fi
echo "$zlib_sha256  $zlib_archive" | sha256sum --check --status || {
  echo "Unexpected zlib archive: $zlib_archive" >&2
  exit 1
}
if [[ ! -f "$zlib_root/zlib.h" || ! -f "$zlib_root/zconf.h" ]]; then
  mkdir -p "$zlib_root"
  tar --no-same-owner --strip-components=1 -xzf "$zlib_archive" -C "$zlib_root"
fi

mapfile -t submodules < <(git -C "$repo" config -f .gitmodules --get-regexp '^submodule\..*\.path$' | awk '$2 ~ /^glue\/rexglue-sdk-main\/thirdparty\// {print $2}')
git -C "$repo" submodule update --init -- "${submodules[@]}" tools/XenosRecomp
git -C "$repo/tools/XenosRecomp" submodule update --init -- \
  thirdparty/zstd thirdparty/smol-v thirdparty/dxc-bin

for name in FFmpeg glslang libmspack ReXGlue sdl3 spdlog XenosRecomp XenosRecomp-zstd; do
  case "$name" in
    ReXGlue) sub="$repo" ;;
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
  -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/orbis-sdk.cmake" \
  -DPS4_ZLIB_INCLUDE_DIR="$zlib_root" \
  '-DCMAKE_ASM_COMPILE_OBJECT=<CMAKE_ASM_COMPILER> <DEFINES> <INCLUDES> <FLAGS> -c <SOURCE> -o <OBJECT>' \
  -DGTA4_NATIVE_DXC_EXECUTABLE="$dxc_root/bin/dxc" \
  -DGTA4_NATIVE_DXC_LIBRARY_PATH="$dxc_root/lib"
cmake --build "$build_dir" --target gta4 --parallel "$jobs"

eboot="$build_dir/glue/gta4-recomp/eboot.bin"
elf="$build_dir/glue/gta4-recomp/gta4"
[[ -s "$eboot" && -s "$elf" ]] || { echo 'Build finished without EBOOT or ELF' >&2; exit 1; }
sha256sum "$eboot" "$elf"
