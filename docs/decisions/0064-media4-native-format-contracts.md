# 0064: Native media format contracts over the pinned AOSP backend

Status: proposed (media4; first source candidate, CI pending)

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

Compilation and every native/JVM fixture run happen only in CI. The first
source candidate has only source/hash, PowerShell/XML/YAML parser and shell
syntax checks locally; no runtime acceptance or new roadmap tick follows.
All test71 gates, existing native2 availability checks, both complete
Windows suites, package and required e2e must pass before acceptance.

The 313-symbol historical baseline is retained. Subsequent exact run/ABI/
semantic observations will describe the implemented format subset; the
remaining NDK codec/extractor/muxer/image/DRM APIs, OpenSL/AAudio contracts,
physical capture/routes, recorder/session/container validation and hardware/
protected-media work remain explicitly open in the Media4 brief.
