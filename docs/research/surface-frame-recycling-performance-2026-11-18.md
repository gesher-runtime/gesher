# Surface frame storage and publication work

## Scope and exact baseline

Baseline: `f93a90fac55dbe1aff641e6e3f26879292907cb2`.

`QueueSurface::Publish` previously allocated and value-initialized a complete
pixel vector on every publication, then copied or converted the producer into
it. Even ordinary packed RGBA restarted `memcpy` once for every image row.
The immutable copy is necessary: the producer may write its buffer again while
a TextureView, SurfaceView, encoder callback or retained Skia image still reads
an older frame.

This change reuses **only the pixel allocation of a destroyed frame**. It also
copies ordinary packed RGBA in one `memcpy`; format conversion and the strided
fallback keep their existing pixel selection. It never changes an application's
input, requested operation, callback count, frame timestamp or output format.

The tested combined production SHA256 (complete QueueSurface header/source plus
the actual `FrameImages` class) is:

- Baseline: `4f38634341b1991aa0804f69fd162d95e465d066fe21454d4c037ad598756d33`.
- Changed: `a4c33b09479887e268e64f284b88182c6ced5dba543b1ae4a6eb6021978004ec`.

## Ownership and resource bound

Every publication still creates a fresh frame and shared ownership control
block. Its private derived object's destructor returns the pixel vector after
the final strong reader has released it. `make_shared` retains the correct
derived destructor when the public pointer becomes `shared_ptr<const
SurfaceFrame>`. No `use_count()` test permits mutation of a published frame;
concurrent promotion of an external weak pointer retains the original object.

The Surface owns a separately locked pool containing at most **one spare
vector, with capacity no greater than 32 MiB**. One 3840 × 2160 RGBA vector
fits. Larger images still publish normally and are not cached. A returned
frame must match both the current geometry and generation; changing size away
and back cannot admit a late frame from the earlier generation.

Geometry changes release obsolete spare storage immediately. Abandonment
closes the pool before dropping the latest frame, so concurrent late returns
cannot repopulate it. Destruction also closes the pool. Frames refer weakly to
the pool: an outstanding reader cannot retain the Surface or its spare pixels.
The pool never acquires the Surface mutex or invokes a listener. Listener
dispatch remains outside the Surface mutex, preserving reentrant reads.

`FrameImages` keys its cache by frame identity and retains that frame through
`SkData`. Fresh frame objects preserve that mechanism; recycling a vector cannot
resurrect an expired weak frame or alter pixels still owned by a cached image.

### Memory tradeoff

This optimizes allocation and initialization work. **It does not reduce idle
resident memory.** A warmed Surface can keep one additional image's pixel
capacity while idle. The bound is per Surface, not per process. Existing
readers may continue to retain any number of old frames; their immutable
ownership is preserved rather than capped or discarded.

For an ordinary sequence with no externally retained older image, requested
pixel payload storage is:

| Storage | Before | After |
| --- | ---: | ---: |
| Steady state | Producer + latest: 2 frames | Producer + latest + one spare: 3 frames |
| Publication peak | Producer + old latest + new latest: 3 frames | Producer + latest + reusable storage: 3 frames |
| Full HD steady payload | 16,588,800 B | 24,883,200 B |
| Full HD publication peak | 24,883,200 B | 24,883,200 B |
| 4K steady payload | 66,355,200 B | 99,532,800 B |
| 4K publication peak | 99,532,800 B | 99,532,800 B |

These are exact C++ payload requests in the stated fixture, excluding allocator
overhead and small frame/pool metadata. They are not Windows commit, process
RSS, physical GPU allocations or a budget for complete applications.

## Measured publication work

The same complete production-source fixture runs both revisions. Each workload
warms the Surface, then performs five rounds with the stated publication count.
The producer already contains the real input bytes. Timed publication includes
the real CPU lock/unlock, frame ownership, copy and listener dispatch. The image
cache cases also execute the actual `FrameImages` class against a declared
SkData/SkImage lifetime adapter.

| Workload, per round | Pixel allocations before → after | Requested payload bytes before → after |
| --- | ---: | ---: |
| 64 × 64, 500 posts | 500 → 0 | 8,192,000 → 0 |
| 1280 × 720, 180 posts | 180 → 0 | 663,552,000 → 0 |
| 1920 × 1080, 120 posts | 120 → 0 | 995,328,000 → 0 |
| 1920 × 1080, 120 posts with image cache | 120 → 0 | 995,328,000 → 0 |
| 3840 × 2160, 30 posts with image cache | 30 → 0 | 995,328,000 → 0 |

The warmed pixel vector already has the required size, so its later `resize`
does not value-initialize another frame. The immutable producer-to-frame copy
remains. The new packed-RGBA path performs that copy in one contiguous call.

One non-sanitized host measurement on this shared Linux executor gave these
five-round median elapsed times:

