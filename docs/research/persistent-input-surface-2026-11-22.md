# Persistent codec/recorder input Surface

## Scope and starting point

Issue [#56](https://github.com/gesher-runtime/gesher/issues/56), **M04 persistent
input and M10 recorder persistent-input leaf**. Baseline:
`b915b0c46599e360bf345e85a033c6fe87d8558b`.

At that source the three persistent MediaCodec native methods and the recorder
setter have no implemented registrations. The generic stand-ins cannot create a
usable persistent producer. Each ordinary codec/recorder input owns a QueueSurface
and abandons it on retirement. `Codec::OnSurfaceFrame` also allocates and converts
an entire I420 frame before discovering that the codec has not started or has
already accepted input EOS.

This change implements the requested persistent producer, exclusive attachment,
successive consumers, lifecycle cleanup and JNI ownership. It also removes
discarded codec conversion work. It does not implement LinearBlock, legacy camera
recording, next-output-file rollover, hardware routing, protected video or general
zero-copy GPU encoding. Those remaining M04/M10 items stay open. Existing encoder
selection, supplied pixels and timestamps remain on their real paths.

The queue audit at scope selection found no competing persistent-input implementation in the
51 open PR bodies. Relevant ongoing media work includes #232, #243, #252, #264 and
#268; none owns this leaf. #278 concerns automerge retries. The independent scaling
PR #280 overlaps the codec JNI translation unit but no persistent state or input
implementation; composition evidence is recorded separately below.

## Android contract evidence

Primary sources are pinned to **Android 16, `android-16.0.0_r4`**:

| Source | Contract used |
| --- | --- |
| [MediaCodec.java](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/media/java/android/media/MediaCodec.java) | Persistent input is reusable by successive codec/recorder instances, at most one concurrently. `setInputSurface` is after configure/before start and in lieu of `createInputSurface`. Java rejects a nonpersistent argument. The application releases the Surface. |
| [MediaCodec JNI](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/media/jni/android_media_MediaCodec.cpp) | Persistent object ownership is acquired under `Surface.mLock`; release clears the field under that monitor. Receiver validation precedes persistent-source lookup. A released token fails with BAD_VALUE. |
| [MediaCodec core](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/libstagefright/MediaCodec.cpp), [CCodec](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/codec2/sfplugin/CCodec.cpp), [CCodecBufferChannel](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/codec2/sfplugin/CCodecBufferChannel.cpp) | The common create/set path checks CONFIGURED, without rejecting an already selected input. CCodec builds the requested wrapper and the channel replaces its selected wrapper before connecting it. Repeated same-source selection and replacement before start are permitted. |
| [GraphicBufferSource](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/module/bqhelper/flagged_files/GraphicBufferSource.inc), [deprecated migration variant](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/module/bqhelper/flagged_files/DeprecatedGraphicBufferSource.inc) | Both backend configure implementations assign the supplied component and restore nonexecuting state; neither has an already-configured-source guard. This completes the source trace beneath the shared codec state check. |
| [MediaRecorder.java](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/media/java/android/media/MediaRecorder.java) | Setter precedes prepare; calling it makes `getSurface` unavailable. Frames before start are discarded. |
| [native MediaRecorder](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/libmedia/mediarecorder.cpp), [StagefrightRecorder](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/libmediaplayerservice/StagefrightRecorder.cpp) | Prepared/recording states reject setter calls. The setter stores the native source; encoder creation during preparation uses it. Reset clears that source. |
| [Surface.java](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/view/Surface.java) | `transferFrom` moves the native reference under the two object monitors. Finalization invokes virtual `release`, allowing PersistentSurface to retire its native token. |

The locally read source SHA256 values are:

```text
MediaCodec.java                c10ff4e847db3f25ec7a68741669b7478943c79edf9798d08a240c45e6666e93
android_media_MediaCodec.cpp    82c596794f38b7ae6fab9f993a9a67e459ca2885130c877d97d137b0bb797e2d
MediaCodec.cpp                 dc186797a1d7aa835e330a5193455a711e5317f1db11bcefaf03b90846a39aeb
CCodec.cpp                     ab3ebfc55f5f25fffe5aa5270135e1d746d3030b2c86a697a1c78a16d2142a8a
CCodecBufferChannel.cpp        cb4c0902b48dce99b68821b669d5516f78bccc0168f042132257fe0dc96b3990
GraphicBufferSource.inc        3e48adabc6b033964b96824cc0d8c69310b6e102cc207dac810b6079cba7ff10
DeprecatedGraphicBufferSource.inc e6e8ec4344b7795078834332729e0f478baee1936f31ee45b2e4283825fa5bfe
MediaRecorder.java             ce9d4bfe82a8e5761a3f5e5faee21cfc331f6931dcbb1d7970be3c175dca4540
mediarecorder.cpp              228f9fad1bb77a34df24601c29cc9e88d0c117bbd6936f62d83b19ba5862f04a
StagefrightRecorder.cpp        40d30f9622c88673a962735caeaf3b1af7e4bd2571d6a92f8f7f4edef42a9eb2
Surface.java                   02e4aafa5f97055394607aa03893881dd388c3a77030fc8d9092dc16b0dc73b1
```

The public codec documentation limits returning to Flushed state to decoders;
encoder flush is not advertised here as a newly supported operation. The
implementation nevertheless rejects a frame whose execution or flush generation
changes during conversion, so existing internal transitions cannot publish old
conversion work into a successor.

Released-before-set recorder input is a bounded invalid-handle choice: this
implementation rejects a cleared persistent token with IllegalArgumentException.
MediaCodec's native reference explicitly defines BAD_VALUE for it. The recorder
documentation defines a persistent input producer but does not specify
release-before-set; Stagefright's internal setter accepts a null native source.
No claim is made that this null-source side effect is reproduced. A recorder
input must still refer to the producer the application chose: the public
[Surface.release contract](https://developer.android.com/reference/android/view/Surface#release()),
also present in the pinned `Surface.java`, says release invalidates that Surface.
The invalid-handle rejection is a declared bounded lifecycle distinction, not an
Android runtime differential pass. A recorder
that acquired a native reference **before** the Java Surface is released retains
that reference through preparation, matching native ownership semantics.

## Ownership and concurrency

The persistent object owns one shared input queue. Its Java `mPersistentObject`
contains an opaque, nonzero registry token rather than an unprotected native
pointer. Tokens are never reused, including at integer exhaustion. Lookup copies
a shared native reference while holding the Java monitor and registry mutex;
release clears the Java field and removes that registry owner under the same
locks. Native destruction occurs after both locks are released.

The factory obtains the normal real Java Surface wrapper, constructs the
framework's `MediaCodec.PersistentSurface`, and uses `Surface.transferFrom` to
move the native producer reference. It does not duplicate native strong ownership
or substitute a test Surface in production. Failures before or after that transfer
close both possible wrappers while preserving the original Java exception.

Each consumer receives a unique generation lease. Admission, callback invocation,
callback replacement and lease retirement share the existing recursive callback
mutex. A retiring lease waits for another thread's admitted callback before the
consumer can be destroyed. Reentrant retirement/replacement is allowed, and the
local callback copy keeps the invocation's captures alive. No second owner mutex
is held across callback quiescence.

The QueueSurface listener also captures the lease generation. A producer that
copied an old listener before detach cannot deliver its old publication to a new
consumer. A retired lease cannot clear or replace a successor's callback.
Detachment clears its listener without abandoning the persistent queue. Ordinary
input Surface retirement still abandons its producer. The final shared input
queue owner abandons the queue and releases its remaining resources.

Codec attachment is serialized with lifecycle operations. Selecting the same
persistent producer again while configured keeps the current lease. A different
source is acquired and its callback installed before the old lease is quiesced;
a conflicting source leaves the application's prior producer intact. An ordinary
input can be replaced by a persistent input, and configured `createInputSurface`
can replace a persistent input with a new ordinary producer. All source locks,
callback installation, old-callback quiescence and retired-owner destruction occur
outside the codec mutex. In particular, two codecs attempting to exchange busy
sources cannot hold their own codec locks while waiting on the other callbacks.

Stop/reset/release use that same separation, including destroying the retired
lease only after releasing the codec mutex. A successor can attach immediately
after detachment, before the old lease object is destroyed. The callback
snapshots dimensions and execution/flush
generations under the codec lock before conversion, then checks admission again
before publication. The existing eight-frame queue bound is unchanged.

The recorder stores a native source before prepare. Prepare acquires the lease;
a conflict fails preparation, releases its incomplete pipeline and enters the
existing recorder error state. Start failure, worker completion, stop, reset and
release use existing pipeline cleanup, which now retires only this lease.
`getSurface` stays unavailable while persistent input is configured. Reset also
releases a pending, never-prepared source reference.

A Surface parcel carries the producer through the existing Binder forwarding
path. It does not transfer MediaCodec's private persistent consumer token. The
real APK therefore keeps the encoder/recorder in its owning process and transfers
the producer Surface to a private EGL service. That one native producer connection
remains live across all three consumer sessions.

## Work and resource measurements

`tools/test_codec_input_surfaces.py` compiles the exact selected codec methods,
the production `VideoFrame::Allocate` and actual RGBA-to-I420 conversion. A global
allocation observer records actual allocation requests inside the callback; a
declared wrapper counts calls into the unchanged conversion body. Each row feeds
30 real callback inputs with a configured output size, independently before start
and after input EOS. The baseline is the immutable starting commit above.

| Configured image | Baseline allocations / bytes for 30 rejected inputs | Baseline conversion calls / output pixels | Candidate allocations / bytes / conversions |
| --- | ---: | ---: | ---: |
| 1280×720 | 60 / 41,474,400 | 30 / 27,648,000 | 0 / 0 / 0 |
| 1920×1080 | 60 / 93,314,400 | 30 / 62,208,000 | 0 / 0 / 0 |
| 3840×2160 | 60 / 373,250,400 | 30 / 248,832,000 | 0 / 0 / 0 |

Both rejection states produce these counts. Bytes include the actual frame and
shared-owner allocation, not just the plane-size estimate. This is removed
native input work, not a whole-application speedup, GPU throughput claim or
resident-memory measurement. The producer's existing RGBA publication still
runs. Accepted frames still perform the required real conversion.

One thousand successive complete production InputSurface leases preserve the
same producer identity, and 200 checked-JVM attachment cycles create only one
producer. These Linux runs declare the QueueSurface/native-window adapter; the
same native identity/retirement assertions are built against actual QueueSurface
and hostgraphics in the Windows executable. No Windows allocator result is
inferred from the Linux adapter.

## Validation performed locally

| Gate | Observed result and boundary |
| --- | --- |
| `test_input_surface_callbacks.py --sanitize` | **5,087 checks, zero failures.** Complete production InputSurface; explicit host queue/JNI boundary. Includes exclusivity, resize/pixels/timestamps, 1,000 successive leases, stale listeners, cross-thread quiescence, reentrant replacement, source release and final cleanup. |
| Same callback gate, `--source-ref b915b0c4...` | **55 original ordinary-input checks, zero failures.** The missing persistent interface is not fabricated on the baseline. |
| `test_codec_input_surfaces.py --sanitize` | **82 checks, zero failures.** Exact codec setters, admission, real allocation/conversion, odd-size planes, timestamps, queue bound and controlled generation races. Includes same-source reuse, ordinary/persistent replacements, failure preservation, callback installation and retirement outside the codec lock, and reciprocal two-codec replacement with both source callbacks held. Observer adapters delegate to the complete production lease implementation. |
| Codec input gate against baseline | **45 baseline/control checks, zero failures.** Records the discarded allocation/conversion work and the baseline generation behavior; it does not claim persistent API support there. |
| `test_persistent_input_jni.py` | **1,227 checked-JVM assertions pass.** Complete JNI registry and InputSurface, actual codec factory/release bodies, explicit Java/native Surface adapter. Includes forged/stale tokens, 100 contended lookup/release pairs, factory failures before/after native transfer, callback quiescence, repeated release and GC finalization. No CheckJNI warnings; no live producer/wrapper owners at exit. |
| `test_recorder_persistent_input.py --sanitize` | **81 checks pass.** Complete Recorder, InputSurface and MP4 writer with declared queue/capture/encoder boundaries. Pre-start and paused frames are rejected, conflicting prepare and real descriptor failure clean up, stop/reset preserve the source, pending ownership releases correctly. FFmpeg independently demuxes and decodes both three-frame MP4 files. Encoder packets are an explicit pre-encoded fixture. |
| Existing codec lifecycle sanitizer gate | **15 receiver + 98 wait checks pass.** The additional destructor barrier observes that exact `StopLocked` destroys its retired lease after releasing the codec mutex. |
| Existing complete Recorder state/JNI gate | **184 checks pass** on the checked JVM. |
| Existing receiver lifetime gate | **50 checks pass** across six receiver, concurrent-release and reentry cases. |
| Existing Recorder/MP4 lifecycle gate | Passes; duration, size and mixed-source files independently demux and decode. |
| Complete recordapp Java/D8 | Compiles separately against the pinned public Android 36 SDK and the project's real Android 16 framework/libcore. Both produce **80,716-byte DEX**. Public SDK compilation confirms the new caller uses public APIs. |
| Windows harness syntax | Actual PowerShell 7.4.22 parses track 59. Both exact new LineCheck invocations bind five required patterns, failure marker, exit code and separate tags correctly. |

Local native sanitizer runs set `ASAN_OPTIONS=detect_leaks=0` only because this
executor's ptrace environment prevents LeakSanitizer. CI does not disable it.
The host JVM gate uses UBSan and `-Xcheck:jni`.

As a behavioral control, the revised codec fixture also compiled the initial
implementation commit `668533d5fe7255c3a6616a62e9fe497e5cdb7197`. It rejected the
same-source selection, ordinary-to-persistent replacement and distinct persistent
replacement checks, then failed the bounded replacement barrier because its
blanket existing-input guard never admitted that operation. These were executed
behavior failures, not a compilation failure. The corrected source passes all
82 checks with the same fixture.

Representative commands, from the repository root:

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_input_surface_callbacks.py --sanitize
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_codec_input_surfaces.py --sanitize
python3 tools/test_codec_input_surfaces.py --source-ref b915b0c46599e360bf345e85a033c6fe87d8558b
python3 tools/test_persistent_input_jni.py --java-home "$JDK21"
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_recorder_persistent_input.py --sanitize
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_codec_lifecycle_contracts.py --sanitize
python3 tools/test_media_recorder_state.py --java-home "$JDK21"
python3 tools/test_media_receiver_lifetimes.py --java-home "$JDK21"
python3 tools/test_media_recorder_lifecycle.py
```

## Required Windows acceptance and limits

`media_input_surface_callback_test` builds the same native lease tests against
the actual Windows QueueSurface/hostgraphics implementation. Track 59 additionally
requires the real public `PersistentInputActivity` from recordapp. Missing APK,
nonzero process exit, failure marker, missing phase evidence or missing final
success marker fail the gate.

The APK keeps one real EGL context/window connected while a first AVC codec,
MediaRecorder and a second AVC codec take successive leases. It checks same-source
selection and persistent/ordinary replacement before start, failed busy-source
replacement preserving the chosen producer, exclusivity, wrong type/state/released
handles, codec reset, recorder prepared reset, getSurface
rejection, pause/resume, pre-start discard and EOS. Three actual MP4 files are
demuxed and decoded; **all 12 expected colors, frame counts, timestamp order and
decoder EOS** must match. No pre-encoded packet fixture is used in the APK.

The second run transfers the real Surface to a separate private producer process
through Binder. The APK requires different producer/consumer process IDs and an
unchanged producer PID for every subsequent render. The harness reads only bytes
appended after this launch, preserving earlier diagnostics and rejecting log
truncation.

These Windows native/ART/EGL/Binder/encoder runs are wired requirements, **not
locally executed results**. Physical camera/microphone capture, Media Foundation
hardware and complete M04/M10 closure are not claimed. The new producer path is
the ordinary real rendering path; synthetic sources stay in declared test-only
fixtures.

## Composition with scaling PR #280

The persistent-input implementation has no dependency on the scaling PR. Both
touch codec JNI and CI registration; the ordinary output scaling path is unchanged
by this input work. An isolated worktree combined persistent implementation
`f1481f1dad0f0e80a67580c20cd1bc50b5aed47b`, refreshed main
`55afec85989c4bf7cf404d486dc7bed6e46e1409` and the exact published scaling head
`27a777ba2b452a61fc4094baf7f55ebc9f3e96fa`. All merges were automatic, without
manual conflict resolutions. The tested union is
`a9408f5dc0472c2b21f952146c3687e47f10fe89`, tree
`4230db49fd6066bc70f733b9239dbe0add758614`.

The 27 disjoint persistent-input files and 18 disjoint scaling files remain
byte-identical to their respective heads. The shared files are codec JNI, the
two existing codec host templates, CI registration and `docs/media.md`.
`CreateInputSurface`, `SetInputSurface` and `OnSurfaceFrame` remain exact matches
to the persistent-input implementation. `StopLocked` differs only by retaining
scaling's existing `video_scaling_mode_ = 1` reset; input destruction remains
outside the codec mutex. All four new host CI gates are present exactly once.
The union codec source SHA256 is
`b94dec2b65a5142acb617b0ce304a806715d3845b239d7a686c510e35d39e540`.

On this exact union, ASan/UBSan executions pass **82 input contracts**, **25
timestamp and 222 window ownership/scaling contracts**, and **42,157 scaling
contracts** including the pinned Android crop comparison. The input gate still
observes zero inactive frame allocations and conversions at all three measured
resolutions. This proves local source composition; it does not substitute for
the required Windows runtime acceptance.
