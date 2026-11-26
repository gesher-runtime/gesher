# ARM32 execution components

**Status: experimental infrastructure. ARM32-only Android APKs are not yet
supported. This work does not add x86 32-bit application support.**

Gesher's existing application path runs Java on x86-64 ART and native libraries
through its x86-64 loader or AArch64 translator. A 32-bit native library has a
different address model and calling convention. Allowing its APK through the
installer before the complete native bridge exists would admit an application
that cannot run.

The components in `compat/arm32` implement a checked, independently testable
execution path for bounded, self-contained ARM EABI5 shared objects. They have
a standalone build. They are not linked into `dalvikvm`, installed as a runtime
DLL, or advertised by `NativeLibs.SUPPORTED` or `build.prop`.

## Implemented path

| Component | Implemented contract |
| --- | --- |
| `arm32_memory` | A separate 32-bit guest address domain, zero-filled pages, independent read/write/execute permissions, checked range arithmetic, transactional memory operations and synchronized scalar/CAS access. |
| `arm32_abi` | Scalar Android softfp argument/return frames, even-register alignment for 64-bit values, stack spills, floating-point bit preservation, JNI descriptor validation and explicit opaque guest handles. |
| `arm32_elf` | Checked little-endian ELF32 ARM EABI5 softfp `ET_DYN` parsing, owned image bytes, SysV/GNU symbols, ordinary REL relocations, standard/legacy Android RELR and bounded Android APS2 packed REL groups, explicit imports, BSS, init/fini arrays and validated RELRO ranges. |
| `arm32_image_memory` | Mapping relocated image bytes into guest pages, inaccessible segment holes and read-only RELRO protection. |
| `arm32_executor` | ARM/Thumb execution through the pinned Dynarmic A32 frontend, finite instruction budgets, checked instruction/data access, explicit faults and typed host-function thunks. |
| `arm32_program` | Isolated dependency-DAG loader, guest-address allocation, typed host import bindings, transactional load rollback, guest constructors and reverse-order finalizers, exported function calls and explicit shutdown. Not integrated with ART or APK packaging. |
| `arm32_bionic` | A narrow ILP32 guest-memory libc adapter for malloc/calloc/realloc/free, memcpy/memmove/memset/memcmp and strlen/strnlen/strcmp/strncmp; a guest `__errno` cell with ENOMEM on allocation failure, bounds checks and typed imports. Not a complete Bionic implementation. |
| `arm32_jni` | Guest `JNIEnv*` and `JavaVM*` with read-only 32-bit tables, typed JNI callbacks, generation-checked Java references, local/global reference ownership, UTF copies, `JNI_OnLoad`, and descriptor-aware named native / `@CriticalNative` calls. The owning thread alone supports `GetEnv` and attach; guest thread creation and detach are unsupported. This is an isolated component with an injected host JNIEnv, not ART native registration or complete JNI32. |

The linked-program test loads and calls the compiled ARM and Thumb fixtures through `Program`, including their constructors and typed host imports. Additional actual compiler-generated ARM/Thumb fixtures test libc imports and a two-DSO `DT_NEEDED` graph with guest-side constructor/finalizer ordering assertions. The dependency's destructor traps if the consumer has not run its own destructor first. The `arm32_bionic` adapter confines pointers to `GuestMemory`; it does not expose host pointers to 32-bit guest code. Its `__errno` cell is per standalone Program, not thread-local: real guest threads require a distinct errno/TLS design before application use. These are component test paths, not Windows/ART APK runs. The executor runs the compiler-generated fixtures in `tests/arm32`: constructors,
relocated data pointers, integer and floating-point calls, 64-bit register/stack
arguments, guest-pointer reads and writes, and an imported host adapter. ARM and
Thumb builds are both checked by inspecting the actual exported function state
bits. A compiler flag alone is insufficient evidence of Thumb coverage.

### Address and ABI boundaries

Guest addresses are `uint32_t` values. They are translated by `GuestMemory`;
they are never used as host pointers. Page zero stays inaccessible, range ends
are checked in a wider integer, and mappings are subject to a configurable
backing-memory budget. The atomic compare-exchange primitive is not an ARM
exclusive monitor; the execution layer also maintains reservation state.

