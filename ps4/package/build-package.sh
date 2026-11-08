#!/usr/bin/env bash
# Build a game-free PS4 package from an Orbis EBOOT and pinned package inputs.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
sdk="${PS4_SDK_BUNDLE:-$HOME/ps4-gladius/sdk/orbis-sdk-v1}"
build_dir="${PS4_BUILD_DIR:-$HOME/codex_scratch/gta4-ps4-clean/build}"
package_dir="${PS4_PACKAGE_DIR:-$HOME/codex_scratch/gta4-ps4-clean/package}"
stubs="${PS4_STUBS_DIR:-$HOME/codex_scratch/gta4-ps4-deps/openorbis-stubs}"
eboot="$build_dir/glue/gta4-recomp/eboot.bin"
elf="$build_dir/glue/gta4-recomp/gta4"
tool="$sdk/sdk/bin/linux/PkgTool.Core"
stage="$package_dir/stage"

for file in "$eboot" "$elf" "$tool" "$stubs/libc.prx" "$stubs/libSceFios2.prx"; do
  [[ -s "$file" ]] || { echo "Missing package input: $file" >&2; exit 1; }
done
expected_bundle=b86155980a3f47d48f2dec7b55559afc41dcb6039020358ec001f5a0b6ec5639
actual_bundle="$(sha256sum "$sdk/BUNDLE.txt" | cut -d' ' -f1)"
[[ "$actual_bundle" == "$expected_bundle" ]] || { echo "Unexpected SDK bundle: $actual_bundle" >&2; exit 1; }
[[ "$(sha256sum "$stubs/libc.prx" | cut -d' ' -f1)" == 7b857f4a5f405e58d046914f883b1dad6ac0d9574e685c4d6fe834222f894c0a ]] || { echo 'Unexpected libc.prx' >&2; exit 1; }
[[ "$(sha256sum "$stubs/libSceFios2.prx" | cut -d' ' -f1)" == cf447a458f86c96768a56a4b2be9edf042e6cae27084c80f925478b02ca1194d ]] || { echo 'Unexpected libSceFios2.prx' >&2; exit 1; }

mkdir -p "$package_dir"
[[ ! -e "$stage" ]] || { echo "Stage already exists: $stage (choose a new PS4_PACKAGE_DIR)" >&2; exit 1; }
mkdir -p "$stage/Resources" "$stage/sce_module" "$stage/sce_sys" "$package_dir/out"
cp "$eboot" "$stage/eboot.bin"
cp "$repo/ps4/package/pkg.gp4" "$stage/pkg.gp4"
cp "$repo/ps4/package/param.sfo" "$stage/sce_sys/param.sfo"
cp "$repo/LibertyRecomp/res/ps4/sce_sys/icon0.png" "$stage/sce_sys/icon0.png"
cp "$repo/LibertyRecompLib/aes_key.bin" "$stage/Resources/aes_key.bin"
cp "$stubs/libc.prx" "$stubs/libSceFios2.prx" "$stage/sce_module/"

python3 - "$repo" "$stage" <<'PY'
import sys
import shutil
from pathlib import Path
from xml.etree import ElementTree as ET
repo, stage = map(Path, sys.argv[1:])
paths = [node.attrib['targ_path'] for node in ET.parse(stage/'pkg.gp4').findall('./files/file')]
if len(paths) != len(set(paths)) or any(p.startswith('game/') or '/game/' in p for p in paths):
    raise SystemExit('GP4 contains duplicates or game data')
for p in paths:
    if p.startswith('Resources/button_prompts/'):
        source = repo/'LibertyRecompLib/private/button_prompts'/Path(p).relative_to('Resources/button_prompts')
    elif p.startswith('Resources/font_atlases/'):
        source = repo/'LibertyRecompLib/font_atlases'/Path(p).relative_to('Resources/font_atlases')
    else:
        continue
    target = stage/p
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
staged = {str(p.relative_to(stage)) for p in stage.rglob('*') if p.is_file()} - {'pkg.gp4'}
if staged != set(paths):
    raise SystemExit(f'GP4/stage mismatch: missing={set(paths)-staged}, extra={staged-set(paths)}')
print(f'Validated {len(paths)} game-free package entries')
PY
cmp "$eboot" "$stage/eboot.bin"
cp "$elf" "$package_dir/gta4.elf"

export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
if [[ -n "${PS4_PKGTOOL_OPENSSL_LIB:-}" ]]; then
  export LD_LIBRARY_PATH="$PS4_PKGTOOL_OPENSSL_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
(cd "$stage" && "$tool" pkg_build pkg.gp4 "$package_dir/out" > "$package_dir/build.log" 2>&1)
pkg="$package_dir/out/IV0000-LBTY00001_00-GTA4PS4000000000.pkg"
[[ -s "$pkg" ]] || { echo "Package missing: $pkg" >&2; exit 1; }
"$tool" pkg_validate --verbose "$pkg" > "$package_dir/validate.log" 2>&1
if grep -q '\[FAIL\]' "$package_dir/validate.log"; then
  cat "$package_dir/validate.log" >&2
  exit 1
fi
ln -sf "$(basename "$pkg")" "$package_dir/out/GTA4.pkg"
sha256sum "$eboot" "$pkg" > "$package_dir/SHA256SUMS"
cat "$package_dir/SHA256SUMS"
