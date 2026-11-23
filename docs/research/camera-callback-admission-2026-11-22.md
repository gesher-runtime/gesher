# MEDIA-06: bounded Camera2 request and callback ownership

Related: [performance epic #178](https://github.com/gesher-runtime/gesher/issues/178),
MEDIA-06. Baseline: `8b316aebf09ab46d5407e955bf86ed748a1e739f`.
Owner: `palmoni5`.

## Problem and behavior

The previous `CameraCallbacks` used `newSingleThreadExecutor`, whose queue is
unbounded. `CameraDeviceUser` could capture continuously and enqueue started,
buffer-error and result metadata while one application callback was blocked.
Its nonrepeating request deque was also unbounded. Closing an executor did not
bound the number of retired workers left behind by repeated close/reopen cycles.

Request submission now reserves a complete burst atomically. Saturation returns
`ICameraService.ERROR_INVALID_OPERATION` before assigning request/frame IDs or
publishing any member of the burst. This is the existing Android submission
failure used when `captureList` fails in
[AOSP CameraDeviceClient](https://android.googlesource.com/platform/frameworks/av/+/c1408e601dc91edab856b567d0c2d9163392b94b/services/camera/libcameraservice/api2/CameraDeviceClient.cpp).
It never waits for callbacks while the application's framework lock may be held.
The repeating producer waits on notification before selecting/numbering another
frame when its reservations are exhausted. A repeating burst visits all its
subsequences in order.

Callbacks run on one ordered worker, outside the camera and callback locks.
Credits cover queued, producing and executing work. A frame returns its credit
only when its producer is finished and all admitted callbacks have returned.
A callback exception, including an application's `AssertionError`, does not
strand the queue's credits or following callbacks.
Required per-frame terminal callbacks are not dropped at saturation.

Flush first marks an active lower-numbered frame terminal, then cancels queued
requests with ordered `ERROR_CAMERA_REQUEST` callbacks. If the active camera
already captured an image, its abort uses `ERROR_CAMERA_RESULT` so Android can
report that buffers may be available. A result already published remains its
terminal result; cancellation cannot publish a second terminal result. A canceled native
read retains its credits until it actually returns. Disconnect wakes a parked
producer and closes admission without joining its callback worker. Native capture
retains the existing bounded join and read-lease ownership, so a blocked native
operation is not falsely reported as terminated.

## Explicit limits and ownership

| Resource | Limit / policy |
|---|---|
| In-flight frames per camera | 64, including queued requests, producing frames and callbacks still executing |
| Request metadata allocation | At most 256 KiB per request; measured from the actual native allocated buffer |
| Targets per request | 64, rejected before copying larger target arrays |
| Per-camera accounted storage | 8 MiB for retained request templates and frame/result/callback reservations |
| Result allowance | Twice the input native capacity plus 4 KiB and per-target callback allowance; covers the metadata allocator's geometric growth and fixed result tags |
| External preparation callbacks | 16 queued or executing; excess calls fail before admission |
| Idle callbacks | One separately reserved state callback at a time |
| Terminal camera disconnect | One separately reserved callback; available even when other pools are full |
| Callback owners per process | Eight, including closed owners with blocked callbacks or an unfinished native producer |

The repeating template owns a separate storage lease. Admission also requires
room for the largest subsequence's frame alongside the entire template, so a
large accepted burst cannot permanently wait for credits that no callback can
release. Frames retain their own
reservation across template replacement/cancellation. A native producer owns a
zero-byte lifetime lease so a blocked configuration/open cannot evade the owner
limit. These are retained-work budgets, not process working-set measurements.
Caller-owned input and application-retained results after delivery are outside
this runtime queue budget. Temporary preparation is serialized per camera;
count-saturated submissions are rejected before copying metadata.

There is no age-based silent callback eviction. An arbitrary application callback
can remain blocked indefinitely; its owner and bytes stay charged until it
returns. The bound limits that stalled consumer's resource damage. When all eight
owners remain occupied, another camera connection reports the Android
`ERROR_MAX_CAMERAS_IN_USE` resource error. After callbacks/producers retire, new
connections can progress again.

## Executed host evidence

The A/B fixture compiles each revision's complete `CameraDeviceUser` and
`CameraCallbacks` sources. It holds the first application callback, then observes
until the native-read counter reaches 10,000 or the producer parks. Native capture
and framework metadata value objects are explicit host adapters. The observation
thread may see a few additional baseline reads before cancellation takes effect.
After unblocking, the fixture requires one terminal callback for every captured
frame in both revisions.

| Observation | Baseline | Candidate |
|---|---:|---:|
| Captures accumulated behind blocked callback | 10,005 at the observation boundary | 64, then producer waits |
| Terminal callbacks after unblocking | 10,005 | 64 |
| Capture worker stopped by cancel/disconnect | Yes | Yes |

Raw output, exact production-source hashes and measurement limits are in
[data/camera-callback-admission-2026-11-22.json](data/camera-callback-admission-2026-11-22.json).
These counts establish bounded accumulation. They do not claim a physical-camera
FPS improvement, CPU ratio, launch improvement or whole-process RAM reduction.
CPU, latency and total private commit remain unmeasured here.

The full-owner host gate additionally passed 1,227 checks: 1,024 saturated
submissions without new metadata copies/identities, native-byte admission,
ordered flush errors, bounded controls, repeat cancellation, failed callbacks,
failed capture, reentrant submit/flush/disconnect, eight blocked retired owners,
and 1,000 idle reopen/close cycles. Independent review additionally reproduced
and drove regression tests for an oversized repeating template with no progress
room, active-before-queued flush order, and continued dispatch after an
application `AssertionError`. Existing capture dispatch (1,036 checks),
native read/close ownership (1,544 checks), and 1,000 contended cancel/flush
selection cases also passed. Full app compilation/D8 against the pinned Android
framework and libcore passed.

The new `CameraCallbackAdmissionTest` is wired into the Windows devices and media3
tracks. It uses the actual ART framework, actual native metadata allocator/size
adapter, and actual request/callback owners. Its producer is explicitly held so
it does not substitute a fake source in an ordinary camera flow. Existing
isolated synthetic-camera integration tests continue to exercise capture and
pixel delivery. Windows execution results must be read from the exact published
head; adding or compiling that gate alone is not a passed Windows result.

## Reproduction

```sh
python tools/test_camera_callback_admission.py --java-home "$JAVA_HOME"
python tools/test_camera_callback_admission.py --java-home "$JAVA_HOME" --measure \
  --source-ref 8b316aebf09ab46d5407e955bf86ed748a1e739f
python tools/test_camera_callback_admission.py --java-home "$JAVA_HOME" --measure
python tools/test_camera2_capture_dispatch.py --java-home "$JAVA_HOME"
python tools/test_camera_capture_lifetime.py --java-home "$JAVA_HOME"
```

The epic's fixed-hardware CPU/latency/private-commit workload comparison,
physical-camera unplug/permission testing, combined resource budgets and
eight-hour soak are still separate acceptance work. This PR does not close #178.
