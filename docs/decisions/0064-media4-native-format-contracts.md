# 0064: Native media format contracts over the pinned AOSP backend

Status: accepted (media4 format slice; full ci 40459815375)

## Context

Media3's Java encoder/extractor/muxer round trips use the actual pinned
AMediaFormat internally, but expose none of its NDK ELF symbols. The
preserved native API baseline has 21 format functions and 122 format data
symbols among libmediandk.so's 276 missing entries. The Android 16 source
contains 152 format data definitions; an older NDK export list does not
describe the entire pinned source API.

These are C functions with opaque pointers, fixed-width integers and LP64
size_t. The host backend uses Windows' calling convention, while an app's
x86_64 ELF library uses System V and an arm64 library reaches it through
the AAPCS64/System V translator. A data symbol such as
AMEDIAFORMAT_KEY_MIME is a pointer object: its exported address must be the
address of that object, not the first byte of its string literal.

## Decision

- Reuse the actual android-16.0.0_r4 NdkMediaFormat.cpp already compiled in
  stagefright_foundation. Add System V wrapper entries for all 21 functions
  without duplicating format storage, coercing types or inventing results.
- Register all 152 actual AOSP pointer data objects. The production name
  list and independent expected-value test list come from the pinned source
  (SHA256 caf6099f6cda1b50724987ba8fdf83b7ed81d25027bdaa209e98416d134cf508).
  Each value remains ordinary format metadata; a DRM or codec-related key
  neither supplies that feature nor advertises its availability.
- Keep the bridge in a small OBJECT target linked into both dalvikvm and
  gesher-exec. Native ELF processes receive the same C API without pulling
  the Java/JNI media player into the launcher.
- Probe libmediandk availability with the real AMediaFormat_new entry. The
  formerly absent whole-library probe used an unimplemented codec entry;
  codec/extractor/muxer/image/DRM entries remain absent until implemented.
  No generic decoder or success stub is added.
- Verify the C contract with the same NDK-built source as a JNI library and
  an independent ELF executable, for x86_64 and arm64-v8a. Run through the
  core ART boot path, standalone gesher-exec and a single-ABI APK in single
  process/device modes on both Windows runners. Print native ABI/caller
  markers and use fresh device roots so prior app output cannot satisfy a
  different ABI's check.
- Test actual typed calls, dynamic calls, data relocation/indirection and
  pinned values; UTF-8/string/buffer ownership and copy/delete lifetime;
  clear/reuse, debug-string lifetime and pinned missing/type/null/error
  results. Never dereference outputs after their documented lifetime or
  exercise undefined null-output preconditions as if they were required.

## Consequences

Compilation and every native/JVM fixture run happen only in CI. Full ci
**40459815375**, attempt 1 at **c5962291855b13ebd0d21fcf72981db973ad9e72**,
passes all 28 jobs (22 success, six skipped). Windows jobs 122048294283
(2022) and 122048294275 (2025) each finish with `0 test(s) failed`. Each
runner proves both ABIs in all four contexts, all eight semantic parts per
context and both device shutdowns: ten test71 PASS lines. Package
122048294220 passes all 23 checks; e2e 122048949119 / 122048949121 each pass
all ten apps and report `0 unexpected failures`. Actual job logs were read.

The original full ci 40456299720 at 92e914238 remains RED: main build
122030308024 reports 12 unknown int32_t/int64_t type errors in the AOSP
header, before Windows/package/e2e run. The accepted source explicitly
includes cstdint before that header and compiles the exact bridge object
early in CI. Build 122041490613 proves that compile, both host links and
203 assembly boundaries/zero problems. Only source/hash and lightweight
script/parser checks run locally.

The 313-symbol historical baseline is retained. Exact per-entry subsequent
verification covers 143 old entries: 21 functions and 122 data objects.
Another 30 pinned data objects are tested without adding them to that
historical list. Both native2 ABI logs now report 133 missing libmediandk
entries of 276 checked; 170 old media entries remain (133 libmediandk,
34 OpenSL and three AAudio). This verification covers ordinary metadata,
not the codec/DRM feature named by any key. The
remaining NDK codec/extractor/muxer/image/DRM APIs, OpenSL/AAudio contracts,
physical capture/routes, recorder/session/container validation and hardware/
protected-media work remain explicitly open in the Media4 brief.

This source predates the coordinator's separately verified Java exit-status
freshness fix. Combined integration CI must verify that prerequisite with
this slice before the coordinator accepts the integrated tree.

## Proposed follow-up: OpenSL contracts

The active Media4 source adds canonical OpenSL UUID pointer objects from
pinned OpenSLES_IID.cpp, copied-value lookup and typed engine queries. It
validates required/optional interface arrays, exposes only requested or
implicit interfaces, permits the pinned prerealize cases, and uses the
actual 0x0B native recording-permission error. Test76 prepares real NDK JNI
callers and manifest-allowed/denied single-ABI APKs for both ABIs/runners.
This follow-up is not accepted by the format run above; its own full CI and
exact semantic logs are required before inventory/roadmap acceptance.

The standalone OpenSL backend still depends on player/video/window/JNI
factoring and remains open; no placeholder is linked into gesher-exec.
Canonical effect/3D identifiers support exact unsupported results, not
effects or 3D audio. Full configuration, loss-of-control, output-mix and
device-policy behavior remain separate Media4 contracts.

The separate native-launcher continuation proposes factoring pure audio,
player/video and NDK targets into both hosts, with actual hostgraphics/window
dependencies and no JNI player in gesher-exec. Its recording gate denies a
native process without an identity provider; ART installs the unchanged
Java manifest decision. The JNI and native callback paths share the actual
once-per-thread ELF TLS initializer in bionic. Both-ABI standalone callers
and real callback/TLS checks are prepared; no compilation or runtime proof
is claimed before its separate CI. App-child recording identity propagation
still needs a service-authorized contract and remains explicitly open.
