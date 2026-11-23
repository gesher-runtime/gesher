# Commit translated ARM64 TLS according to its assigned extent

## Scope and result

This is the active TLS portion of [#178 NATIVE-02](https://github.com/gesher-runtime/gesher/issues/178), supporting [#56 N08/T12](https://github.com/gesher-runtime/gesher/issues/56). The translated ARM64 owner previously kept its entire 256 KiB TLS reservation committed while attached, even with a small module set. It now retains the page-rounded prefix covering the TCB and **every pending or published reservation**. Guest addresses, offsets, alignment, module ownership and the 256 KiB capacity remain the same.

For the reproducible component workload below, a 64-byte TCB and 8 KiB TLS module require 12 KiB of page-backed storage per context, compared with 256 KiB before. At 128 contexts that is **32 MiB to 1.5 MiB, a 95.3125% reduction in the active TLS page range**. The local measurement uses an explicit POSIX VM boundary. Actual Windows commitment is a required pending CI measurement, not a result inferred from Linux.

Construction and `Unpark` still transiently commit all 256 KiB before `Attach` clears historical bytes and installs images. The 8 MiB translated stack is unchanged. This leaf does not complete NATIVE-02's stack-growth work, N08's dynamic TLSDESC/module growth, native Windows ARM64 support, or the real-app CPU/RAM/latency budget in T12. There is no RSS or application startup claim.

## Source and contract

Baseline is main `d38b5549dc3963f8d491e9205f9b2743bb6d3745`:

| Production file | Before SHA-256 | After SHA-256 |
| --- | --- | --- |
| `compat/arm64/dll/translator.cc` | `eef6fbc4b3cff2f1421f026be993501bd76f1f88d43b1aa992c9571596b6dd2e` | `8d442347e1a1c0b257cb19d2d8e68a398852ca137a8b6b6b399b07bb92b4ade8` |

The public header only clarifies that registration can fail when backing storage cannot be committed; its function table and ABI do not change. [The existing ELF caller](https://github.com/gesher-runtime/gesher/blob/d38b5549dc3963f8d491e9205f9b2743bb6d3745/compat/elf/elf_loader.cc) already rejects a negative registration result before using the offset. Other C++ allocation failure behavior is unchanged.

The [Android ELF TLS design](https://android.googlesource.com/platform/bionic/+/b96d8213d8d3fc4efa483312fdf9e7bd68a64ee2/docs/elf-tls.md) describes static TLS accesses as offsets from each thread's own pointer. This change preserves that property; it does not claim Android's dynamic TLS implementation. The preservation reference for Gesher's current pending-loader preparation/publication/rollback behavior is the unchanged baseline owner and its existing tests.

[Microsoft's VirtualAlloc contract](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc) distinguishes reservation, commitment and physical residency. A reserved range can be committed by page, committing an already committed page is allowed, and commitment does not establish that every page is resident. [VirtualFree](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualfree) operates on every page intersecting a decommit range; reserved pages must not be accessed. Therefore the new boundaries are rounded using `GetSystemInfo().dwPageSize`, and only whole pages above the highest live reservation are reclaimed.

## Ownership and failure ordering

`TlsBlock` records the actual contiguous committed prefix under the existing `g_tls_mutex`. Its default is the full reservation because construction and `Unpark` already establish that precondition. After `Attach` completes historical clearing and image initialization, it trims the unused tail. The block stays registered for sleeping threads as before; `Detach` removes it before `Park` can release pages.

`ApiTlsRegister` first validates size, alignment and available gaps. It then commits the entire required range in **all attached blocks before publishing the new token, module record or high-water extent**. This ordering matters because `Prepare` and `Publish` do not expose an allocation-failure result. A loader can prepare a module, execute nested constructors and publish to another sleeping thread without encountering a newly uncommitted destination.

A failed `VirtualAlloc` returns `-1` before registration becomes visible. Earlier successful block growth is trimmed back to the previous live extent. If that cleanup fails, the block keeps both its usable pages and the accurate larger prefix record; a later retry reuses them. Existing module bytes remain valid throughout.

Rollback clears the abandoned interval before erasing it, computes the maximum extent across the remaining pending and published modules, and trims only the tail beyond that extent. An interior gap or a page also containing a nested committed module remains committed. Failed decommit never changes the recorded prefix. No module offset moves, and zero-sized pending modules still receive distinct one-byte reservations.

This adds one decommit attempt per successful attachment when there is unused tail. A later module which crosses the current page boundary adds one commit call per live block that needs growth. Reusing an interior gap makes no redundant commitment call. Guest calls after attachment gain no per-call lookup or VM operation. Each live TLS registry entry grows from 16 to 24 bytes on the x64 Windows ABI. The change exchanges those small metadata and lifecycle costs for lower steady TLS commitment; CPU/latency tradeoffs remain unmeasured.

## Before/after resource measurement

Both versions use the same fixture and actual extracted production methods. Local runs use GCC 14.2, ASan/UBSan and a POSIX mapping/profiling boundary on x86-64 Linux. Its `mmap`/`mprotect`/`madvise` operations make invalid page accesses fail, and its query adapter reports the mapped page state. It is **not** Windows `VirtualQuery`, Windows commit charge, a JIT or an Android process. The footprint population contains distinct `Env` objects with the same owner; separate actual host threads exercise foreign/sleeping ownership in the contract corpus.

| Active contexts | Assigned bytes per context | Reserved TLS bytes, both | Baseline active page range | Candidate active page range | Candidate attachment trims |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 8,256 | 262,144 | 262,144 | 12,288 | 1 |
| 32 | 8,256 | 8,388,608 | 8,388,608 | 393,216 | 32 |
| 128 | 8,256 | 33,554,432 | 33,554,432 | 1,572,864 | 128 |

Transient TLS commitment during initialization equals the reserved column in both versions. Parking remains zero committed TLS pages. These are deterministic resource counts, so no statistical timing improvement is claimed. Savings decrease as the assigned module extent approaches 256 KiB; at full capacity the existing commitment is required. The full-capacity registration/rollback path is included in the contracts.

[Raw output, source hashes, commands and environment](arm64-active-tls-2026-11-23.json) record baseline and candidate separately. Uncollected CPU, latency and resident-set values are `null`, not zero.

## Validation and negative controls

The final local candidate passes **29,229 checks**. Baseline measurement mode passes **16,437** preserved lifecycle checks, while baseline gate mode fails the intended active-prefix budget assertion. The different totals include the candidate-only failure-path corpus and per-region VM query assertions; they are not a comparison of feature coverage.

The component fixture includes:

- Initial trimming and decommit failure, exact bookkeeping, distinct contexts with the same thread owner, TCB preservation, private preparation and per-owner constructor mutations.
- A real sleeping host thread whose entire module range is committed before an offset escapes; its initial image and BSS appear only on publication.
- Nested committed modules sharing a page with an abandoned outer reservation, aligned interior reuse, distinct empty tokens, invalid alignment and exhaustion.
- Allocation failure on each of three live blocks, full-tail growth, rollback of earlier successful commits, failed cleanup, and subsequent retry using retained commitment.
- Detached/parked exclusion, full `Unpark` commitment before historical clearing, image restoration, handle/registry retirement and reservation reuse.

Eight independently built source mutants must each exit through a specifically named assertion; a crash, compiler error or timeout is rejected as inadequate evidence. They omit attachment trimming, skip foreign-thread growth, commit only one page, publish after allocation failure, discard failed-decommit bookkeeping, omit rollback trimming, reclaim an overlapping page, or omit partial-growth cleanup. All eight are rejected.

The existing pending-load rollback corpus and unchanged native ARM64 rollback corpus pass under ASan/UBSan. The shared pending fixture previously used stack arrays to represent live TLS. It now owns real mappings, and its assertions that inspect retired bytes explicitly deny decommit so they still observe the production `memset`, rather than OS zeroing. Its reused `Attach` seam now reproduces the full recommit performed by real `Unpark`. The new lifecycle corpus separately requires successful page reclamation; these boundaries do not replace that gate.

The complete production `translator.cc` translation unit compiles for Windows x64 with the pinned `llvm-mingw-20261006` compiler and actual Dynarmic `9d4582339990d4eae53f1dc7160686920fc2075c` headers after the repository's existing verified FPSR/exclusive-monitor patch. Both current and baseline standalone Windows probes compile and link. The local environment cannot execute them or the actual translator DLL.

The selected-source policy remains unchanged and passes 26 mutation controls plus four repinned property controls. Supplier policy passes 36 semantic mutants plus its authority negative. No runtime approval, source qualification, compiler role or supplier gate is relaxed.

## Required Windows/full-CI evidence

`ci.yml` adds the host sanitizer/negative-control run. Its Windows native shard runs the same compiled probe against pinned baseline `d38b5549` in measurement mode, then against the current source in gate mode with the actual `gesher_arm64.dll`. The probe uses real `VirtualQuery` on Windows. That gate retains existing parking, stack publication, nested callback, errno and reservation-reuse checks, and adds real translated nested constructor preparation/publication/outer rollback while another reservation stays parked. A replacement thread must receive the new nested image independently of the former owner's constructor mutation.

The reusable framework pin changes from unpublished `06d175a8` to published `6afcb6e6`; their complete `framework.yml` blob is identically `834692396e307a6cd9251c527b7a91ab402d13ac`. This is the existing [#346 startup repair](https://github.com/gesher-runtime/gesher/pull/346), included so the required workflow can start. Workflow recipe, runner route, permissions and assertions remain intact.

**Actual Windows, translated DLL execution and the complete fork CI result are pending at submission.** Linux boundary counts and PE crosslinks do not satisfy them. Main's large-frame, guard, longjmp/unwind, ELF loading and ABI suites remain required in the complete workflow. Any runtime failure must be fixed without weakening those tests.

## Reproduction

```sh
python3 tools/test_arm64_tls_lifecycle.py --host-boundary --sanitize --negative-controls
python3 tools/test_arm64_tls_lifecycle.py --host-boundary --sanitize \
  --source-ref d38b5549dc3963f8d491e9205f9b2743bb6d3745 --measure
python3 tools/test_arm64_pending_rollback.py --sanitize
python3 tools/test_arm64_native_rollback.py --sanitize
python3 tools/net-tcp-priority/test-selected-policy.py --selftest
python3 tools/net-tcp-priority/test-selected-supplier.py
```

This constrained local session requires `ASAN_OPTIONS=detect_leaks=0` because LeakSanitizer cannot run under its tracing environment; the committed CI command keeps the normal sanitizer defaults. No Windows or production sanitizer setting changes.

On actual Windows, with the current translator DLL and pinned baseline probe built by CI:

```powershell
./arm64_tls_lifecycle_baseline.exe measure
./arm64_tls_lifecycle_test.exe gate ./gesher_arm64.dll
```
