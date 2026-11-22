# MediaRecorder MP4 rollover without recreating its sources

Date: 2026-11-22. Issue: [#56](https://github.com/gesher-runtime/gesher/issues/56),
bounded **M10 `setNextOutputFile` / size-limit rollover** leaf.
Comparison base: `972c2b42b40c4ac799e147f5e352d07ba615d0fb`.

## Scope and observable behavior

The framework already exposes `MediaRecorder.setNextOutputFile(FileDescriptor)`
and its `File` overload. The native `_setNextOutputFile` method was absent from
Gesher's real method table, and a recorder always terminated when it reached its
configured file-size limit. Applications could not continue a recording into
their chosen next file.

The method now admits one pending, owned duplicate for **MPEG_4 only**. The app
can close its descriptor immediately. A second pending choice is rejected; it
does not replace the first choice. At the limit, the current MP4 is finalized
and a new MP4 starts with the existing track definitions and actual codec
configuration. Capture, audio/video encoders, their running timelines, and the
chosen input Surface stay in the same recording session.

The crossing encoded packet is discarded, as in Android's MPEG4Writer. A new
video segment discards dependent packets until an actual sync packet arrives;
the existing encoder receives a sync-frame request. No packet is relabeled as
a key frame. The container restarts its local presentation timeline while
retaining the relative timing of its actual accepted tracks. Like the pinned
writer, a new file resets its duration origin to its first accepted media;
paused time is excluded. Existing first-file/no-rollover duration behavior is
unchanged, and skipped initial dependent packets do not start the new clock.

The info sequence for a continuing three-file recording is 802, 803, 802, 803,
802, 801. The existing size-estimation rule can skip an approaching notification
if one unusually large packet crosses the whole notification interval. Each
802/803 post is ordered across both workers, outside the writer and state locks.
803 follows a successful new writer start. An old-file finalization or new-file
start failure reports an error, closes both owned descriptors and does not
fabricate 803. Without a next file, the existing 801 completion remains.

No microphone/camera substitution, invented encoder capability, input mutation,
permission bypass or package-specific behavior is introduced. The public Java
gate renders its own explicit test pixels through an actual EGL window and uses
the real recorder/encoder/MP4/extractor/decoder path. Host capture and encoded
packet adapters are separately declared test fixtures.

This does **not** close M10 as a whole, M04, M09, general recorder/codec parity,
or physical-device qualification. It does not add rollover to THREE_GPP, ADTS,
Ogg or WebM. Existing non-MP4 size-limit handling has no new descriptor wait.

## Pinned Android contract

All source links below use `android-16.0.0_r4`:

| Primary source | Relevant behavior |
| --- | --- |
| [MediaRecorder.java](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/media/java/android/media/MediaRecorder.java) | Next descriptors must be writable/seekable; caller ownership and immediate close are explicit. Normal use is after 802 and before 801. The previous pending file must be used before another can be supplied. The `File` overload opens and closes its own descriptor. |
| [android_media_MediaRecorder.cpp](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/media/jni/android_media_MediaRecorder.cpp) | Null argument precedes receiver lookup. Released receiver and native INVALID_OPERATION map to IllegalStateException; other setter failure maps to IOException. The exact private native registration is `_setNextOutputFile (Ljava/io/FileDescriptor;)V`. |
| [mediarecorder.cpp](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/libmedia/mediarecorder.cpp) | A real read-only descriptor is rejected before the backend's output/writer-state check. Failure of this setter does not force the public recorder into ERROR. Its comment warns that invalid fd transport can skip the server call; `fcntl == -1` is logged without an immediate return. |
| [IMediaRecorder.cpp](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/libmedia/IMediaRecorder.cpp) | The Binder proxy writes the fd and reads the reply without checking transaction failure. The server duplicates the transported fd before calling the recorder. Consequently a raw Stagefright call with -1 is not evidence of the public invalid-fd behavior. |
| [StagefrightRecorder.cpp](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/libmediaplayerservice/StagefrightRecorder.cpp) | Supports only MPEG_4, requires an existing writer, and truncates the supplied file before calling `setNextFd`. Thus even a second-pending rejection follows truncation of the newly supplied file. |
| [MPEG4Writer.cpp](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/libstagefright/MPEG4Writer.cpp) | `setNextFd` holds one duplicate; `switchFd` waits up to 100 ms for delivery. The control handler finalizes the old writer without stopping its sources, initializes the next writer and emits 803 only after start succeeds. The track loop drops the crossing sample and skips non-sync video at a new segment's start. Track reset and first-sample normalization restart each file's duration clock. |

Verified source SHA256 values:

```text
MediaRecorder.java             ce9d4bfe82a8e5761a3f5e5faee21cfc331f6931dcbb1d7970be3c175dca4540
android_media_MediaRecorder.cpp 45c1b20485b530ea4ed8b457f7cf598bd0c969fa5bf8796a575dbf0930625216
mediarecorder.cpp               228f9fad1bb77a34df24601c29cc9e88d0c117bbd6936f62d83b19ba5862f04a
StagefrightRecorder.cpp         40d30f9622c88673a962735caeaf3b1af7e4bd2571d6a92f8f7f4edef42a9eb2
MPEG4Writer.cpp                 591539419f922db99c4dd93ba4320174418730327922e85799787627f92ca127
IMediaRecorder.cpp              5aebf10bc0a0d82d2f5129907cbe2503ffff7bc37825c13bc2b7f8f084c16688
```

### Deliberate limits of the comparison

The pinned Stagefright setter ignores `ftruncate`'s return value, and its writer
does not check `dup` failure before returning success. Gesher checks seekability,
truncation and duplication, returning the documented IOException failure while
leaving the recorder usable. An unseekable descriptor is outside the documented
input contract; descriptor exhaustion must not be reported as a successfully
retained output. Those negative cases are **not** described as differential
passes against an executed Android runtime.

An independent review identified the related invalid-fd/state-precedence edge.
The final implementation preserves the documented before-prepare state error
for an invalid descriptor, and rejects it with IOException when a prepared MP4
writer exists. Unsupported output format also remains an invalid state. A real
read-only descriptor keeps the native wrapper's earlier IOException precedence.
Null still produces IllegalArgumentException before receiver lookup, and a
released receiver with any nonnull descriptor produces IllegalStateException.
All six additional invalid/state/read-only combinations run through the actual
JNI namespace in the checked-JVM test; failures do not poison the recorder.

This invalid-fd matrix is a documented-contract decision, not an Android runtime
differential pass. The transport trace also checks pinned
[Parcel.cpp](https://android.googlesource.com/platform/frameworks/native/+/android-16.0.0_r4/libs/binder/Parcel.cpp)
(`1fa251f995b6ed3e0e670ed145268888ae4aa34a6c8e34958fdc294f10b511f1`)
and [IPCThreadState.cpp](https://android.googlesource.com/platform/frameworks/native/+/android-16.0.0_r4/libs/binder/IPCThreadState.cpp)
(`7d9bd8ce38d37d4e7a1f004ab2144fe69ff95799e0db24b7bdb5d0681af065a8`).
The former serializes an fd object and its value-returning read returns zero on
missing reply data; the latter records a failed transaction in the reply error.
The media proxy ignores that status. Gesher does not fabricate successful output
admission from an invalid descriptor or infer backend precedence across a failed
transport.

Gesher also rejects delivery after terminal worker cleanup has destroyed its
writer. The documented call window ends before the 801 notification. The pinned
raw writer may still retain an otherwise unused fd after its recording has
ended; that undocumented late-call behavior is not claimed as parity.

The existing complete-file size admission remains unchanged. It includes the
pending MP4 metadata and declines a sample which would reach/exceed the limit.
Android's writer uses its own estimates/margins and asynchronously selected
source packets. This change does not claim identical segment bytes or cut times
between different Android encoders/backends. The byte-identical comparison below
uses the same Gesher source packets and explicit boundaries in both modes.

## Ownership, concurrency and resource bounds

`Recorder::SetNextOutputFile` resolves the public receiver using its existing
operation lease, validates the descriptor and state, and publishes the duplicate
under `mux_mu_`. Worker finalization uses that same mutex to retire the current
writer and any unused pending descriptor. Stop/reset joins the workers before
clearing the session. A failed setter does not poison the existing session or
lose an already accepted pending file.

For size-limited MP4, each packet-writing operation takes `writer_event_mu_`
before `mux_mu_`. Unlimited and other-container recording keeps its existing
single writer-lock path, without the additional event lock.
Info posts release `mux_mu_`; they never hold `mu_`. The other worker cannot pass
that post, and a Java callback can acquire the writer mutex to supply its fd.
There is no added native queue of pending events; the framework's normal Handler
delivery retains its Android behavior. Only the two fixed recorder workers
participate in writer serialization. The 100 ms wait releases `mux_mu_` and can be woken by the fd setter
or stop/reset. All uses of both writer/state locks follow writer then state;
terminal cleanup releases the state lock before taking the writer lock.

`Mp4Muxer::SwitchFile` secures a duplicate of the pending fd before finalizing the
old file. Failure closes that temporary ownership. Successful finalization
closes the old descriptor, retains codec/track/container metadata, clears sample
table lengths/counters, resets per-file admission, and starts the successor.
The table vectors retain their capacity, so their retained storage is bounded by
the largest individual segment, not the number of completed segments. Existing
per-frame conversion/packet storage is unchanged.

The stable recorder ownership is one current fd plus at most one pending fd.
The switch has one additional temporary duplicate while securing the successor;
it closes before the 803 callback. Tests observe two owned fds after admission,
one during 803 before supplying another file, and the original fd count after
stop/reset/error. The caller's original fd remains its own.

## Actual production allocation comparison

`tools/test_mp4_rollover_work.py` compiles the complete production MP4 writer.
The recreation control uses the existing Create/AddTrack/config/Start/Stop
workflow for each file. The reuse mode calls the real new SwitchFile path.
Both write 32 segments of 2,048 actual AAC packets each, with matching timestamps,
orientation/location metadata and CSD. Instrumentation counts real C++
operator-new calls/requested bytes during writer operations, including every
finalization. File creation, input preparation and verification are outside the
counter in both modes.

| Source / mode | Allocation calls | Requested allocation bytes | Final output bytes |
| --- | ---: | ---: | ---: |
| Original `972c2b42`, recreate | 3,392 | 6,084,640 | 20,533,408 |
| Candidate, recreate control | 3,392 | 6,084,640 | 20,533,408 |
| Candidate, reuse | 1,470 | 3,517,840 | 20,533,408 |

Reuse removes **1,922 allocation calls (56.66%)** and **2,566,800 requested bytes
(42.18%)** in this workload. All 32 files are independently SHA256-compared,
byte-for-byte equal in all modes, and the final reused file is independently
decoded with FFmpeg. The common rolling output checksum is
`215088296887602789`; correctness does not rely solely on that checksum.

The old source has no next-output-file implementation; this measures the real
writer recreation work avoided by the new implementation, not a claimed old-API
success. These are cumulative allocations, not resident memory or whole-app
speed. Native host adapters separately show one capture/encoder over three audio
segments and one producer/encoder over three video segments.

## Validation completed locally

| Gate | Result and boundary |
| --- | --- |
| Complete Recorder + MP4 with ASan/UBSan | **1,617 checks passed**. Actual threads, file syscalls, writer output and callbacks; explicit host capture/codec/Surface adapters. |
| Independent decode and source checks | **15 finalized MP4 files decoded** without FFmpeg error output. All **93 AAC packets** in the three primary segments and two duration-regression files match their original packet bytes. **30 H.264 frames** retain pixels, orientation and genuine sync boundaries. Mixed audio/video outputs contain both tracks with ordered packet timestamps. |
| Real JNI namespace + complete Recorder, checked JVM/UBSan | **204 assertions passed**. Includes null/released/order/format/pending/read-only/invalid-fd behavior, nonfatal retry and reset. The host JDK uses fd instead of Android's descriptor field; that field name is the only production JNI text adaptation. |
| Existing receiver-lifetime checked-JVM gate | **50 checks passed**, covering retained operations, serialization, concurrent release and reentry. Its explicit Recorder observer includes the new method solely to compile the complete JNI unit. |
| Existing Recorder lifecycle gate | Passed, including duration/pause, size stop, capture restart/write failure, mixed tracks and independent MP4 decode. |
| Complete recordapp Java + D8 | All sources compile against the pinned public Android 36 SDK and separately against the project's Android 16 framework/libcore. The new caller uses public APIs. This is compilation, not APK execution. |
| Real PowerShell parser | Track 59 parses under 7.4.22; both new LineCheck calls bind exactly the required output/pattern/exit/tag/forbidden arguments. |

The two-worker test deliberately holds 802 while the other worker has already
encoded a packet batch. A linker observer sees that worker blocked at its actual
writer admission; 803 cannot overtake the held callback. Stop is also exercised
while that callback is held. The actual condition-variable boundary verifies
that a late next fd is admitted during the grace interval and that stop wakes
the wait rather than waiting for its timeout.

The duration regression uses real AAC packets to fill the first file in 100 ms,
then smaller real AAC packets after rollover. With a 120 ms duration limit, the
second file accepts six packets on its new timeline even though their source
times start at 120,000 us. An intervening 200 ms pause cannot complete the
recording. The output sequence is 802/803/800, with five and six independently
verified packets in the two files. This exercises `Track::resetInternal`,
first-sample timestamp normalization and per-file duration behavior traced in
the pinned MPEG4Writer, rather than accidentally retaining the first file's
absolute cutoff.

Failure cases include actual EMFILE during dup, nonseekable/read-only/invalid
descriptors, 100 stop/reset retirement cycles, start failure with a pending fd,
ENOSPC while finalizing the old moov, and ENOSPC while starting the successor.
The syscall failure wrappers are host-only; production sources are not rewritten
for those cases. Each failure is checked for ownership and absence of false 803.

The native video fixture intentionally supplies two real dependent packets after
a sync request, then a genuine IDR packet. The dependent packets are absent from
the successor file. The counts are 85 encoded/82 retained AAC packets, and 37
encoded/30 retained H.264 frames: three crossing packets are discarded in each
three-file run, with four additional dependent packets skipped for video.

Commands:

```sh
python3 tools/test_media_recorder_rollover.py --sanitize
python3 tools/test_media_recorder_state.py --java-home "$JAVA_HOME"
python3 tools/test_media_receiver_lifetimes.py --java-home "$JAVA_HOME"
python3 tools/test_media_recorder_lifecycle.py
python3 tools/test_mp4_rollover_work.py --baseline 972c2b42b40c4ac799e147f5e352d07ba615d0fb
```

For this ptrace-hosted Linux environment only, the ASan invocation used
`ASAN_OPTIONS=detect_leaks=0`. CI retains LeakSanitizer's default behavior.
The direct fd-count assertions remain enabled locally and in CI. The MP4/Recorder
sources compiled by the native gate have these SHA256 values:

```text
record/recorder.cc          21e37ce9227f3201c7c247e74de2f41145609b26c33a67232dbd8b8178c4208c
mux/mp4_muxer.cc           3b2b11badec4363208048f84d02035e79580ac564c3a8697281517a919691330
jni/media_recorder_jni.cc  17a74c0f9abef11ab2cebb281521a8358f1f09e463bd415fa3b6ee4bf1f229f0
```

## Composition with persistent input PR #287

This feature does not depend on #287. An isolated local QA worktree combines
the frozen rollover commit `832bcfbf2d85f847d9ff2431c3c660c362fac8e3` with
the exact published persistent-input head
`dda3fad72e11e1ff880822adcc0b79a846e3604a`. Its initial union is
`fc69da38c2e797163e42b9c65a43e65f7908061b`, tree
`6cfc4f307007b64294975950668158eeccd9d40a`.

Four additive conflicts were resolved: both new Recorder method definitions,
both declarations, the adjacent media-documentation paragraphs and the adjacent
Windows acceptance blocks. Both complete operations, CI host gates, native
registrations, manifest components and required Windows checks remain present.
All **11 disjoint rollover files** and **24 disjoint persistent-input files**
are byte-identical to their respective frozen heads in that initial union.

On the union, the **1,617 rollover checks and 15 decoded outputs**, the
**81 persistent-recorder checks and two decoded outputs**, and the
**198 checked-JVM state checks** pass. All six recordapp Java sources compile
against the public SDK and actual framework/libcore; D8 produces 93,652-byte
and 93,704-byte outputs respectively. The real PowerShell parser accepts the
combined track and validates both rollover result-check bindings.

A second **QA-only fixture change**, with no production changes, directly feeds
one persistent producer across an MP4 rollover. At 803 it verifies that the same
producer and encoder exist and that a competing consumer cannot acquire the
still-owned lease. After terminal stop, the next consumer can acquire it and the
producer remains usable. Both resulting MP4s independently decode. This expanded
persistent fixture passes **155 checks** with ASan/UBSan. Its local QA commit is
`0f8f18aab40200f1231773b7a52d6ca88a0b4136`, tree
`0901d805c4d81169ab65f280282f01d9120c466b`; only the persistent fixture and its
runner differ from the initial union. These are host composition results, not
execution of either public Windows acceptance activity.

After the independent review's invalid-fd precedence correction, the isolated
union merged final production commit
`4f26a72cbec04908eb5a916cb2169753f8758e69` without conflicts. This QA head is
`6d518c6da7898a2da5ec15629019ee79eb63089a`, tree
`fa5b95e4a3b7b527c78156ba58626bf499884024`. The updated **204 checked-JVM**
assertions and **155 persistent/rollover ASan/UBSan** assertions pass again, with
all four persistent-recording outputs independently demuxed and decoded. The
standalone final production source separately passes its full **1,617 rollover
assertions and 15 decoded outputs**. No writer/allocation-benchmark code changed.

## Required Windows runtime acceptance

`RecorderRolloverActivity` creates one real EGL window on the recorder's Surface
and renders visible, monotonically numbered black/white source frames. It
supplies its first next file using the FileDescriptor overload and closes that
fd immediately; it supplies the next using the File overload. Both calls occur
after the relevant 802 callback. It requires the ordered three-file event
sequence and the same public Surface object after both 803 callbacks.

After recording it extracts and decodes all three MP4s. Every stored frame must
decode to a later identifier actually submitted by this producer; each file
must start at a sync sample, have identical real CSD, preserve orientation and
have increasing packet/decoded timestamps. Documented boundary drops are allowed;
there is no claimed public API count of every accepted Surface buffer. The test
also checks before-prepare and released/null-call errors and thread cleanup.

Track 59 requires this activity both in the ordinary single-process runtime and
as a device app. It selects the actual software AVC implementation and removes
synthetic microphone input. Missing APK/pass lines, FAILED output, timeout and
nonzero runtime exit fail the gate. The device check reads only the log bytes
appended by that launch so a previous pass cannot satisfy it.

**Local validation did not execute Windows, ART, EGL, Binder, real codecs or
physical capture hardware.** The new Windows acceptance gate remains pending
the matching full CI run. No complete Windows/Android runtime or hardware pass
is inferred from the host adapters, compile results or earlier PRs.
