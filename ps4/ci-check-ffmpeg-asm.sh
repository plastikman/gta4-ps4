#!/usr/bin/env bash
# Host-side CI check for the PS4 FFmpeg NASM routines; does not need Orbis SDK.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
ffmpeg="$repo/glue/rexglue-sdk-main/thirdparty/FFmpeg"
patch="$repo/ps4/patches/FFmpeg.patch"
config="$repo/glue/rexglue-sdk-main/thirdparty/ffmpeg-overlay/config.asm"
run_id="${GITHUB_RUN_ID:-local}-$(date +%s)-$$"
output_dir="${PS4_CI_ASM_DIR:-$HOME/codex_scratch/gta4-ps4-ci-asm/$run_id}"

command -v nasm >/dev/null || { echo 'NASM is required' >&2; exit 1; }
git -C "$repo" submodule update --init --depth 1 -- glue/rexglue-sdk-main/thirdparty/FFmpeg
[[ -f "$config" ]] || { echo "Missing NASM config: $config" >&2; exit 1; }

if git -C "$ffmpeg" apply --reverse --check "$patch" >/dev/null 2>&1; then
  echo 'FFmpeg patch already applied'
else
  git -C "$ffmpeg" apply --check "$patch"
  git -C "$ffmpeg" apply --whitespace=nowarn "$patch"
  echo 'Applied FFmpeg patch'
fi

python3 - "$patch" "$config" <<'PY'
import re
import sys
from pathlib import Path

patch, config = (Path(p).read_text() for p in sys.argv[1:])
expected = {
    name: value
    for name, value in re.findall(r'^\+#define ((?:ARCH|HAVE)_[A-Za-z0-9_]+) (-?\d+)', patch, re.M)
}
actual = {
    name: value
    for name, value in re.findall(r'^%define ((?:ARCH|HAVE)_[A-Za-z0-9_]+) (-?\d+)', config, re.M)
}
# FFmpeg's patched C config enables HAVE_CPUNOP, but NASM 2.16 rejects the
# corresponding CPU directive, so the NASM overlay intentionally disables it.
expected.pop('HAVE_CPUNOP', None)
if expected != {k: v for k, v in actual.items() if k != 'HAVE_CPUNOP'} or actual.get('HAVE_CPUNOP') != '0':
    comparable = {k: v for k, v in actual.items() if k != 'HAVE_CPUNOP'}
    raise SystemExit(f'FFmpeg NASM config differs from patch: missing={expected.keys()-comparable.keys()}, extra={comparable.keys()-expected.keys()}, changed={[k for k in expected.keys() & comparable.keys() if expected[k] != comparable[k]]}, HAVE_CPUNOP={actual.get("HAVE_CPUNOP")}')
print(f'NASM config matches {len(expected)} ARCH_/HAVE_ definitions; HAVE_CPUNOP is disabled for NASM 2.16')
PY

mkdir -p "$output_dir"
sources=(
  libavutil/x86/cpuid.asm
  libavutil/x86/fixed_dsp.asm
  libavutil/x86/float_dsp.asm
  libavutil/x86/imgutils.asm
  libavutil/x86/lls.asm
  libavcodec/x86/dct32.asm
  libavcodec/x86/fft.asm
  libavcodec/x86/idctdsp.asm
  libavcodec/x86/imdct36.asm
)
for source in "${sources[@]}"; do
  object_name="${source//\//_}.o"
  nasm \
    -DHAVE_AV_CONFIG_H -DREX_PLATFORM_PS4=1 -D_USE_MATH_DEFINES \
    -I"$ffmpeg/" -I"$ffmpeg/libavutil/x86/" -I"$ffmpeg/libavcodec/x86/" \
    -P"$config" -DPIC -f elf64 \
    -o "$output_dir/$object_name" "$ffmpeg/$source"
done
echo "Assembled ${#sources[@]} FFmpeg objects into $output_dir"
