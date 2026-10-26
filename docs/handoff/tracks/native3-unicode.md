# Verified stage: pinned Unicode and Process proc natives

Full ci 40386670586, attempt 1, source
6f6ac6fa9519207797ee5b997e0ae1e5b9e77049, including verified oatdump
integration 634a273. All 28 jobs personally read: 22 success, six skipped.
Both Windows summaries end with 0 test(s) failed. No retry.

## Verified behavior

The production bridge uses pinned ICU4C 76 data, replacing Windows' ICU DLL
and 16-bit CRT predicates. Bionic's exact ASCII branch, binary properties,
category bounds, simple case mappings and invalid sentinels agree with an
independent pinned ICU4X 2.0.0-beta2 reference. All 1,114,112 code points and
four out-of-range values match for 14 plain and 14 locale-suffixed System V
shim entries on each Windows version. All 28 expected whole-domain digests
must match; this is not a symbol-presence test.

ci 40374405950 records the unmodified bionic wrapper's reference outputs;
ci 40376822140 verifies its saved Cargo lock, 14 digests and 11 boundary rows.
The lock was recovered from logs and SHA-256 checked. No local Actions
artifact download, AOSP/ICU build or WSL occurred.

Actual scoped PASS lines per runner (121799290171/121799290021):

- native3: every Unicode code point and invalid sentinel matches pinned bionic ICU4X, plain and locale forms.
- native2 (x86_64/arm64): unicode through dalvikvm, pinned ICU4X boundaries; plain/_l/wctype/wctrans and errors.
- All four native2 APK checks require the same Unicode result, one per ABI
  in single-process/device modes, before accepting the all-passed verdict.
- native3: Process proc natives use real procfs, full-width values and the Android field format.
- Existing Bitmap and Minikin font checks pass in both ABIs and APK modes.

The NDK checks cover 11 independent rows, class/transform handles and
invalid-name/handle EINVAL. Four baseline missing entries (iswalnum_l,
iswgraph_l, wctrans_l, towctrans_l) receive scoped evidence in the inventory;
baseline ownership/counts remain historical. Process readProcLines/
readProcFile/parseProcLine cover actual procfs, shared output indexing,
quoted/parenthesized/character fields, full-width counters, floats, long
files, truncation, sanitation and argument/missing-file contracts. All three
registrations have matching lightweight test stub declarations.

## Installer and e2e

Package 121799290097 verifies installed oatdump/framework image, actual
installed app/window/input and device/data/association/toast/Start-menu
uninstall cleanup. Both e2e logs (121800356572/121800356301) pass all nine
apps, required Puzzles/TIC-80, association/shortcut launch and four transport
fixtures; both end with 0 unexpected failures. Review logs are under
C:\work\gesher-ci-review\native3-40386670586-*.log.

## Remaining scope

Remaining native3 work continues on its existing branch/brief: full NDK
coverage, native-only font bootstrap, binary128/complex libm, wide widths/
collation/other ABI contracts, record locks, inotify, proc metadata, file
mremap, epoll/poll, JVMTI and linker namespaces. No aggregate NDK/POSIX item
is closed by this stage. Media/surface/network/arm64 work stays with its
assigned tracks. Any head above the verified SHA must differ only in docs.
