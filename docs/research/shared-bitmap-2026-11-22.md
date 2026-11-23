# Shared immutable Bitmap storage and Parcel forwarding

This implements the bounded shared-Bitmap storage/transport leaf associated
with issue #56 (G11), based on main
`20252fde18db387b4d99d99f7050d6336f58bf6e`. It restores the actual
`Bitmap.asShared()` storage contract and removes repeated full-image heap
allocation/copying from immutable Bitmap Parcel chains. It does not claim
complete graphics parity, a hardware bitmap backend or a whole-application
speedup.

The chosen application pixels, color information, density, stride, source
identity, mutability and descriptor restrictions remain meaningful inputs.
No substitute image, package exception, capability claim or production test
source is introduced. Synthetic pixels and platform adapters are confined to
the explicit tests. The public Windows test exercises ordinary Bitmap and
Binder APIs with its own declared test pixels.

## Contract and original implementation

All Android evidence is from the project's pinned `android-16.0.0_r4` tag:

| Primary source | Relevant operation |
| --- | --- |
| [Bitmap.java](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/graphics/java/android/graphics/Bitmap.java) | `asShared()` returns an immutable shared-memory Bitmap, or the existing object when it already meets those conditions. `createAshmemBitmap()` retains density/premultiplication and the existing gainmap path. |
| [hwui Bitmap.cpp](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/libs/hwui/hwui/Bitmap.cpp) | The public allocator honors the SkBitmap row stride; the private Android allocator maps writable storage, restricts future writes, and initializes it through the existing mapping. Mapped Bitmap ownership includes its descriptor and mapping. |
| [Bitmap JNI](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/libs/hwui/jni/Bitmap.cpp) | The Linux Parcel writer forwards an immutable ashmem descriptor even for a small Bitmap. A large ordinary source is copied into immutable shared storage. The reader maps according to the declared mutable flag and adopts the mapping. |
| [SharedMemory.java](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/os/SharedMemory.java) | Its Parcel representation is `writeFileDescriptor`; its creator owns the received descriptor. Mapping and closing the Java owner are separate lifecycle operations. |
| [Parcel.java](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/os/Parcel.java) | `writeFileDescriptor` gives the Parcel its own duplicate; file-descriptor allowance remains a property of the destination Parcel. |

The original Gesher Parcel bridge used SharedMemory for ordinary images over
12 KiB when descriptors were allowed, but every recipient allocated a complete
heap pixel array and copied the mapping into it. Forwarding that recipient
created another shared region and copied the full image again. The original
upstream host allocator also fell back to heap storage for `asShared()`, and
the Windows mapped `Bitmap::createFrom` returned null. Thus the public shared
identity/idempotence contract and efficient descriptor-forwarding path were
not available even though large-image transport already used descriptors.

## Implementation and ownership

`SharedBitmapRegion` owns one native descriptor, mapping and capacity. Its
allocator creates a real memfd with close-on-exec and sealing enabled, sizes
it, maps it writable for initialization and applies `F_SEAL_GROW`,
`F_SEAL_SHRINK` and `F_SEAL_FUTURE_WRITE`. This prevents later shared writable
mappings, writes through the descriptor and resizing while retaining the
initial writable mapping for the existing Skia copy. It does not pretend the
host is an Android build or change hardware capability checks.

The narrow hwui patch enables its existing mapped Bitmap adoption/destructor
on Gesher Windows and delegates only the private ashmem allocation overload to
the new allocator. The original public allocator, Skia copy, immutable flag,
Bitmap wrapper and SkPixelRef ownership remain in place. Bitmap destruction
unmaps its own view and closes its own descriptor. Other platforms retain the
upstream branches.

The reader's temporary Java SharedMemory owns the descriptor received from
the Parcel. Before closing that owner, the native helper duplicates the fd,
checks its real `fstat` capacity, maps the declared size and transfers both
resources to the native Bitmap. Each recipient therefore survives sender
recycle, Parcel recycle and Java SharedMemory close. It never borrows the
sender's mapping or fd lifetime. Descriptor/mapping cleanup is RAII; cleanup
preserves an already-pending Java exception and releases a successfully
adopted native Bitmap if a later Java-owner close fails.

| Source and destination | Resulting behavior |
| --- | --- |
| Immutable native shared Bitmap, descriptors permitted | Forward its existing fd and allocation size. The Parcel and recipient acquire independent ownership; no new pixel array or memfd and no full-image copy. Small shared Bitmaps use this same path. |
| Ordinary large Bitmap, descriptors permitted | Allocate one sealed shared region and copy the original readable pixels. The recipient is immutable, preserving the previous large-blob contract. |
| Mutable Bitmap with existing shared backing | Preserve actual mutable aliasing on receive, but snapshot on `asShared()` and on large-blob forwarding. Mutable source backing is never reused for immutable transport. |
| Ordinary small Bitmap or descriptors forbidden | Use inline bytes, preserving the existing inline mutability rules. A received inline Bitmap owns its heap storage. |