Android's `armeabi-v7a` uses the base AAPCS32 softfp calling convention: floating
values travel in core registers or on the stack, with aligned register pairs
for doublewords. Host adapters declare parameter/result kinds. A `Pointer32`
parameter remains a guest value; the adapter explicitly reads or writes its
buffer through `GuestMemory`. No generic cast to an arbitrary host function is
available.

The isolated `Jni32` component maps an actual guest `JNIEnv*` function table with
four-byte entries, and binds only implemented slots. It handles reference
lifetimes, host Java-thread affinity, bounded copied UTF strings, exceptions
and several common object/array operations. `CallNative` marshals scalar and
reference arguments according to the JVM method descriptor, runs guest
`Java_*` exports with AAPCS32 and translates their return values. Host
references are never truncated into a 32-bit guest pointer. CriticalNative
calls omit JNIEnv and receiver. Unknown table slots are null and fault
closed; they are not silently simulated. The test harness supplies an
in-process fake host JNIEnv, not a running ART instance. The guest JavaVM invoke table supports `GetEnv`, `JNI_OnLoad` and
same-thread `AttachCurrentThread` / `AttachCurrentThreadAsDaemon` when
the real ART thread is already attached. `DestroyJavaVM` and
`DetachCurrentThread` explicitly return JNI_ERR, and new guest threads
are unsupported. The actual ART native registration/dlsym path, most JNI
functions, arrays and buffer pinning, real guest-thread attachment, and
nested cross-ABI callbacks remain unsupported. A native
frame must release its guest local references before returning to ART.

### ELF and execution limits

DT_NEEDED names may be passed to an explicitly supplied dependency callback before import resolution. This only exposes the dependency contract: the caller must provide its own namespace, recursive loading, lifecycle and external rollback. With no callback, a DT_NEEDED image is rejected. Callbacks run only after ELF metadata and relocation ranges have been checked, avoiding external loads for malformed tables. This is **not yet a dependency manager**.

The image loader supports `R_ARM_NONE`, `R_ARM_RELATIVE`, `R_ARM_ABS32`,
`R_ARM_REL32`, `R_ARM_GLOB_DAT` and `R_ARM_JUMP_SLOT`, together with standard ELF32 RELR and Android's earlier RELR tag family. RELR entries are checked for size, range, writable targets, duplicates and bounded expansion before relocation writes. Android APS2 REL groups use checked SLEB128 decoding (including negative offset deltas) and are validated against the same relocation and rollback rules. Neither format establishes full Android dependency or JNI support. It preserves Thumb symbol
bits and rejects unresolved required imports. Weak imports follow the relevant
relocation contract. ELF tables and relocation targets are range checked;
failed loads preserve the previous output image, including allocation and
import-adapter failures.

Dependency namespace/lifecycle, TLS, IFUNC, Android packed RELA relocations, symbol versions,
executable interpreters and preinit arrays are explicitly rejected. The loader
does not claim general Android DSO compatibility. It accepts System V/Linux
OSABI values with ABI version zero and rejects unsupported FDPIC variants.

The standalone `Program` coordinates up to 64 guest dependency objects in dependency-first order and maps each into a reserved guest range. It rejects cyclic dependency graphs; its provider, symbol namespace and host bindings are intentionally explicit. Loaded objects are not Android linker namespaces, and a successful `DT_NEEDED` callback is not proof that an Android dependency is present. A failed load unmaps only objects created by that transaction. Any side effects of a guest constructor cannot be rolled back. The program deliberately has no native threads or nested JNI callbacks.

Image mapping and execution require exclusive ownership of the guest address
space while loading/running. One executor call can be active at a time;
same-executor reentry fails explicitly. Guest threads, nested JNI callbacks,
signals, TLS and system calls require further runtime work. An unimplemented
instruction or supervisor call reports failure. Code changes between calls
invalidate prior translations; instruction synchronization and stopped host
adapters also invalidate the cache. The reserved unmapped return marker is
`0xffff0000`.

## Build and run

The core tests need CMake 3.24+, a C++20 compiler and thread support. They do not
need AOSP, an Android runtime or Dynarmic:

```sh
cmake -S compat/arm32 -B out-arm32-core -DCMAKE_BUILD_TYPE=Release
cmake --build out-arm32-core --parallel 4
ctest --test-dir out-arm32-core --output-on-failure
```

