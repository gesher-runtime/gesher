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
  locale-suffixed functions, stdio extensions, binary128
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
- ADR 0009 epoll/poll limits: edge-trigger behavior, nested epolls, readiness
  when polling an epoll fd, concurrent waiters and pipe polling. Coordinate
  socket/legacy resolver gaps with net4 (ADR 0070); binder readiness belongs
  to binder2. The 84 remaining libc network/resolver entries are assigned
  to net4 in the baseline inventory, not implemented concurrently here.
- ADR 0017: System V JVMTI environment/callback tables for ELF agents and
  linker namespace isolation. Verify actual agents and isolated dependency
  lookup; a JNI-only library test does not cover either contract.

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

Not started. First read ci 40296960603's complete `INFO native2 ... ndk:*`
lists (jobs 121487355022 and 121487354891), write an ownership inventory,
then implement the common-shim and filesystem items above. Review logs are
available locally in C:\work\gesher-ci-review; no artifacts were downloaded.