An imported mutable shared blob is mapped writable; an imported immutable blob
is mapped read-only. Adoption does not add seals to somebody else's supplied
descriptor. The newly allocated immutable snapshots are the regions whose
write/resize permissions this implementation seals.

The writer validates the readable Skia byte size and complete row allocation.
Skia's readable size can omit final-row padding, while the receiver validates
`rowBytes * height`. The new memfd or Java byte array is zero-initialized; the
writer copies only the readable bytes and retains a complete declared stride
allocation. This avoids reading past the last row or emitting a short padded
image. Positive overflow, invalid dimensions/stride/color/blob metadata,
declared sizes above actual fd capacity and syscall failures fail before
invalid pixel access. Actual capacity is checked before mapping an incoming
descriptor. Existing source IDs, serialized color information and density
continue to travel in the same wire layout.

The self-audit also reproduced an existing malformed-wire zero-stride hole:
Skia accepts zero in `setInfo` as a request to infer the default stride, but
the separate allocation calculation used the raw zero value. A nonempty
serialized Bitmap must now have a positive explicit row stride. Both inline
and shared zero-stride regressions are required; the pre-correction source
fails the new shared regression by accepting it. Valid Bitmap writers already
emit a positive stride. This defensive malformed-wire rejection is not labeled
an Android runtime differential pass; the pinned reader has the same raw
zero/default distinction.

## Source-based work comparison

`tools/test_bitmap_shared.py --source-ref 20252fde...` compiles the unchanged
baseline Parcel bridge together with the original pinned host allocation
fallback. The candidate compiles the complete production Parcel and storage
units plus the pinned Bitmap class declaration and selected unchanged hwui
allocation, adoption, ID and destruction methods. The Skia pixel/refcount and
Java Parcel/SharedMemory transport adapters are explicit test boundaries;
actual Linux descriptors, mappings, seals and JVM native calls execute.

Each case performs **20 sequential Parcel write/read hops**, recycles each
sender and checks every recipient's complete logical pixel checksum. Initial
source creation, the optional initial `asShared()` call and pixel verification
are outside the work counters equally. The second case means the public
`asShared()` API was called; the original host implementation still returned
heap storage, which is the actual baseline limitation being repaired.

| Image size | Baseline explicit pixel-copy bytes | Candidate, initial mutable source | Candidate, after initial `asShared()` | Baseline recipient heap-pixel bytes |
| --- | ---: | ---: | ---: | ---: |
| 1280 × 720 | 147,456,000 | 3,686,400 | 0 | 73,728,000 |
| 1920 × 1080 | 331,776,000 | 8,294,400 | 0 | 165,888,000 |
| 3840 × 2160 | 1,327,104,000 | 33,177,600 | 0 | 663,552,000 |

For each size the baseline performs **40 full-pixel copies, 20 heap-pixel
allocations and 20 new memfds**. From an initially mutable source the candidate
performs **one copy, no recipient heap-pixel allocations and one new memfd**:
97.5% fewer copied bytes and 95% fewer new regions. Starting after `asShared()`
removes all subsequent pixel copies, pixel heap allocations and new memfds.
All six cases have exactly matching checksums at every hop.

There is a disclosed fixed-cost tradeoff: fd duplications increase from
**40 to 60** per chain because each recipient now retains an independently
owned native descriptor. Mapping calls decrease from **40 to 21** with the
initial snapshot, or **40 to 20** after `asShared()`. These are cumulative
pixel-storage/work counts, not all metadata/object allocations, peak RSS,
elapsed latency or a complete application benchmark. Smaller explicitly shared
Bitmaps may have proportionally greater descriptor overhead; ordinary small
Bitmaps keep the inline path.

The baseline produces **2,420 CheckJNI warnings** for its existing unchecked
exceptions between Java Parcel calls. The baseline comparison preserves that
source and records those known warnings; it is a pixel/work control, not a
JNI contract pass. The candidate rejects any CheckJNI warning and reports
none. All measurements, source hashes, reference hashes, counter fields and
Android build-output hashes are in
[shared-bitmap-work-2026-11-22.json](shared-bitmap-work-2026-11-22.json).

## Native and checked-JVM acceptance

The native gate passes **1,022 assertions** with ASan/UBSan. It exercises real
seals, denied descriptor writes/resize/writable maps, independent mappings,
100 retirement cycles, real fd/map counts and injected create/truncate/map/
seal/dup/stat failures. A real `fork` and `SCM_RIGHTS` transfer discards the
child's inherited sender resources before receive. The parent then destroys
its final source mapping/fd before the child reads all 65,536 bytes. Both
processes retire their actual fd/mapping resources. This is Linux process
transfer, not a simulated Binder result.

The checked JVM gate passes **505 assertions with zero JNI warnings** while
executing the complete production JNI bridge and actual pinned public
`asShared()` method. It covers shared identity/idempotence, mutable snapshots,
five-hop forwarding, sender/Parcel retirement, padded strides, density,
serialized color metadata, source IDs, fd-forbidden inline transport, a
writable imported shared blob and independent snapshots from that blob.
Malformed negative and `INT_MAX` fields, short real fd capacity, syscall
failures, every Parcel header read/write failure, Java close after native
adoption and a Parcel that owns an fd before throwing all retain correct
cleanup. Six threads each create/verify/retire 40 independent recipients of
the same live immutable source.