Instruction execution currently requires an x86-64 host; configuration rejects
other CPU backends because their exclusive-read paths have not been adapted or
verified. The core memory, ABI and ELF tests remain independently buildable.
Use the same Dynarmic revision already selected for
Gesher's AArch64 translator:
[`9d4582339990d4eae53f1dc7160686920fc2075c`](https://github.com/yuzu-mirror/dynarmic/tree/9d4582339990d4eae53f1dc7160686920fc2075c).
Its 0BSD license and bundled third-party licenses remain applicable. The source
stays outside this repository and is built with `DYNARMIC_FRONTENDS=A32`.
`compat/arm32/patch_dynarmic.py` verifies the exact source hashes and adds
distinct exclusive-read callbacks to the A32 interface/backend. They let the
adapter fault misaligned LDREX-family loads while preserving ordinary unaligned
loads. All inputs are checked before patching, existing notices stay in place,
and configuration verifies the complete patch. A64 code is unaffected.

```sh
git clone -c core.autocrlf=false https://github.com/yuzu-mirror/dynarmic deps/dynarmic
git -C deps/dynarmic checkout 9d4582339990d4eae53f1dc7160686920fc2075c
python3 compat/arm32/patch_dynarmic.py deps/dynarmic
python3 -m pip install ziglang==0.17.1 pyelftools==0.33
python3 tests/arm32/build_guest_fixture.py --out-dir out-arm32-fixtures

# Supply a directory containing boost/version.hpp (Boost 1.83.0 in CI).
cmake -S compat/arm32 -B out-arm32 -DCMAKE_BUILD_TYPE=Release \
  -DGESHER_ARM32_WITH_DYNARMIC=ON \
  -DDYNARMIC_DIR="$PWD/deps/dynarmic" \
  -DBoost_INCLUDE_DIR=/path/to/boost_1_83_0 \
  -DARM32_GUEST_FIXTURES="$PWD/out-arm32-fixtures/fixture-arm.so;$PWD/out-arm32-fixtures/fixture-thumb.so" \
  -DARM32_GUEST_BIONIC_FIXTURES="$PWD/out-arm32-fixtures/bionic-arm.so;$PWD/out-arm32-fixtures/bionic-thumb.so" \
  -DARM32_GUEST_JNI_FIXTURES="$PWD/out-arm32-fixtures/jni-arm.so;$PWD/out-arm32-fixtures/jni-thumb.so" \
  -DARM32_GUEST_DEPENDENCY_DIR="$PWD/out-arm32-fixtures"
cmake --build out-arm32 --parallel 4
ctest --test-dir out-arm32 --output-on-failure
```

The fixture builder uses Zig's Clang frontend with Android EABI targets,
`-nostdlib` and softfp; its output contains only the explicitly declared test
adapter import. These are real compiler-generated ELF objects, not APKs or a
proof that Android's native runtime works.

The optional independent calling-convention oracle uses production argument
frames with a separate CPU implementation. It compiles varied scalar signatures
and checks computed values instead of merely decoding the same frame algorithm:

```sh
python3 -m pip install ziglang==0.17.1 unicorn==2.1.4 pyelftools==0.33
python3 tests/arm32/abi_oracle.py --cxx g++
```

The oracle currently runs on a Linux host. `ci.yml` adds component builds and
execution on Ubuntu 24.04, Windows 2022 and Windows 2025, with the oracle on
Ubuntu. `ci-required` includes these jobs so failures propagate. A workflow
definition is not evidence of a completed Windows run; inspect the actual run
at the PR head before making that claim.

## Validation and review

The initial infrastructure implementation was independently reviewed for memory/ABI, ELF
loading and execution. Host unit tests cover invalid ranges, allocation
failure, permission violations, malformed ELF input, rollback, JNI descriptor
validation and stale-handle rejection. The independent ABI oracle exercises
110 ARM and 110 verified Thumb calls. The ELF review additionally exercised
100,000 deterministic mutations/truncations under ASan and UBSan.

Review found and corrected missing RELRO protection, allocation exceptions
escaping the loader interface, unsupported FDPIC OSABI acceptance, exclusive
load alignment and cancellation of queued code-cache invalidation. These
specific failures have regression tests. ASan/UBSan results do not include
LeakSanitizer when leak detection is disabled in a ptrace-supervised environment.

The later RELR, APS2, dependency-callback, linked-program, guest libc, JNIEnv32/JavaVM32 and JNI_OnLoad adapter additions have not yet been executed in a verified complete component test run. In particular, the new host C++ JNI bridge has not yet passed a local or CI full compilation/test cycle. The ELF32 guest test source was separately compiled with local Clang 17 and LLD for both ARM and Thumb; `readelf` confirmed actual libc imports, `DT_NEEDED` and cross-library symbol imports plus constructor/finalizer arrays. The pinned Zig guest compiler and Dynarmic executor were not available in that local environment. Clang 17 and LLD also compiled the JNI table-call fixture separately for ARM and Thumb: `readelf` verified ELF32 EM_ARM, EABI5 softfp, exported JNI methods and the Thumb-bit distinction. A local JDK verified the host JNI table slot offsets and CMake's ability to find JNI headers. These checks verify build inputs and fixture shapes only, not complete bridge execution. Their added fixture tests are committed but not yet execution evidence. None of these results substitutes for real Windows/ART application tests.
Existing x86-64/AArch64 production sources, packaging and advertised ABI lists
are unchanged by these standalone components.

## Remaining gates for application support

| Gate | Required behavior and evidence |
| --- | --- |
| Full ELF runtime | Dependency namespaces and lifecycle, Android packed relocations, TLS, dynamic loader services, guest C++ runtime and unwind behavior. |
| JNI32 | Extend the partially implemented guest JNIEnv table to the full JNI API, JavaVM table, methods/fields, arrays and direct buffers, thread attachment, nested callbacks and native registration. The initial guest JNIEnv, opaque references, UTF operations and descriptor-marshaled named calls have isolated fake-host tests but no ART end-to-end proof. |
| ART signatures | Connect the existing descriptor-aware `Jni32::CallNative` adapter to ART's actual `dlsym`, native method resolution and `RegisterNatives` paths without guessing Java signatures. CriticalNative scalar call packing exists only in the standalone adapter. |
| Bionic32 | Correct ILP32 data layouts, allocations, errno, pthread objects, callbacks, variadics, filesystem/network/time/signal APIs and ARM system-call contracts. |
| Android NDK objects | NativeActivity, windows, input queues, graphics/media and other guest-visible structures and callbacks. |
| Lifecycle | Nested guest/host calls, guest threads, TLS teardown, cache/exclusive-monitor coherence and real process shutdown. |
| Packaging and APK admission | Deliver the actual runtime, select/extract the matching base/split libraries, preserve working packages on failed updates, and advertise an ABI only when available. |
| Windows application acceptance | Install and launch ARM32-only APKs that exercise JNI, NativeActivity, dependencies, threads and callbacks, plus x86-64/AArch64 regression tests. |
| Other 32-bit ABIs | IA-32 (`x86`) requires a distinct execution solution. Legacy `armeabi` needs a separately specified compatibility target. |

The Java runtime remains x86-64 unless its process architecture actually
changes. `Dexopt.ISA`, `VMRuntime.is64Bit()` and device ABI identity must not be
spoofed to persuade a particular application to select different native assets.

## Provenance and contracts

The supplied [reference fork](https://github.com/IS-IVR/gesher-bridge/tree/79d4e3542692abb534360a86746481f8bfbb4cfb)
was inspected for architecture and limitations. Its ARM32 branch at that
revision contains an isolated JIT probe and plan, not an application runtime.
No fork commits, patches or source files were imported. This implementation
was written independently against the contracts below and the target's
existing architecture.

- [Android NDK ABI guide](https://developer.android.com/ndk/guides/abis)
- [Arm AAPCS32](https://github.com/ARM-software/abi-aa/blob/main/aapcs32/aapcs32.rst)
- [Arm ELF32 ABI](https://github.com/ARM-software/abi-aa/blob/main/aaelf32/aaelf32.rst)
- [Android Bionic relocation behavior](https://android.googlesource.com/platform/bionic/+/refs/heads/main/linker/linker_relocate.cpp)
- [Android Bionic RELRO protection](https://android.googlesource.com/platform/bionic/+/5e44c22eb/linker/linker_phdr.cpp)
- [Gesher JNI bridge design](decisions/0017-native-libs-jni-bridge.md)
- [Gesher AArch64 translation design](decisions/0047-arm64-translation.md)
