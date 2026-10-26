# Track brief: native3

Follow-up to native2, verified in ci 40296960603 at d380a3e (both runners,
all jobs, package and e2e). Branch wip/native3, worktree
C:\work\gesher-native3, cut from integration after native2 merges.
ADR 0062 is reserved. Read track-rules.md and the current status sections
of native-libs.md, posix.md, arm64.md, plus ADRs 0011, 0014 and 0057 first.

## Scope

- Close public NDK API gaps reported by native2's `ndk:*` coverage lines,
  grouping the full missing-symbol lists by implementation owner. The
  real-app import gate passes; it covers a smaller set. Do not equate it
  with complete NDK API coverage or add dummy exports to satisfy counts.
- Platform libraries absent in the verified dlopen test: libjnigraphics.so,
  libmediandk.so, libcamera2ndk.so, libvulkan.so. Provide working API/ABI
  bridges and handles, by soname and system path. Existing AndroidBitmap
  implementations should be reachable through libjnigraphics as well.
- Own libc/libm/libdl/libz API and ABI gaps: the API lists include wide
  locale-suffixed functions, resolver APIs, stdio extensions, binary128
  long-double functions and zlib stream/header/reset APIs. Include the old
  LLP64 value/layout audit (ADR 0011); passing symbol lookup is insufficient.
  Windows ICU's Unicode data version can differ from AOSP ICU 76: establish
  matching wide-character behavior at the pinned baseline, including new
  characters and the fallback on supported Windows versions.
- Implement fcntl byte-range record locks (currently always granted),
  with conflict, blocking, unlock, close and cross-process tests; preserve
  flock's independent open-file-description behavior.
- Finish inotify: other processes' writes before close; IN_OPEN/IN_ACCESS/
  IN_CLOSE_NOWRITE and moved watched directories. Audit the remaining
  POSIX limits in posix.md: signal() dispositions across processes,
  /proc/<pid>, mapping names/inode/device metadata and permission behavior.
  Route locale/time-zone limits to their locale owner with an explicit brief.
- mremap currently moves anonymous regions by copying and rejects file views.
  Close the file-backed/shared-mapping limit without losing shared writes.

Coordinate shared files through integration. Graphics/surface/HardwareBuffer
and font APIs overlap surfaces/graphics; media APIs overlap media3; network
resolver/network-binding APIs overlap web3/net; process APIs overlap exec3.
Assign each reported gap explicitly and keep this brief's ownership list
current. Full Vulkan support needs its own implementation/verification work;
an absent-library fallback is not its completion.

## Verification

Extend NDK-native tests for both x86_64 and arm64, run in dalvikvm and APKs
(single process and device), plus real library self-tests where relevant.
Verify error paths, data layouts, callbacks, ownership and cross-process
semantics. Keep native2, 21-posix, 40-native-libs, 42/43-exec and 47-arm64
green on both runners; package and e2e too. CI only for AOSP. Before final
CI merge current integration; tick only CI-verified roadmap items with run ids.

## Status and next steps

Running: ci 40305973210 at e94f760, including integration 91c195d. Read
the bitmap build/link results and all ABI/mode checks, both zero-failure
Windows summaries, package and e2e before claiming this implementation.

First implementation (not CI-verified): libjnigraphics' six bitmap entry
points over hwui's real ABitmap storage, with System V entries/JNIEnv
conversion and a compression callback trampoline. Builds AOSP's existing
apex/android_bitmap.cpp; patch 0620 handles the host's software-only Bitmap
getHardwareBuffer path. Tests extend native2app in both modes/ABIs and add
46-native3's framework dalvikvm Bitmap tests (direct pixel aliasing while
locked, PNG callback/decode round trip, data space and error paths).
Java test sources compile locally with JDK 21 against the saved framework
classes; PowerShell parses; the patch applies to the pinned source. No
AOSP build was run locally; all C++/NDK linking and runtime checks await CI.

The complete ownership inventory is still outstanding. Read ci 40296960603's
`INFO native2 ... ndk:*` lists (jobs 121487355022 and 121487354891), record
each remaining symbol's implementation owner, then continue the common-shim
and filesystem items above. Review logs are available locally in
C:\work\gesher-ci-review; no artifacts were downloaded.
