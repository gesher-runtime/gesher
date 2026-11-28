# Separate upstream filesystem programs

This recipe builds upstream `mkfs.fat`, `fsck.fat`, `mkfs.exfat`,
`fsck.exfat`, `mke2fs`, and complete `e2fsck` as Android x86-64 PIE
executables for Gesher's existing ELF process loader. The exFAT programs include
the narrowly scoped capability-query port described below; their filesystem
format, check and repair algorithms remain upstream code. Upstream GPL code is
not copied or linked into Gesher's core. The generated programs are separate
optional components. Their original source archives, licenses, build recipe,
port helper and complete transformation instructions travel with the binaries
in the same distribution.

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
under `bin/`. `--verify-only` rechecks its receipt, every packaged file, original
archive and provenance digest, ELF properties, and current Gesher import
registrations without compiling. It also recomputes the exFAT transformation
from the supplied original archive and checks its resulting source hash and
edit receipt against the pinned manifest.
When rebuilding from the recipe shipped under `sources/`, pass
`--gesher-source /path/to/gesher-checkout` for that import audit. The programs
are compiled from the supplied upstream archives and the shipped exFAT port.

The source manifest pins archives by SHA256 and length. Source cache hits are
rehash-checked before fresh extraction. The NDK archive is authenticated against
its pinned publisher archive; every file of the supplied LLVM toolchain and
sysroot is checked against that archive before use. A changed archive or
extraction fails the build. Downloaded code, extracted trees and build products
must stay outside this repository. The port is applied only to a fresh external
source tree, after checking the original file and every port input against the
manifest. Its transformed source hash is pinned too. An already modified source
tree, changed port source or unexpected upstream input fails validation.

## exFAT opened-target capability port

Upstream exfatprogs 1.4.5 checks the release returned by `uname()` before opening
the target, assuming a Linux release of at least 2.6 implies the required block
and sysfs interfaces. Gesher deliberately reports `0.0.0-gesher`; changing that
release would claim Linux capabilities the runtime may not implement. The
separate exFAT tool port replaces this release comparison with actual interface
queries. It does not alter Gesher's `uname()` implementation or the original
CardWake APK.

`port_exfat.py` removes the obsolete version preflight in the external
`lib/libexfat.c` and calls the independently written `exfat_capabilities.h`
after the target's existing `open()` succeeds. For a block target, the helper
uses `fstat()` on that exact descriptor, obtains its actual byte length and
logical sector size with `BLKGETSIZE64` and `BLKSSZGET`, and opens the sysfs
directory for its `st_rdev` major/minor identity. It requires a matching `size`
attribute. A partition additionally requires its real `partition`, `start`
and parent `size` attributes, a bounded extent within that parent, and a sector
aligned offset. A whole disk must have neither partition nor start metadata.
Malformed, missing, denied or inconsistent required data fails the operation;
the port does not guess block geometry or fall back to an assumed sector size.

The helper rechecks the opened descriptor's identity and geometry after the
metadata reads. The integrated upstream device-info path checks them again
before consuming the result, including agreement with the target's `lseek()`
size. It uses the validated block offset, partition status and sector size.
The existing runtime still checks media identity, grant lifetime, write consent
and revocation for subsequent operations; this read-only metadata helper grants
no access and opens no additional block target. Regular file images retain the
upstream size, sector selection and image handling, without requiring block
ioctls or sysfs metadata. The actual upstream format, check, repair and I/O
implementations remain in place, with their real results and errors.

Every build runs `test_exfat_port.py --sanitize` before configuring the upstream
packages. That suite checks transformation integrity and rejection controls,
exercises the capability helper with explicit isolated host adapters and
temporary metadata files, and checks the upstream device-info entry point's
regular file behavior. Its output is retained in
`receipts/logs/exfatprogs.log`. The native host checks use AddressSanitizer and
UndefinedBehaviorSanitizer; caller-supplied `ASAN_OPTIONS` and `UBSAN_OPTIONS`
are forwarded only to that test subprocess. These fixtures do not exercise a
physical card, Windows device handles or Android ELF execution.

The source manifest pins the helper, transformation script, test script,
original and transformed source files, and port license. The schema-2 build
receipt records each edit's offsets, removed and inserted byte hashes, and a
digest of the complete transformation description. No upstream function body
is stored in this repository: the script derives edited source from the
authenticated archive outside the checkout. The original archive plus the
exact supplied transformation, helper and recipe reconstruct the modified
corresponding source used for the exFAT binaries.

## Runtime and distribution

Libraries used by the tools are static: GNU libiconv provides FAT codepage and
label conversion; e2fsprogs' own uuid/blkid libraries supply its Android-supported
exFAT probe interface. The only dynamic dependency is Android `libc.so`, resolved
by Gesher's Bionic shim. No host Linux libraries or system filesystem utilities
are used by a guest executable. NLS message catalogs and unrelated e2fsprogs
utilities are not built. The actual FAT/exFAT checks and all e2fsck filesystem
repair passes remain upstream code.

`receipts/build.json` records inputs, recipe, source transformations,
configuration, binary hashes, dynamic imports and their source registrations.
That static audit establishes
that the imports have registered entry points; it does not prove their runtime
semantics. Windows execution through `gesher-exec`, damaged image repair, and
explicitly consented test-card operations are separate required validation.
The build and its host suite never open a physical disk device or execute the
generated Android filesystem programs.

Distribution must retain the supplied corresponding source archives, notices
and recipe. The source archives include their complete license and per-file
copyright notices. The standalone programs keep those licenses; Gesher's core
keeps its existing license. The independently written capability header is
available under either Apache-2.0 or MIT, with the full MIT notice in the
header, so the separate upstream programs may use the MIT option. The build
and transformation scripts and tests use Apache-2.0, supplied in
`licenses/gesher-port/LICENSE`. The package also retains NDK runtime notices
for compiler support code statically included by the Android toolchain.
