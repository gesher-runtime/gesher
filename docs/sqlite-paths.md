# SQLite file identity across the guest and Windows namespaces

`ContextImpl.openOrCreateDatabase()` passes a guest pathname to SQLite JNI.
The SQLite library uses the Win32 VFS, which previously interpreted
`/mnt/c/Users/.../databases/same.db` as a rooted path on the current Windows
drive. On the Windows CI runner whose working directory is on D:, this became
`D:\mnt\c\Users\...`, while Java/POSIX file operations had created the real
directory on C:. SQLite correctly returned `SQLITE_CANTOPEN` for that wrong
location. The failure was observed in `AppStorageContextTest` in run
[43980723130](https://github.com/gesher-runtime/gesher/actions/runs/43980723130)
at PR head `976ab27e825665b6d922b8f3c82465d9ee2e01da`, on Windows 2022 and 2025.

## Production boundary

The patch to the pinned Android 16 r4 SQLite 3.50.4 amalgamation changes
`winFullPathname`, before its call to the original Win32 canonicalizer.
`compat/posix/src/sqlite_path.cc` uses the existing `DiskPath` mapper, including
`/mnt/<drive>`, `/mnt/unc`, UTF-8 conversion, reserved-character encoding and
virtual-file rejection. It then calls the original `winFullPathnameNoMutex`.
Native canonicalization errors are returned unchanged. Mapping failures are
`SQLITE_CANTOPEN`; they do not become successful opens or disk virtual files.
The native canonicalizer's final UTF-8 copy also checks the actual output
length against both the supplied capacity and VFS limit. An expanded path that
does not fit returns `SQLITE_CANTOPEN_FULLPATH` before copying; stock SQLite's
successful `snprintf` truncation is not allowed to publish a different name.
SQLite's existing `/C:/...` and extended-path `file:` URI normalization still
runs before mapping. Native drive, relative and rooted paths retain the shared
POSIX mapper's semantics (ADR [0045](decisions/0045-posix-paths-tz-procfs.md)).

This is the VFS boundary, so it applies to databases opened by JNI, SQL
`ATTACH`, and `VACUUM INTO`. No package names or fixture paths are special.
The original Context database creation, data migration and deletion workload
remains intact.

SQLite's pager calls `xFullPathname` before allocating the database filename,
URI parameters and journal/WAL names. The mapper operates before that
allocation. `xOpen` receives the original core-owned `sqlite3_filename` pointer;
no callback substitutes a `std::string` or loses the hidden metadata. This is
required by the [VFS contract](https://www.sqlite.org/c3ref/vfs.html) and
[URI parameter API contract](https://www.sqlite.org/c3ref/uri_boolean.html).
The [URI parser](https://www.sqlite.org/uri.html) continues to interpret
`mode`, `cache`, percent encoding and query parameters itself. `:memory:`,
empty names and URI memory databases keep SQLite's own behavior.

## Downstream files and limits

In the pinned `sqlite3.c`, `sqlite3PagerOpen` constructs `zJournal` and
`zWal` from the canonical `zPathname`. Pager `xAccess`/`xDelete` calls use
those names. The multi-database commit path constructs its super-journal
name from `sqlite3BtreeGetFilename` and writes canonical child-journal names
inside it. Recovery reads those names unchanged. Win32 shared memory derives
its name from the original file's canonical `zPath`. These paths remain
SQLite's stock Win32 file, locking, sync, delete and shared-memory operations;
they are not remapped on each operation.

The default unnamed-file path still uses Win32's native temporary-file
generator, including `DELETEONCLOSE`. Gesher's existing
`SQLITE_TEMP_STORE=3` configuration keeps SQL temporary storage in memory.
The test checks both behaviors separately.

Two deprecated, process-global directory override PRAGMAs,
`temp_store_directory` and Windows-only `data_store_directory`, pass raw
directory strings to `xAccess` and store them outside the pager's canonical
filename path. Direct assignments to `sqlite3_temp_directory` and
`sqlite3_data_directory` have the same separate configuration boundary.
This correction does not add guest-namespace support to those existing native
directory override APIs. Ordinary SQLite/Android database operations and
native temporary-file generation do not use them. No new claim is made about
remote UNC servers, ACLs, media failure, power-loss recovery, or cross-process
locking on Windows until those environments are exercised.

## Evidence and reproducible gates

`tools/test_sqlite_paths.py` accepts pristine `sqlite3.c` and `sqlite3.h`
from the exact pinned tag, or downloads and verifies them with `--fetch`.
Their SHA-256 values are enforced in the runner. It applies the actual AOSP
patch with `git apply --check`, compiles the patched callback and production
bridge, and extracts the unchanged POSIX path classifier/mapping functions.
UTF conversion, `GetFullPathNameW`, font lookup and procfs/device backends are
explicit portable adapters. The Windows canonicalizer is modeled with distinct
C: and D: roots; actual SQL parsing, URI metadata, pager behavior and file IO
use the pinned engine's Unix VFS. Its pathname limit matches the default
Win32 VFS's 1040 UTF-8 bytes (260 Windows path characters times four).
This is a production-boundary regression,
not execution of Win32 or ART.

```sh
python3 tools/test_sqlite_paths.py --fetch --sanitize --negative-control
# Or use previously downloaded, pristine pinned sources:
python3 tools/test_sqlite_paths.py --sqlite-source /path/to/sqlite-3500400 \
  --sanitize --negative-control
```

The negative control compiles the original unpatched `winFullPathname` and
requires real failures of guest-path open, SQL `ATTACH` and `VACUUM INTO`.
The corrected test performs real insert/query/rollback, multi-database commit
with super-journal, WAL and SHM creation/cleanup, read-only and missing-file
errors, URI metadata observation at `xOpen`, UTF-8 and encoded names, virtual
path rejection, in-memory databases and native temporary-file IO. Boundary
fault checks cover native extended-error propagation, small output capacity,
and a raw guest name below the VFS limit whose reserved-character encoding
expands beyond it. The portable canonicalizer uses the exact new bounded-copy
helper; its negative control uses stock successful-truncation behavior.

The default CMake target `sqlite_path_test` compiles the same test against the
production SQLite library and POSIX mapper, without the portable adapters.
CI runs `dist/sqlite_path_test.exe` on the Windows 2022 and 2025 native shards.
The existing `AppStorageContextTest` remains the ART/Context integration gate.
The portable run and Windows cross-compilation cannot establish either Windows
runtime pass. The new exact PR head must complete those Windows CI gates before
this correction is considered runtime-qualified.
