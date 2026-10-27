# 0064: Native media format and bounded OpenSL contracts

Status: accepted (format ci40459815375; bounded OpenSL ci40476766790)

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

## Accepted follow-up: OpenSL identifiers and capability contracts

Reuse pinned OpenSLES_IID.cpp's53 real16-byte UUID pointer objects (correct
the existing19 and add34 old missing symbols), including full source notice.
Compare copied UUID values rather than pointer identity. Use the exact typed
engine/global query signatures and supported class subsets; validate output,
index/object/null and required/optional arrays, reset failed outputs, and
expose only implicit/requested interfaces with pinned prerealize behavior.
Unsupported effects/3D/device identifiers retain explicit unsupported results.
Use actual0x0B recording denial with no object. The production manifest gate
is unchanged; bare-core ART without a framework media identity also denies.
Normalize the volume argument's low16 bits explicitly at the host boundary:
the ARM guest does not promise incoming short sign extension. Actual current
CI records raw64936→signed16 -600, with set/get/value success at -600/-32768/
-1/0. No register contents are inferred for the previous failing source.

Full ci **40476766790**, attempt1 at **d10bea3a98bb8dfe76972209b6aff5df4fed58ec**,
contains accepted integration db84749b/freshExit30deb08c and passes22 jobs/
six skipped. Windows122099872570/122099872556 finish0 failures, with14 test76
PASS each: ten ABI/context combinations plus four device shutdowns. All50
semantic rows and40 volume result rows per runner were read, including real
manifest-allowed/denied single/device APKs and bare-core denial. Actual
package122099872538 passes23 checks and e2e122100699777/122100699799 each
passes ten apps/four transports/association/shortcut with0 unexpected
failures. Exactly34 new IID inventory verification objects preserve all
historical fields and143 prior format proofs.136 old media entries remain.
No effect/3D method or full OpenSL engine is established by those constants.

The earlier full40470959941/2b343aa7 remains RED: both runners exactly six
test76 failures from the core fixture's incorrect grant assumption and the
ARM signed16 boundary. Corrected source was verified above without retrying
that unchanged candidate. The same green run also verifies the stronger
lossless AAudio fixture: requested/effective44100-frame capacity, original
750/1250Hz/44100-frame gate, zero lost/xruns and a controlled small-ring loss
versus configured-ring zero-loss case. Old1153.3Hz cause remains unproved.

## Proposed follow-up: standalone backend and remaining contracts

Separate native source e5693412/full40477536464 factors pure audio/player/
NDK OBJECT targets into both hosts, shares the existing guest-thread TLS
entry guard, preserves the actual manifest provider and no-identity denial,
and prepares real NDK ELF queue/TLS callback tests. Its runtime/full CI proof
is pending, and it is not part of accepted source d10. No placeholder native
backend is accepted. Native-child recording identity, configuration,
loss-of-control callbacks, output-mix and full effects/device-policy behavior
remain open. The installer notice proposal also needs package-CI proof
before release; no installed-notice claim follows from source preservation.

## Proposed follow-up: AAudio MMAP capability and policy

Requested process policy and effective backend capability are separate.
Keep the API override initially UNSPECIFIED(0), accept exactly0/1/2/3 with
invalid values preserving the previous selection, and store actual stream
identity independently. The current shared Windows engine has no Android
MMAP service. NEVER/AUTO/UNSPECIFIED can open actual legacy tracks/capture;
ALWAYS must return Android16's NO_SERVICE(-881), nulling failed outputs,
after the format-error check. Existing stream identity and I/O survive later
global policy changes. This does not implement hardware MMAP or every builder
validation/policy option.

Separate source wip/media4-aaudio prepares typed dynamic API36 calls (the
CI r27 NDK predates these declarations), real output/read/close and independent
manifest grant/denial through both ABIs/core/single/device APKs in test78.
All three historical coverage entries remain unverified. Standalone ELF
must use the genuine backend continuation only after its full CI passes;
no library is advertised by policy-only exports. New platform-policy getters,
full builder validation, error callbacks and routing remain open.
