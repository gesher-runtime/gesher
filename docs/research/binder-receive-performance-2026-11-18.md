# Binder delivery without payload-sized snapshots — 2026-11-18

## Scope and contract

Baseline: upstream `f93a90fac55dbe1aff641e6e3f26879292907cb2`.

`WriteRead` owns the broker's complete reply in a local byte vector. Previously,
each returned transaction was copied from that reply into three additional
vectors: data, offsets and scatter/gather extra bytes. After validation, those
vectors were copied again into the receiver's final transaction allocation.

The receiver now validates byte views into its immutable reply and copies each
region directly into the final allocation. Offset entries are read with
`memcpy`, because a wire table may follow an odd-sized data region and need not
be aligned to `uint64_t`. Existing vector callers use the same validator through
an overload; the broker and sender retain their owned storage.

Application inputs, transaction bytes, command order, identity fields and FD
ownership are preserved. Dimensions and the ordered object/parent layout are
validated before the mapped allocation. The subsequent sequence remains:
allocate, copy, rebase, claim, adopt distinct local FD copies, publish, finish.
Every prior error/abandon path and the caller-owned published prefix remain.
No cache or additional retained storage is introduced. Safe snapshots of
mutable application pointers on the **send** path are unchanged.

The reference contracts are the
[Android 16 Binder UAPI](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/kernel/uapi/linux/android/binder.h),
the existing [scatter/gather contract and ownership note](../decisions/0068-binder2-scatter-gather.md),
and the existing [wire format](../../compat/posix/src/binder_wire.h).
This private storage change does not broaden Gesher's Binder capability claims
or close the remaining transport/compatibility work listed in those documents.

## Measurements

The host fixture executes the production `WriteRead`, byte readers, SG
validator/rebaser, mapped allocator, FD adoption/cleanup and guarded publication
code. Deterministic adapters stand in for named pipes, Win32 process-memory
operations, OS handle duplication/adoption, and thread-connection selection.
The fixture builds the peer's response and receiver mapping before accounting.

The plain case returns 4,194,304 data bytes. The SG case returns 129 data bytes,
16 offset bytes and 4,194,320 extra bytes, including a parent pointer fixup.
Each measurement is one complete synchronous receive. C++ allocation counts
include the owned reply, command/protocol buffers and allocator/SG metadata.

| Measurement | Exact baseline | Updated |
| --- | ---: | ---: |
| Plain: allocation count | 19 | 18 |
| Plain: cumulative temporary allocation bytes | 8,389,116 | 4,194,812 |
| Plain: peak tracked temporary bytes | 8,389,040 | 4,194,736 |
| Plain: explicit production `memcpy` bytes | 8,388,788 | 4,194,484 |
| SG: allocation count | 24 | 21 |
| SG: cumulative temporary allocation bytes | 8,389,670 | 4,195,205 |
| SG: peak tracked temporary bytes | 8,389,506 | 4,195,041 |
| SG: explicit production `memcpy` bytes | 8,389,350 | 4,194,901 |

The plain delivery avoids exactly one 4 MiB temporary allocation and one 4 MiB
payload copy. SG avoids its three intermediate vectors; the new unaligned-safe
validator performs an additional eight-byte scalar copy for each offset.
Small protocol/validation allocations and the final receiver allocation remain.

These are temporary allocation and explicit-copy counts at the stated host
boundary. They exclude the preallocated receiver mapping, adapter copies,
Windows pipe/kernel allocation, allocator overhead, and application-side work.
They do not measure process RSS, IPC latency, throughput, ART startup or
application-wide CPU speed. The original reply still occupies memory until
`WriteRead` returns; the change removes the redundant in-flight snapshots.

## Regression evidence

- The new production-source host suite passes **5,640 checks** with ASan/UBSan.
  It covers plain transactions and replies, all eight wire-offset alignments,
  nested pointer fixups, odd and empty payloads, byte identity, partial pipe
  I/O, and preserved sender snapshots.
- Malformed dimensions, every truncated response length in the SG fixture,
  unsupported/misaligned/overlapping/out-of-range objects, invalid parent and
  extra-buffer fields fail before publication. A mapped-capacity failure is
  rejected before claim or FD adoption.
