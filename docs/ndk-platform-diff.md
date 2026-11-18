# Separately pinned NDK platform exports

`tools/ndk_platform_diff.py` compares the API36 and API37 public platform-stub
slices inside an official NDK r30 archive, without loading a library or running
tools from that archive. The
[generated report](research/ndk-r30-api36-to-37/README.md) is another bounded A07
evidence increment for [issue #56](https://github.com/gesher-runtime/gesher/issues/56).
It complements the [SDK artifact comparison](sdk-platform-diff.md) from
[PR #151](https://github.com/gesher-runtime/gesher/pull/151).

This is static export metadata. It does not complete A07, establish API37 runtime
support or change Gesher's SDK level, build selection, permissions, capabilities
or application workflow. The existing native runtime inventory remains unchanged.

## Exact package and profile evidence

The [manifest](research/ndk-r30-api36-to-37/manifest.json) pins:

| Input | Observed value |
| --- | --- |
| Official package | `ndk;30.0.16248370` |
| Archive | `android-ndk-r30-linux.zip` |
| Archive size | 738,633,529 bytes |
| Official SHA-1 | `5107f898313790e449e87eee2183d9a20602dee9` |
| SHA-256 | `753611f410d002cfcd3f3dc2ef49aad532089d3180b436c060a90bf0fcb64df2` |
| Repository channel | `channel-0` / stable |
| Package release / revision | `r30` / `30.0.16248370` |
| Platform metadata range | API21–37 |
| Selected host directory | `toolchains/llvm/prebuilt/linux-x86_64` |
| Compiler metadata | `21.0.0`, based on `r574158c` |

The official [r30 release notes](https://github.com/android/ndk/wiki/Changelog-r30)
identify the compiler update and maximum sysroot API37. The precise archive
identity comes from the selected official repository record and the downloaded
bytes. The manifest records the 2026-11-18 `repository2-3.xml` snapshot SHA-256
`8db7cc1bd65bf8c9dffc3678a70d6dc98b02781a24d1ef0d31ecf111a55cc2d5` and its
436,016-byte size. Its selected Linux archive size and SHA-1 were checked
against the actual download.

The full text and hashes of `source.properties`, `meta/platforms.json`,
`meta/abis.json`, `meta/system_libs.json`, `AndroidVersion.txt` and
`clang_source_info.md` are preserved. The last two files identify the packaged
compiler and its reported source history; no compiler from the archive is run.
Every selected API-directory file is separately hashed. The archive and its
binary contents are not committed.

The tool checks the supplied archive's size/SHA-1/SHA-256, the selected official
archive record, the metadata text/hashes, and the exact selected-file set/hashes.
It rehashes the archive after reading. It does not fetch or authenticate the
historical repository snapshot during an offline comparison. The original
snapshot or current official package record can be checked separately; changed
metadata from a future repository snapshot must not silently replace these pins.

### ABI and API selection

The selected package declares these ABI profiles and supplies these slices:

| ABI | Triple from metadata | Bitness | Default | Minimum API | Selected slices |
| --- | --- | ---: | --- | ---: | --- |
| `armeabi-v7a` | `arm-linux-androideabi` | 32 | true | 21 | 36, 37 |
| `arm64-v8a` | `aarch64-linux-android` | 64 | true | 21 | 36, 37 |
| `x86` | `i686-linux-android` | 32 | true | 21 | 36, 37 |
| `x86_64` | `x86_64-linux-android` | 64 | true | 21 | 36, 37 |
| `riscv64` | `riscv64-linux-android` | 64 | false | 37 | 37 only |

Packaged ABI presence and a `default` flag do not establish Android device or
Gesher runtime support. The
[NDK build-system guide](https://android.googlesource.com/platform/ndk/+/master/docs/BuildSystemMaintainers.md)
directs consumers to the package metadata for programmatic ABI selection; the
actual pinned files above supply this report's values.

API36 and API37 have no aliases in this package. The selected directories are
therefore exactly `sysroot/usr/lib/<triple>/36/` and `.../37/`. For other pinned
inputs, the reader follows only explicit numeric aliases and records the entire
alias chain. It does not guess the nearest lower directory. The global API
range and each ABI's minimum are checked separately.

RISC-V's API36 profile is `below_abi_minimum`, with a **null denominator**.
Its API37 profile is measured independently; its exports are not additions from
a measured zero-export API36 baseline. A missing API directory or missing/
non-ELF expected system library similarly keeps the complete-profile denominator
null and prevents that ABI from being presented as a complete paired comparison.

Each of the nine supported slices contains 25 ELF platform libraries listed by
`system_libs.json`. In total, 225 ELF stubs are interpreted and all 293 selected
files are hashed. The remaining 68 files are explicitly listed as exclusions:
50 CRT/relocatable object files and 18 C++ linker scripts (`libc++.so` and
`libc++.a` in each slice). Linker-script text is retained, without resolving its
providers. C++ runtime libraries and static archives outside the selected API
directories are outside this platform-export scope.

## Measured change

| ABI | API36 exports | API37 exports | Added identities | Removed | Changed measured symbol fields |
| --- | ---: | ---: | ---: | ---: | ---: |
| `armeabi-v7a` | 4,808 | 4,862 | 54 | 0 | 0 |
| `arm64-v8a` | 4,720 | 4,774 | 54 | 0 | 0 |
| `x86` | 4,781 | 4,835 | 54 | 0 | 0 |
| `x86_64` | 4,720 | 4,774 | 54 | 0 | 0 |
| `riscv64` | null: below ABI minimum | 4,774 | not compared | not compared | not compared |

Each paired ABI adds 24 identities in `libandroid.so`, nine in
`libbinder_ndk.so`, eight in `libmediandk.so`, six in `libaaudio.so`, four in
`libc.so`, two in `libnativewindow.so`, and one in `liblog.so`. The changed-row
file contains 216 export additions across the four pairs, eight named version
definition additions, and 100 library byte-change records: **324 records** in
total. These overlapping observations are not 324 APIs or runtime defects.

All 25 paired libraries change bytes in each ABI, including libraries whose
measured export identities and attributes are unchanged. Raw byte changes are
therefore a separate category from export changes. The comparison also avoids
false changes from numeric version-index renumbering: for example,
`LIBC_DEPRECATED` moves from index 13 to index 14 in x86_64 libc when `LIBC_37` is
inserted, while its version name remains the same.

## Symbol interpretation

`tools/elf_symbol_inventory.py` reads section tables directly using Python's
standard library. It supports ELF32/ELF64, little-endian `ET_DYN` files with
ordinary section indices, dynamic symbol/string tables, GNU symbol versions,
version definitions/requirements, and dynamic SONAME/NEEDED names. It is bounded
to 32 MiB per ELF file, 8,192 sections and 200,000 dynamic symbols. Extended or
missing section tables, unsupported binding/type/visibility forms, and other
unrecognized encodings fail explicitly rather than reporting zero exports.

The [ELF symbol-table specification](https://gabi.xinuos.com/elf/05-symtab.html)
distinguishes definition sections, binding, type, visibility and size. The tool
keeps these observed fields separate. A named, defined dynamic symbol with
GLOBAL, WEAK or GNU_UNIQUE binding and DEFAULT or PROTECTED visibility is counted
as an externally visible export. Undefined references, local definitions,
HIDDEN/INTERNAL definitions, null entries and metadata entries have separate
categories. `st_other` is retained raw; only the four supported visibility values
are interpreted. No rule deletes a symbol merely because its name begins with
an underscore or its version name contains `PRIVATE` or `DEPRECATED`.

ELF visibility does not independently prove Android source-level public API
status. This report's public platform-stub scope comes from the NDK's selected
system-library artifacts; hidden headers, source API lists and declarations are
not parsed. Any private-looking version name remains visible as evidence.

The [GNU linker version documentation](https://sourceware.org/binutils/docs/ld/VERSION.html)
describes named versions and default aliases. This tool resolves version indices
to names and retains the GNU version hidden bit separately from ELF visibility.
Thus `name@V1` remains an export alongside `name@@V2`; its nondefault version does
not make it an ELF HIDDEN symbol. Indices 0/1 remain local/global scope markers,
not named versions. Version-definition flags, parent nodes, undefined-version
provider names, and dependency filenames are recorded separately.

Export identity is `(symbol, version name, version role, provider)`. Default
status, binding, type, visibility, raw `st_other`, definition kind and raw
OBJECT/TLS size are compared attributes. Duplicate records in one identity
group are retained in a sorted list, never overwritten. A default-alias change
or GLOBAL-to-WEAK change therefore remains visible without collapsing versions
by base name. Undefined references are not counted as exports.

GNU-linked fixtures can contain absolute zero-size OBJECT symbols naming version
nodes. The exact matching form is retained as `absolute_version_label` metadata.
Other absolute definitions remain eligible exports. The genuine NDK inputs have
no such version-label symbols, named undefined references, local/private or
hidden/internal definitions, or nondefault aliases. Their named records comprise
40,420 GLOBAL FUNC, 2,556 GLOBAL OBJECT and 72 WEAK FUNC entries across all nine
profiles. These are repeated per-file observations, not unique Android APIs.

Function addresses, instruction bytes and function sizes are not compared.
OBJECT/TLS `st_size` is a raw stub observation; it does not establish object
contents, pointer values, source type layouts or ABI-correct runtime storage.
No symbol lookup, relocation or version matching is executed.

## Acquire and reproduce

The comparison requires Python 3.10 or newer and its standard library. Download
the pinned official archive to an external directory, retaining its applicable
license. The command reads the ZIP directly; there is no global installation or
full toolchain extraction.

```sh
mkdir -p /tmp/gesher-ndk-platform-inputs
curl --fail --location \
  https://dl.google.com/android/repository/android-ndk-r30-linux.zip \
  --output /tmp/gesher-ndk-platform-inputs/android-ndk-r30-linux.zip

python3 tools/ndk_platform_diff.py \
  --archive /tmp/gesher-ndk-platform-inputs/android-ndk-r30-linux.zip \
  --manifest docs/research/ndk-r30-api36-to-37/manifest.json \
  --out-dir /tmp/gesher-ndk-platform-report

cmp /tmp/gesher-ndk-platform-report/summary.json \
  docs/research/ndk-r30-api36-to-37/summary.json
cmp /tmp/gesher-ndk-platform-report/delta.jsonl \
  docs/research/ndk-r30-api36-to-37/delta.jsonl
cmp /tmp/gesher-ndk-platform-report/README.md \
  docs/research/ndk-r30-api36-to-37/README.md
```

`summary.json` contains raw metadata, profile statuses and denominators, every
selected library hash/header, category/binding/type/visibility/version-form
counts, version definitions/requirements, exclusions and inventory digests.
`delta.jsonl` contains all changed records, with before/after evidence.
`README.md` is generated from the same result. Unchanged symbol names are not
repeated in the report; their complete per-library inventory is measured and
hashed and can be reproduced from the pinned archive.

Output contains no current timestamp or local input path. Canonical JSON uses
sorted keys, ASCII escapes and compact separators. Manifest and per-library
inventory digests hash that canonical form. The delta digest hashes exact JSONL
bytes with trailing LF newlines. Report files explicitly select their encoding
and LF output on every host, including Windows. Generator hashes are recorded. The same inputs
and generator produce byte-identical output in different directories.

## Validation and limits

Run the normal tests from the repository root:

```sh
python3 -m unittest discover -s tools/tests -p test_elf_symbol_inventory.py -v
python3 -m unittest discover -s tools/tests -p test_ndk_platform_diff.py -v
python3 -m unittest discover -s tools/tests -p test_native_api_inventory.py -v
```

The new suites contain ten tests. Reader fixtures compile isolated ELF32/ELF64
shared libraries with host C/C++ compilers and `-nostdlib`; they are never loaded
or run. GNU `readelf` independently checks the dynamic-symbol records. Fixtures
cover ordinary/weak/protected symbols, OBJECT/TLS sizes, GNU_UNIQUE, undefined
references and providers, default/nondefault aliases, parent version nodes,
ordinary absolute exports and version-label metadata. A static table fixture
retains local/hidden/internal entries that ordinary linkers may localize.
Profile fixtures cover metadata aliases, unsupported ABI/API combinations,
missing inputs, linker scripts, library introductions, distinct version
identities and byte-identical replay. Compiled-reader tests require a Linux
x86 host C/C++ toolchain with 32/64-bit ELF linking and GNU `readelf`; missing
tools are reported as skips, not as successful independent validation.

The author also cross-checked every one of the 225 genuine selected stubs against
GNU `readelf` 2.44: all 43,048 defined symbol rows and 225 null rows, ELF
class/machine/flags, SONAME/NEEDED and version-definition records agreed. No
version requirements occur in these stubs. The normal fixtures supply evidence
for branches absent from the actual package. All checks are local/static; no
CI rerun, device action or target execution is part of this increment.

NDK r30's API36 slice, its compiler source identity, the separately pinned SDK36
and SDK37 artifacts, and Gesher's `android-16.0.0_r4` source tag are separate
profiles. Their equality is not inferred from a shared major API number. Header
semantics, types, prototypes, inline functions, macros/constants, full C/C++ ABI,
runtime providers, Android17 source/module alignment, CompatChanges and CTS/CDD
qualification remain unmeasured. This report adds one evidence dimension; it
does not combine those missing dimensions into a coverage percentage.
