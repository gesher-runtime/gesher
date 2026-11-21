# Native callback cleanup qualification

This is an isolated prerequisite for issue #56 N15. It adds no production
nonlocal transfer, guest instruction, libc substitution or application-visible
behavior. The fixture links the exact production `thunks.S`; its native callback
is a test function, not a translated ARM64 program. Passing this gate would
qualify one pinned native cleanup mechanism and its explicit limits. It would
not establish that Gesher supports `longjmp` across a native callback.

The assembly unwind prerequisite must be present first. The qualification
increment does not change production thunks or translator behavior. The reviewed
branch also includes the separately approved call-record ABI prerequisite from
PR131, as described below. Existing full CI remains unchanged by this increment.

## Why a native proof is necessary

An outer guest jump from a native comparator must abandon the intervening native
call. Restoring guest registers in a nested Jit does not remove that native
frame. Host wrappers also own resources and translator state; a native checkpoint
that skips their C++ destructors is insufficient.

The pinned llvm-mingw release is `20261006`, clang 23.1.3, LLVM commit
[`0d261d1ca552c95a8f007e061c787ac7132fbcbc`](https://github.com/llvm/llvm-project/tree/0d261d1ca552c95a8f007e061c787ac7132fbcbc).
Its [SEH unwinder](https://github.com/llvm/llvm-project/blob/0d261d1ca552c95a8f007e061c787ac7132fbcbc/libunwind/src/Unwind-seh.cpp)
provides `_Unwind_ForcedUnwind`, invokes a stop callback before the frame's
personality, and resumes the forced walk from cleanup landing pads through
`_Unwind_Resume`. The stop may reach an owner before unwinding its still-live
scopes. Returning an error from that stop does not provide a resumable
destination; the fixture restores its checkpoint only after verifying cleanup.

The same source explicitly declines C++ cleanup for an unrecognized foreign
Windows exception in `_GCC_specific_handler`. A foreign exception passed to
`RtlUnwindEx` is therefore not interchangeable with the LLVM cleanup path. Native
unwind metadata is necessary, but does not itself prove language cleanup.

The [pinned libc++abi personality](https://github.com/llvm/llvm-project/blob/0d261d1ca552c95a8f007e061c787ac7132fbcbc/libcxxabi/src/cxa_personality.cpp)
allows `catch (...)` to match a forced foreign exception. This mechanism is not
catch-proof. `noexcept` and foreign language handlers require characterization;
no result here authorizes silently crossing Java, CLR, MSVC C++ or arbitrary
third-party frames. No LLVM implementation was copied into this fixture.

## Actual native path and resources

`RunOwner` owns a test checkpoint, a unique lifetime token, a registration guard,
and real Win32/heap resources. It calls an adjacent helper DLL, a real C callback
frame and native C++ wrappers. These call the exact production
`gesher_call_host_sysv` and `gesher_guest_entry_shim` assembly. A leaf test entry
stub and test trampoline select the native callback. Nine System V integer
arguments exercise the host thunk's dynamic stack-argument allocation.

The EXE and helper DLL each link a separate static copy of the pinned libunwind
and libc++abi, matching the production DLL's `-static` link choice. Link maps and
defined symbols establish ownership at build time. At execution, the probe
requires different `_Unwind_ForcedUnwind` addresses belonging to the actual EXE
and DLL modules. DLL cleanup must resume the same forced transfer through its
own runtime and reach the EXE owner.

This release's static `libc++.a` contains the libc++abi personality objects.
The verbose linker receipts identify that actual archive/member, rather than
assuming the standalone `libc++abi.a` is the linked input. Both runtimes and
the combined C++ archive are authenticated against the official toolchain.

Each C++ resource guard creates a Win32 event and a heap allocation. Its
destructor closes/frees them and records strictly ordered, once-only cleanup.
Intermediate native continuations have separate counters. An owner may land
only after abandoned guards and nested owner registrations have retired, with
exactly one exception deletion and without destroying the target's live scopes.

The original test checkpoint stores Microsoft x64 preserved GPRs, full
XMM6–XMM15, stack position and continuation PC. It deliberately does not restore
historical FP control/status. The save declaration is `returns_twice`; the
restore leaf does not invoke CRT `longjmp` or initiate a second unwind. This is
a pinned platform/compiler-specific experiment, not portable C++ semantics.

## Destination and lifetime checks

The supported process executes one normal warm-up, 16 repetitions of four
scenarios, and those four scenarios on each of four native threads: **81 cases**.
The scenarios are ordinary return, escape to a live owner, nested escape to the
outer owner, and escape to the nearest inner owner followed by normal outer
return. It checks results, abandoned continuations, destination selection,
cleanup order/counts and the process handle count after repetition/thread exit.

A token contains a fixed owner-slot address, unique generation and owning
thread. Resolution checks only the current live chain. Retired/reused tokens
and an active token on another thread are refused **before any transfer**. The
fixture never jumps to a dead frame or another thread; those refusals do not
assign semantics to undefined guest programs.

The owner CFA and function region come from the actual LLVM native backtrace
and `RtlLookupFunctionEntry`. Landing requires both to match, the token to remain
live, and the CFA to equal the saved stack position. There is no fake unwinder
or simulated destructor callback.

## Boundary and rejected-design processes

Each row runs in a separate bounded native process. A crash, timeout, generic
invariant failure or wrong exit code fails the gate. The specific diagnostic is
required too. These are **expected results until native run receipts confirm
them**.

| Mode | Required result | Interpretation |
| --- | --- | --- |
| default | exit 0, 81 cases | Native C/LLVM cleanup chain and live destinations work. |
| `--foreign-normal` | exit 0, one case | Actual SEH `__finally` runs on ordinary return. |
| `--catch-all` | exit 71, interception diagnostic | A real catch-all intercepts the transfer; owner landing is not reached. |
| `--noexcept` | exit 72, terminate diagnostic | The actual noexcept boundary terminates instead of landing. |
| `--foreign-finally` | exit 73, missing-finally diagnostic | A foreign SEH resource is still open at the owner; landing is refused. |
| `--raw-rejected` | exit 74, skipped-cleanup diagnostic | Deliberately rejected raw native jump skips owned C++ cleanup. |

The raw-jump process exits immediately after observing missing cleanup. Skipping
nontrivial C++ destructors with ordinary `longjmp` is outside the defined C++
contract. This rejected-design experiment is not a portable semantic baseline.
The positive cases instead execute the pinned native unwinder's cleanup path.

## Reproduction and native evidence

```sh
python3 tools/arm64/build-cleanup.py \
  --toolchain /path/to/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64 \
  --archive /path/to/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64.tar.xz \
  --out /empty/output/directory
```

The recipe verifies the official archive SHA256 and selected installed tools
and runtime archives against that archive. It separately pins runtime/header
hashes, builds with warnings as errors, and checks actual `.xdata`, personalities,
defined static symbols and link maps. Receipts record source commit/tree/file
hashes, compiler identity, commands, objects and binary hashes. Cross-compilation
does not run or prove cleanup.

```powershell
tools/arm64/run-cleanup.ps1 -Dir C:\path\to\native-callback-cleanup
```

The focused workflow builds the clean checkout and consumes that exact artifact
on Windows 2022 and Windows 2025. It checks source/output hashes,
source/run/attempt/repository identity, artifact ID/digest and native architecture.
Each process has a 30 s deadline. Raw logs and run receipts survive failed
outcomes. Independent review must assess actual native evidence before treating
the primitive as qualified.

## Separate historical Gesher baseline (removed)

An earlier revision of this PR also collected a historical characterization of
the 80-case same-thread callback corpus (`callback_longjmp_contract.c`) against
the pinned PR #120 artifacts of run `44162241853`, through
`prepare-callback-baseline.py` and `run-callback-baseline.ps1`. Those artifacts
expire, so the jobs would start failing for reasons unrelated to any change;
the workflow jobs were removed before merge. The two scripts and fixture remain
as manual historical tools. That collection was never a callback semantic pass.

Production translator cleanup, correct Jit owner selection, guest masks/FP
state, allowed boundary discovery and an actual translated corpus pass remain
future work even if every native qualification case passes.

## Shared production call record

Integration QA found that the original fixture copied the former 56-byte
`HostCall` layout. PR131 extends the production record to 64 bytes and writes
`out_xmm0_hi` at byte offset 56 on normal return. A text-clean merge would leave
that copied test record too small for the actual assembly it links.

The consolidated branch preserves the reviewed PR131 history and includes
`compat/arm64/dll/host_call.h` directly in the fixture. Its shared static layout
assertions therefore apply to the test as well as the translator. The fixture
initializes the complete current record and no longer owns a parallel layout.
The native cleanup control flow and `EntryFrame` contract are unchanged.

The source receipt hashes the shared header, both workflow path filters include
it, and the native receiver rejects a receipt that omits it. The receiver also
checks its exact source hash through the existing source-verification loop.
Historical Windows results qualify their original source; a fresh native run
must qualify this integrated fixture and its current production thunks.