- Handle duplication/adoption failure, claim failure/lost reply, output
  permission/copy failure, and finish failure/lost reply retain the prior
  rollback and published-prefix behavior. Adopted handles close exactly once;
  staging handles are never treated as receiver-owned.
- Multiple outstanding returned buffers survive destruction/replacement of the
  transport reply and later calls. Mixed command prefixes, claim-time reentry,
  and 400 concurrent deliveries through the real mapped allocator retain their
  independent payloads and broker IDs.
- The exact baseline passes all **5,634** behavior/measurement checks with
  resource budgets omitted. With the six new budgets enabled, that same
  baseline fails **exactly six checks**: cumulative temporary bytes, peak
  temporary bytes, and copied bytes for each workload.
- An independent reviewer reproduced the new sanitizer gate and ran **310,433
  differential checks** over 200,000 seeded valid/malformed layouts using the
  old vector validator and the new raw-byte validator. Pointer metadata and
  rebased bytes agree, including unaligned table addresses. That reviewer
  control is a separate scratch harness, not a committed CI test.
- The existing Binder reply framing sanitizer suite passes **149 checks**.
- Both complete production translation units, `binder.cc` and `binderd.cc`,
  cross-compile for Windows **x64 and ARM64** with the CI-pinned LLVM-MinGW
  `20261006`, `-Wall -Wextra -Werror` and `-fno-exceptions`. The compile uses the
  pinned Android 16 Binder/ioctl UAPI headers and Gesher's existing fixed-width
  Linux types. The Binder UAPI header SHA256 is
  `ccdccf5df0f7aa3006b465fc774738fab7b351fcde5025e035ce569237fceca7`.
- The new sanitizer gate is included alongside the existing Binder framing
  gate in mandatory CI preflight. `git diff --check` passes.

The baseline sanitizer run stops at the old empty-snapshot path's
`memcpy(nullptr, ..., 0)` reported by UBSan. The unchanged non-sanitized
baseline provides the behavior and allocation negative controls above. The
new source passes the empty-payload case with both sanitizers enabled; no
assertion or sanitizer check was relaxed for the baseline. Local runs disable
only LeakSanitizer process inspection (`ASAN_OPTIONS=detect_leaks=0`) because
the execution environment uses ptrace; ASan/UBSan remain fatal.

The current production SHA256 values reviewed and tested are:

| Source | SHA256 |
| --- | --- |
| `compat/posix/src/binder.cc` | `277f452917b33fdba22b374aa0aaece43c638c591df6cf99fd816c348454c83b` |
| `compat/posix/src/binder_sg.h` | `883b14cbe7baf8f4e71137cc572eeaca166c092880e600864a8eb4526ca8330d` |

Fresh Windows named-pipe/process/handle execution, Windows ART/APKs and the
complete current-head Binder integration suites remain platform validation
gates. Cross-compilation and host adapters do not establish those outcomes.

## Reproduction

From the repository root:

```sh
python3 tools/test_binder_receive_views.py --sanitize
python3 tools/test_binder_receive_views.py --measure \
  --source-ref f93a90fac55dbe1aff641e6e3f26879292907cb2
# Expected exit 1, with exactly the six new resource budgets failing:
python3 tools/test_binder_receive_views.py \
  --source-ref f93a90fac55dbe1aff641e6e3f26879292907cb2
python3 tools/test_binder_reply_allocations.py --sanitize
git diff --check
```

On a ptrace-restricted host, prefix the sanitizer commands with
`ASAN_OPTIONS=detect_leaks=0`. CI keeps its ordinary sanitizer defaults.

For full-source cross-compilation, point `BINDER_MINGW` to the pinned toolchain
and `BINDER_UAPI` to the pinned UAPI include tree with its architecture ioctl
forwarding header:

```sh
for target in x86_64 aarch64; do
  for source in compat/posix/src/binder.cc compat/binder/binderd.cc; do
    "$BINDER_MINGW/bin/$target-w64-mingw32-clang++" \
      -std=c++20 -Wall -Wextra -Werror -fno-exceptions \
      -DGESHER -DUSE_MINGW -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DNOGDI \
      -D_WIN32_WINNT=0x0A00 -Icompat/posix/include -idirafter "$BINDER_UAPI" \
      -c "$source" -o "/tmp/$(basename "$source" .cc)-$target.o"
  done
done
```
