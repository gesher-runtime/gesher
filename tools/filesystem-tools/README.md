# Separate upstream filesystem programs

This recipe builds the original `mkfs.fat`, `fsck.fat`, `mkfs.exfat`,
`fsck.exfat`, `mke2fs`, and complete `e2fsck` as Android x86-64 PIE
executables for Gesher's existing ELF process loader. It does not copy or link
upstream GPL code into Gesher. The generated programs are separate optional
components. Their original source archives, licenses, and this build recipe
travel with the binaries in the same distribution.

On a Linux x86-64 build host with Python 3.11+ (with safe tar extraction), make, a native C compiler,
pkg-config and the pinned Android NDK:

```sh
python3 tools/filesystem-tools/build.py \
  --ndk /path/to/android-ndk-r27c \
  --ndk-archive /path/to/android-ndk-r27c-linux.zip \
  --work /path/outside/checkout/filesystem-tools-work \
  --output /path/to/artifact-root \
  --jobs 4
```

The result is `artifact-root/filesystem-tools/`. Install the complete directory,
including `sources/`, `licenses/`, and `receipts/`; the six executable names live
under `bin/`. `--verify-only` rechecks its receipt, every packaged file, ELF
properties, and current Gesher import registrations without compiling.
When rebuilding from the recipe shipped under `sources/`, pass
`--gesher-source /path/to/gesher-checkout` for that import audit. The programs
themselves are compiled entirely from the supplied upstream archives.

The source manifest pins archives by SHA256 and length. Source cache hits are
rehash-checked before fresh extraction. The NDK archive is authenticated against
its pinned publisher archive; every file of the supplied LLVM toolchain and
sysroot is checked against that archive before use. A changed archive or
extraction fails the build. Downloaded code, extracted trees and build products
must stay outside this repository. No upstream source modification is applied.

Libraries used by the tools are static: GNU libiconv provides FAT codepage and
label conversion; e2fsprogs' own uuid/blkid libraries supply its Android-supported
exFAT probe interface. The only dynamic dependency is Android `libc.so`, resolved
by Gesher's Bionic shim. No host Linux libraries or system filesystem utilities
are used by a guest executable. NLS message catalogs and unrelated e2fsprogs
utilities are not built. The actual FAT/exFAT checks and all e2fsck filesystem
repair passes remain upstream code.

`receipts/build.json` records inputs, recipe, configuration, binary hashes,
dynamic imports and their source registrations. That static audit establishes
that the imports have registered entry points; it does not prove their runtime
semantics. Windows execution through `gesher-exec`, damaged image repair, and
explicitly consented test-card operations are separate required validation.
The build never opens a disk device or invokes a generated filesystem program.

Distribution must retain the supplied corresponding source archives, notices
and recipe. The source archives include their complete license and per-file
copyright notices. The standalone programs keep those licenses; Gesher's core
keeps its existing license. The package also retains NDK runtime notices for
compiler support code statically included by the Android toolchain.
