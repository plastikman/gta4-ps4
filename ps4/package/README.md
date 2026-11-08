# Reproducing the PS4 package

`pkg.gp4` and `param.sfo` are the exact package metadata staged for CXIV.
The GP4 lists 27 entries, all game-free. The script copies those entries from
tracked source files, the freshly built EBOOT, and two OpenOrbis stub modules.
It verifies the GP4 against the staged files, builds the PKG, validates it, and
keeps the matching ELF for crash symbolization.

```sh
ps4/build-clean.sh
PS4_PKGTOOL_OPENSSL_LIB="$HOME/codex_scratch/libssl1.1/usr/lib/x86_64-linux-gnu" \
  ps4/package/build-package.sh
```

The default build and package directories are under
`~/codex_scratch/gta4-ps4-clean/`. Set `PS4_BUILD_DIR` and `PS4_PACKAGE_DIR`
to select others. The package script refuses to overwrite an existing stage.
It writes `out/GTA4.pkg` as a link to the validated content-ID PKG.

External inputs that Git does not contain:

- `PS4_SDK_BUNDLE` defaults to `~/ps4-gladius/sdk/orbis-sdk-v1` and provides
  the Orbis toolchain, Mesa RADV, and `PkgTool.Core`. This bundle identifies
  itself in `BUNDLE.txt` and carries `SHA256SUMS` plus `verify.sh`. Treat it as
  read-only. The scripts require the CXIV bundle fingerprint
  `b86155980a3f47d48f2dec7b55559afc41dcb6039020358ec001f5a0b6ec5639`
  for `BUNDLE.txt`.
- `PS4_STUBS_DIR` defaults to
  `~/codex_scratch/gta4-ps4-deps/openorbis-stubs`. Its `libc.prx` and
  `libSceFios2.prx` are the source-built OpenOrbis stubs used by CXIV. The
  script checks their SHA-256 values before packaging. The stubs can be built
  from the OpenOrbis sources using orbis-win's
  `scripts/build-openorbis-module-stubs.sh`.
- The Linux `PkgTool.Core` needs OpenSSL 1.1. Set
  `PS4_PKGTOOL_OPENSSL_LIB` to the directory containing `libssl.so.1.1`
  and `libcrypto.so.1.1`. It is already cached at the path in the command
  above on this machine.

The Xbox 360 game files are never staged; they remain on the console under
`/data/LibertyRecomp/game`. Exact byte-for-byte PKG reproduction has not yet
been demonstrated. A clean source build and package validation are the gates
before removing the old build tree.