| Workload | Baseline | Changed |
| --- | ---: | ---: |
| 64 × 64, 500 posts | 0.256 ms | 0.147 ms |
| 720p, 180 posts | 27.298 ms | 16.562 ms |
| Full HD, 120 posts | 76.677 ms | 39.160 ms |
| Full HD + image cache, 120 posts | 99.713 ms | 44.327 ms |
| 4K + image cache, 30 posts | 121.945 ms | 64.527 ms |

These timings describe the tested publication work, not application frame
rate or total CPU. They vary with competing executor work, cache state,
allocator and platform; they are informational and are not CI thresholds.
Allocation/lifetime budgets are the deterministic gates. No Windows GPU,
display compositor, ART application or physical-device performance was timed.

## Validation

- The new complete-source host suite passes ASan/UBSan. The final local run
  reported **398,788 checks, zero failures**; its number of concurrent pixel
  observations varies with scheduling.
- The baseline passes the identical behavior assertions in `--measure-only`
  mode. With the new budgets enabled it fails **exactly 25 allocation checks**
  (five workloads × five rounds). No behavior assertion is disabled by that
  mode; only the newly introduced allocation budgets are omitted.
- Pixel oracles cover RGBA, RGBX and BGRA, retained image-cache reads, signed
  and zero timestamps, callback order/reentry, damage/copyback, stale weak
  pointers, size changes away and back, and old dequeued producer geometry.
- Concurrent publication, strong/weak readers and concurrent final-owner
  returns check immutable bytes and the single-spare limit. Tests also cover
  abandonment, destruction with surviving readers, and uncached images above
  the 32 MiB capacity limit.
- Existing geometry **158**, abandonment/release **35**, image-cache **48**,
  timestamp **20** and codec-window ownership **31** checks pass. Existing
  assertions were kept; the timestamp fixture includes the actual new pool
  classes when testing the changed source.
- Complete production `queue_surface.cc` and the new native Windows fixture
  both compile for **Windows x64 and ARM64**, using pinned LLVM-MinGW
  `20261006`, C++23, `-O2 -Wall -Wextra -Werror -fno-exceptions`.
  Android headers come from official `android-16.0.0_r4` AOSP archives;
  upstream headers are system includes. The preincludes and warning exceptions
  match `gesher_graphics_cxx` in `cmake/aosp/hwui.cmake`.
- The new sanitizer fixture is in unconditional CI preflight. A new default
  CMake target and the existing Windows deep-compatibility track require the
  allocation/lifetime check against actual hostgraphics objects.

Local sanitizer runs set `ASAN_OPTIONS=detect_leaks=0` because LeakSanitizer
process inspection is unavailable under ptrace. ASan and UBSan remain fatal;
the CI invocation does not disable LeakSanitizer. Payload lifetime counters
also verify complete release after every workload.

**Fresh Windows/ART execution remains pending.** Cross-compilation is not an
execution result. The new native fixture was compiled as a complete translation
unit; the full Windows runtime and its linked fixture were not run locally.

## Reproduction

```sh
python3 tools/test_surface_frame_recycling.py
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_surface_frame_recycling.py --sanitize
python3 tools/test_surface_frame_recycling.py \
  --source-ref f93a90fac55dbe1aff641e6e3f26879292907cb2 --measure-only

# Expected exit 1: exactly 25 new allocation budgets reject the baseline.
python3 tools/test_surface_frame_recycling.py \
  --source-ref f93a90fac55dbe1aff641e6e3f26879292907cb2

ASAN_OPTIONS=detect_leaks=0 python3 tools/test_surface_geometry_contracts.py --sanitize
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_surface_image_cache.py --sanitize
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_media_surface_contracts.py --sanitize
```

On a full Windows build, run `media_surface_frame_recycling_test.exe`; the
deep-compatibility track runs it automatically. Its assertions exercise real
hostgraphics reference ownership, 120 warmed 720p publications without pixel
allocations, expired weak-frame identity, exact pixels, resize, abandon and
destruction with a surviving reader.

## Contract and pending integration

The [Android native-window contract](https://developer.android.com/ndk/reference/group/a-native-window)
defines the producer queue, CPU lock damage region and unlock/post operation.
The [Surface contract](https://developer.android.com/reference/android/view/Surface)
defines the producer surface and lifetime. Gesher's existing immutable
`Latest()` result and `FrameImages` retained backing remain unchanged.

Pending PR #94 at `7e12abf08b7354e0869a37e3d46c014a8f52ba41` moves publication
into an atomically timestamped `PublishLocked` path. Its header merges cleanly,
but its moved/reindented publication body overlaps this allocation/copy change
and requires a conflict resolution that retains both. This PR is independently
based on the baseline main commit; it does not claim that the pending branches
merge automatically or that their combined Windows runtime has been validated.

The coordinator independently reproduced the final-source sanitizer pass:
338,372 checks, zero failures at the changed production hash above. A separate
scratch resolution of the one #94 source conflict retained its atomic
`PublishLocked`/timestamp flow and this pool/copy implementation. That combined
source passed ASan/UBSan geometry **158**, release **38**, timestamp **25**,
codec-window ownership **194** and recycling **346,564** checks, all with zero
failures. This is manual source-integration evidence; a fresh combined Windows
runtime execution remains pending.
