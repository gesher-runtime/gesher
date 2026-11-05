# Framework software JNI contracts, 2026-11-05

This addresses part of N01 in the [systematic inventory](systematic-compatibility-2026-11-05.md).
Five classes now have 18 actual native bindings, grouped into four regression
fixtures. A registration, class-presence result or successful runtime startup
is not treated as proof of the operations below.

## Implementations

| Class | Implementation and scope |
| --- | --- |
| `android.os.MemoryFile` | AOSP `android_os_MemoryFile.cpp` operations, registered through System V entry thunks. Storage is the existing `SharedMemory` implementation; size/pin operations use libcutils' host ashmem backend. |
| `android.util.MemoryIntArray` | AOSP `android_util_MemoryIntArray.cpp`: file-backed shared atomic integers, owner/read-only reader mappings and descriptor/unmapping lifecycle. A local registration table supplies System V entry thunks. |
| `android.security.Scrypt` | Existing BoringSSL `EVP_PBE_scrypt`, implementing RFC 7914 without an additional memory ceiling. Null inputs/derivation failure return null as AOSP does; JNI allocation exceptions remain pending, with acquired arrays released through scoped owners. |
| `android.net.LocalSocketImpl` | Seven natives over Gesher's descriptor-based AF_UNIX stream operations. Filesystem bind/connect, single-byte and array transfer, partial-write completion and EOF are implemented. The upstream libcutils Windows helpers and libbase ancillary backend are not usable implementations in this build, so simply compiling the upstream source would not supply the required behavior. |
| `android.util.CharsetUtils` | ART JNI string conversion and AOSP's temporary-terminator decoder. Capacity arithmetic avoids signed overflow, and null-address/invalid-range guards prevent several invalid raw-buffer calls from reaching pointer accesses. |

Source commits are `67ccb0fb`, `6767251d`, `20d39858` and `720394d1`;
bounded runner/integration-test commits are `b1f15e78` and `317e91d4`.
No AOSP checkout was changed: wrappers compile its selected sources directly,
and the new sources are selected by `cmake/aosp/framework.cmake`.

## Regression contracts

All fixtures use the boot framework's actual classes through reflection, so
they compile with a JDK alone and do not introduce replacement Android classes.
`tests/build-test-apps.sh` already includes them in `framework-tests.dex`.

| Fixture | Checks |
| --- | --- |
| `FrameworkMemoryTest` | MemoryFile length/native size, pin policy, byte offsets and streams, invalid bounds, closed descriptors and idempotent close. MemoryIntArray initialization, boundary integer values, invalid indices, Parcel descriptor transfer, read-only reader rejection, live update visibility and independent reader lifetime after owner close. |
| `FrameworkScryptTest` | Two RFC 7914 known-answer vectors, unchanged input arrays, null inputs, invalid/signed/overflowing costs, empty output, negative output exception and successful derivation after failures. |
| `FrameworkLocalSocketTest` | Filesystem connection, unsigned byte and offset-array transfer, zero-length read, overflowing offset rejection, unsupported descriptor send without payload loss, peer-credential error, half-close/reverse traffic, peer EOF, supplied descriptor ownership and missing/unsupported endpoint errors. |
| `FrameworkCharsetTest` | Exact ASCII, encoded NUL, Hebrew/euro, supplementary and lone-surrogate bytes; short/exact buffer capacity; unchanged failed output; decoder sentinel restoration; null/invalid ranges. Real ArtFastDataInput/Output callers with eight-byte buffers exercise long-string allocation/retry and oversized UTF rejection. |

`tests/windows/run-tests.ps1` includes all four. For bounded individual
processes with isolated data directories, use:

```powershell
tests/windows/run-framework-jni.ps1 `
  -RuntimeExe <candidate-directory>/dalvikvm.exe `
  -BootDir <read-only-framework-directory> `
  -ProfileDir <read-only-device-profile-directory> `
  -TestDex <compiled-framework-tests.dex-or-jar> `
  -OutputDir <private-result-directory>
```

The runner removes `GESHER_BINDER`, uses one private `ANDROID_DATA` per fixture,
caps each process at 30 seconds, and records separate stdout/stderr. It does
not start a Binder server or alter the installed runtime/profile.

## Verification and limits

All five new native sources and `android_runtime.cc` compile with the current
Windows candidate's flags, including `-Werror`. The four fixture classes compile
with the existing local JDK 21 and D8 for API 36. Candidate runtime results
are recorded separately after a coordinated host rebuild.

The pre-change `564118b1` host executable (SHA-256
`31423e063a16d760ed47dddb0655dad282f0befe6ff5130f84faeb43b35a6342`)
was tested against the actual installed framework, in private processes. All
four fixtures failed promptly at the expected missing native binding:
MemoryFile `native_get_size`, Scrypt `nativeScrypt`, LocalSocketImpl `bindLocal`
and CharsetUtils `toModifiedUtf8Bytes`. This establishes that the tests exercise
real previous gaps; it does not verify the replacement binary.

Retained limits:

- Host ashmem does not enforce its protection mask on subsequent mappings,
  distinguish every ordinary regular file from shared memory, or simulate
  Android memory purging. The Java MemoryIntArray reader policy and read-only
  mapping are useful contracts, distinct from complete hostile native mapping
  protection. No cross-process memory test is claimed here.
- Local sockets support the filesystem namespace in this slice. Android
  abstract/reserved namespaces and SCM_RIGHTS sends throw `IOException` with
  an unsupported-operation error. Credentials retain the backend's actual
  `getsockopt(SO_PEERCRED)` failure; no process identity is invented. General
  POSIX ancillary transport remains N04 work.
- CharsetUtils follows the pinned ART behavior: supplementary pairs emit
  four-byte UTF-8 sequences, NUL emits `c0 80`, and unpaired UTF-16 surrogates
  retain three-byte encodings. This differs from strict Java modified UTF-8's
  six-byte supplementary encoding and is intentional upstream behavior.
  Its raw-address API cannot validate the caller's actual allocation extent.
- These are x86_64 host JNI contracts. They do not establish an independently
  executed ARM64 managed runtime or general native ABI conformance.