Reproduction, with a JDK 21 and C++20 host compiler:

```sh
python3 tools/test_bitmap_shared.py --sanitize --output-dir /tmp/bitmap-candidate
python3 tools/test_bitmap_shared.py \
  --source-ref 20252fde18db387b4d99d99f7050d6336f58bf6e \
  --output-dir /tmp/bitmap-baseline
pwsh -NoProfile -File tests/windows/test-bitmap-shared-gate.ps1
```

The native runner retrieves and SHA256-verifies the exact pinned reference
files by default. `--reference-dir` can supply those same verified files;
`--java-home` selects the JDK; `--keep-source-dir` retains generated native
source/headers for inspection. On this ptrace executor only, the native run
used `ASAN_OPTIONS=detect_leaks=0`; CI keeps LeakSanitizer's default behavior.
Explicit fd/mapping retirement assertions remain enabled everywhere.

The complete new `shared_bitmap.cc` also compiles for both publisher
LLVM-MinGW `20261006` targets, x86-64 and AArch64, using the real Gesher POSIX
headers and pinned Bitmap class declaration. Skia headers remain the explicit
test adapter in this compile. This checks Windows declarations/types in the
new helper; it is neither a complete hwui link nor a Windows execution result.

## Mandatory Windows and actual Android API gates

All five surfaceapp Java files compile against both the pinned public Android
36 SDK and the available real Android 16 framework/libcore classes. Both D8
outputs are **36,180 bytes**. The updated framework-level
`BitmapParcelCapacityTest` compiles/D8s to **7,628 bytes**. These are genuine
Java/DEX compiler results; local APK resource packaging and runtime execution
are not claimed.

`BitmapSharedActivity` and its private `:bitmapRelay` service use public SDK
APIs only. They perform two cycles at 9×7 and 641×359 pixels. Each transfer
requires descriptor transport even for the small shared image. The recipient
process verifies all pixels and retains its own Bitmap; only after the original
sender and Parcel have been recycled may it forward that Bitmap back. It
recycles its own Bitmap before the reply returns. A third Binder call confirms
its owner has retired before the original process verifies every returned
pixel, config, color space, density, immutable state and `asShared()` identity.
The app requires different parent/relay PIDs and the same relay process across
all calls. Its own mutable original is overwritten/recycled before use of the
shared snapshot.

Track 52 launches this activity on the real system-server/device path. It
accepts only this launch's appended log bytes and requires all four completed
cases plus the eight-hop final result. Application failures, log truncation,
missing phases/output and nonzero exit fail the existing `SurfaceCheck` gate.
The maintained PowerShell control parses the actual full track and executes
its actual condition block: a complete declared log fixture passes and
**nine negative controls fail**. These log fixtures qualify the gate's failure
handling, not the Bitmap implementation.

The existing Windows services gate also requires the extended
`BitmapParcelCapacityTest`: ARGB_8888, RGB_565, ALPHA_8 and RGBA_F16 shared chains,
sender recycle before receive, independent exported SharedMemory closure,
read-only mapping, denied future writable mapping and denied shrink, alongside
the original short-capacity regressions. The public Binder app supplies the
separate-process boundary; the framework test supplies actual Skia formats and
hidden descriptor-inspection APIs. Neither test fabricates production storage.

**Windows/ART/Binder/actual Skia pixel execution and a complete patched hwui
build remain pending the matching full CI run.** Host adapters do not qualify
gainmap rendering, hardware bitmaps, GPU upload, actual Windows memory usage,
all Android error precedence or physical devices. No full #56 graphics row is
closed merely by these local results.

## Source identities

| Tested source | SHA256 |
| --- | --- |
| Baseline `bitmap_parcel.cc` | `641c84900054630ae8827a738f614feea8dbfcac8629a30709a58d98b0e0e3ae` |
| Candidate `bitmap_parcel.cc` | `2e50d9bbfca22b229a02dc868a694208acb2f1fa63f87f3cdf63cdeffca66e46` |
| `shared_bitmap.cc` | `51545490f9e8fe925696967aa9a7bc3a813c037d0e917e09c0ff5e11de549272` |
| `shared_bitmap.h` | `ec58301bfbb14b5e74c8e8554af7c18fd5dbe812ae2e20cf07a623087c68b072` |
| Pinned raw hwui `Bitmap.cpp` | `8c85f818808825f8633ba5e2239564bf6956c385ad7365b85755e96f5380c9f4` |
| Patched hwui `Bitmap.cpp` | `b3530ae6960dd53da69e61f489b601dde2f3cee9bdbdf2c23ef83305e2992dba` |

The companion JSON pins every native fixture, public SDK caller, maintained
PowerShell gate and compiled Android artifact in this report. The PR's final
commit/tree identifies the complete deliverable; counters from another source
are not relabeled as execution of a later merged tree.
