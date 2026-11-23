# Binder output construction in the connection's reply

This is a measured component of [#56 T12](https://github.com/gesher-runtime/gesher/issues/56)
and the copy-reduction portion of [#178 NATIVE-06](https://github.com/gesher-runtime/gesher/issues/178).
It does not complete either row. Broker lock contention, connection workers,
aggregate admission and queue budgets, retained input capacity, and claim/finish
round trips remain separate work.

The broker currently serializes a transaction into `ReadStream::out`, then copies
that complete command stream into the connection's `WireReply`. The latter
already owns the length prefix and is the storage ultimately passed to
`WriteMessage`. The existing prefix improvement in `eb652ece90f327622d1971867ab1730ae71ea786`
removed a different copy at the send boundary; this change removes the earlier
copy between two output vectors.

`ReadStream` now borrows that connection-local reply. Its `command_start` records
the first command position after the wire status and consumed-command fields.
The spawn-looper replacement uses this position. Android read-buffer accounting
continues to use the separate `ReadStream::size` and `limit`; the wire header does
not consume that budget. The final `ThreadRead` status replaces the reserved
status field before the completed reply is sent.

The reference stays inside the synchronous dispatch, including a blocking read's
condition-variable wait. It is not stored in a thread, transaction, buffer or
queue. The existing lock boundaries, pending-buffer identity, claim/finish
validation, object translation and FD ownership are unchanged. Each ordinary
reply still dies at the end of its connection-loop iteration.

## Separate correctness prerequisite

The untouched main revision is `5aa6b39027e9c38de2781ff9cda066418d25b1ba`.
The full actual-core sanitizer fixture exposed an existing empty-transaction
failure: `In::Read` calls `memcpy` with a null zero-length vector destination.
The original failure is preserved in the raw report.

The separate prerequisite commit
[`95eb90b9ac81c5dee387d6685e7670ccaf19f44f`](https://github.com/yom-chadash/gesher/commit/95eb90b9ac81c5dee387d6685e7670ccaf19f44f)
adds only `if (n == 0) return true;` before pointer subtraction and copying.
It leaves nonempty bounds and cursor advancement unchanged. Tests cover a null
empty view, a zero-length near-end read, a rejected nonempty overrun, a successful
last-byte read and a subsequent end-of-input rejection.

Both performance variants contain this same prerequisite. The measured local
baseline `0523c7789e74d41eea399a8f01884489f440dcac` and the public prerequisite
have the exact same tree, `afb8846dd01556d7af2ef7884a16fe3b0f00b728`, and the same
sole main parent. Both remote tree and parent were verified. The raw build
receipts retain the original local reference instead of rewriting that provenance.
The performance result is therefore not attributed to fixing the empty copy.

## Measured broker-core CPU

The interval includes actual `ThreadWrite`, transaction parsing and buffer
allocation, object/layout validation, sender completion, receiver `ThreadRead`,
complete reply construction, actual claim/finish dispatch, `BC_FREE_BUFFER` and
reply destruction. It uses one thread calling those production functions through
the exact extracted dispatch branches with their real mutex. It excludes pipe
I/O, process admission, concurrent callers, Windows scheduling, the guest receive
adapter and ART. These are broker-core component measurements.

Both variants use GCC 14.2, `-O2`, the same fixture and an Intel Xeon Platinum
8573C host. Timed builds compile out allocation/copy instrumentation and have no
sanitizers. Each process is pinned to the same allowed CPU. Four ABBA groups give
eight fresh processes per revision. Each workload in each process has two
declared warm-up samples followed by six steady samples: **all 768 observations
are retained**, including 192 warm-ups, and the tables use all 576 steady samples.
There is no outlier exclusion. This protocol does not establish cold-start,
multi-caller contention or application latency equivalence.

| Workload | Operations per sample | Median CPU before / after, ms | Change | Median wall before / after, ms |
| --- | ---: | ---: | ---: | ---: |
| 64-byte transaction | 50,000 | 37.4285 / 32.5710 | −12.98% | 37.4287 / 32.5821 |
| 4 KiB transaction | 20,000 | 18.7470 / 16.8250 | −10.25% | 18.7512 / 16.8256 |
| 64 KiB transaction | 4,000 | 28.0215 / 21.3975 | −23.64% | 28.0216 / 21.3981 |
| 1 MiB transaction | 300 | 336.4885 / 222.8330 | −33.78% | 337.0990 / 222.8912 |
| SG with a 256 KiB child buffer | 1,200 | 30.7025 / 22.7570 | −25.88% | 30.7026 / 22.7578 |
| Empty command, no read | 500,000 | 21.4120 / 21.1150 | −1.39% | 21.4166 / 21.1155 |

The SG workload also contains a 32-byte parent buffer, two actual buffer objects
and their offset array. The empty control takes the same dispatch without a
transaction or read; its small timing difference is treated as a control, not a
material improvement. Every measured transfer checks framing, status, consumed
count, origin, code and complete payload length; the separate correctness corpus
compares all output bytes and ownership transitions.

There is no claim of Windows IPC speed, application startup/frame performance,
power savings or reduced whole-process RSS/commit. The raw report records every
process command, source/generated-source hash, CPU/wall sample, checksum,
compiler, CPU model and affinity so that the summaries can be recomputed.

## Actual output construction accounting

Allocation accounting surrounds only receiver output construction after the real
input buffer is queued. It records requested C++ allocation sizes and their live
peak. It excludes the already-owned transaction buffer, allocator headers,
allocator residency, other threads and transport. The counted fixture also
observes the bytes submitted to the actual `Out::Write`. These include scalar
wire fields; vector relocation copies, the prefix's initial resize, and the final
four-byte status overwrite are not `Out::Write` calls. Counted/sanitized timings
are retained as diagnostics and are not used for the CPU table.

| Output | Allocations before / after | Requested bytes before / after | Live requested peak before / after | `Out::Write` bytes before / after |
| --- | ---: | ---: | ---: | ---: |
| 64 bytes | 7 / 3 | 612 / 284 | 384 / 252 | 320 / 164 |
| 4 KiB | 7 / 4 | 8,648 / 4,484 | 8,420 / 4,368 | 8,384 / 4,196 |
| 64 KiB | 7 / 4 | 131,528 / 65,924 | 131,300 / 65,808 | 131,264 / 65,636 |
| 1 MiB | 7 / 4 | 2,097,608 / 1,048,964 | 2,097,380 / 1,048,848 | 2,097,344 / 1,048,676 |
| SG with a 256 KiB child | 9 / 5 | 525,560 / 262,876 | 524,788 / 262,592 | 524,752 / 262,380 |

All counted output allocations are retired after each request. The gate requires
one serialized body pass through `Out::Write`, with no complete nested-stream
recopy. It applies no timing or allocator-capacity threshold. The same prerequisite
baseline passes the full reference corpus but fails this copy budget, as intended.

## Correctness, failures and ownership

The fixture compiles the actual broker core, real `ServeThread`, framing and
dispatch from the selected production source. A single generated observational
call in `Out::Write` supplies byte accounting. A second direct wrapper executes
the same extracted write/read and claim/finish branches for measurements.

The wire corpus uses actual byte streams and real OS resources. Linux provides
an explicit Windows-I/O boundary using `socketpair`, self-process `dup`/`close`
and real pipe resources. Windows uses actual byte-mode named pipes, real events
and `DuplicateHandle`. The fixture admits two broker process records inside its
own OS process and begins at `ServeThread`; it does not claim to test separate
OS-process authentication or the outer process-admission protocol. Existing
cross-process Binder/SG/OFD gates remain required.

Coverage includes:

- Empty, one-byte, 63-byte, 4 KiB, 64 KiB and 1 MiB payloads, complete SG data,
  every offset and descriptor side-table byte, original sender identity and flags.
- Exact status/consumed fields after successful, malformed and rejected input;
  a real nonblocking `EAGAIN`; one-byte-short versus exact Android read limits;
  and `BR_SPAWN_LOOPER` replacing the first command without changing the header.
- Synchronous calls and replies, a real translated callback and nested
  synchronous call, node acknowledgments, nested publication failure and correct
  restoration of the original outer reply chain. Earlier output remains intact
  after subsequent requests and buffer retirement.
- Rejected finish before claim, foreign claims and duplicate finishes; oneway publication failure
  releasing the next queued request; actual copy/layout failures creating no
  receiver buffer; pending output blocking later write commands.
- Actual FD and shared-description staging handles, successful independent
  receiver adoption, rejected publication, duplicate/late finish, normal buffer
  free and original/adopted resources remaining valid for their owners.
- Repeated real short writes and a real partial wire output followed by an
  injected write error. An incomplete response is rejected, the pending thread
  and buffer retire, staging handles close and original sender handles survive.

The candidate passes **1,613 ASan/UBSan checks**. The prerequisite baseline passes
**1,607 checks** with only the six direct-output work-budget assertions disabled.
An independent reviewer replayed the complete candidate corpus and all six
assertion-specific mutants and inspected both the production delta and the
separate prerequisite.

| Mutant | Required assertion failure |
| --- | --- |
| Put command start at offset zero | Spawn preserves status/consumed |
| Omit final read-status update | Real read failure updates reserved status |
| Replace consumed count with zero | One actual command is consumed once |
| Charge the wire header to the Android read limit | Exact client read budget delivers work |
| Reject zero-byte input reads | Null empty view succeeds without access/advance |
| Copy the whole output into another vector | One serialized body pass, no nested recopy |

Each mutant must fail its named assertion. An unrelated compiler failure,
sanitizer failure or process crash is not accepted as that negative control.
The existing reply framing/sending corpus also passes **149 checks**.

Local LeakSanitizer cannot operate under this executor's ptrace environment and
was disabled. ASan and UBSan remain active. CI retains its normal sanitizer
configuration; no local leak-test result is claimed.

## Source qualification and Windows gates

The original OFD manifest, its 71 source digests and original transition patch
are unchanged. A separate exact single-file transition pins the complete new
`binderd.cc`, reverses it to main's original digest, verifies those exact original
bytes, and reconstructs the current file forward. It covers both the prerequisite
and the output change without excluding an arbitrary function or region.
Its `native_runtime_approval` remains explicitly false.

Source policy passes 33 mutation controls, four independently repinned property
controls, and six independently repinned Binder controls. Refreshing a source
or manifest hash cannot authorize a different command offset, read status,
empty-copy rule, claim predicate or runtime approval. The unchanged selected
supplier checks pass 36 semantic mutants plus the authority-hash negative.

The complete production broker links for Windows x64 and ARM64 with the pinned
LLVM-MinGW compiler, strict warnings, the actual Android UAPI and BCrypt. The
complete final Windows x64 protocol/ownership fixture also links. This is build
evidence; actual Windows execution and the full product graph remain required
full-CI results.

The PR adds the complete sanitizer corpus and all six mutants to Linux preflight,
cross-compiles the current fixture, and requires its success marker in the native
Windows shard. Existing Binder framing, participant-lifetime, Binder/SG and
guest/host tests, all jobs and their deadlines remain. The reusable workflow's
inaccessible `fe42fc3d…` reference is replaced by published main `5aa6b390…`, which
has the same **new** `framework.yml` blob `6dc4cca565aaf15fdf518eee825b0e44765b275f`.
The full recipe and hosted-runner inputs are unchanged. Release/cache/actor policy
tests pass 21/6/3 checks.

## Reproduction

Run the helper from the candidate checkout. The baseline selection changes only
the production broker source; both use the same complete fixture.

```sh
python3 tools/test_binder_output_ownership.py \
  --sanitize --negative-controls --output-dir /tmp/binder-after

python3 tools/test_binder_output_ownership.py \
  --source-ref 95eb90b9ac81c5dee387d6685e7670ccaf19f44f \
  --sanitize --measure-only --output-dir /tmp/binder-before

# Counted baseline must fail only the scoped direct-output budget:
/tmp/binder-before/broker-output gate

python3 tools/test_binder_output_ownership.py \
  --timing --build-only --output-dir /tmp/binder-timed-after
python3 tools/test_binder_output_ownership.py \
  --source-ref 95eb90b9ac81c5dee387d6685e7670ccaf19f44f \
  --timing --build-only --output-dir /tmp/binder-timed-before
```

Execute `/tmp/binder-timed-{before,after}/broker-output measure all 6` in ABBA
order four times, with the same available CPU affinity. Each process emits all
six workloads and both warm-up and steady samples. Use sample indices 0–5 for
the steady table while retaining −2/−1 separately. Allocation accounting needs
the counted executable instead of `--timing`; its `measure` output records the
one-request counters separately from the timed loops.

The [raw JSON](binder-direct-output-2026-11-23.json) contains all source/build
receipts, full raw observations, summary samples, scoped counters, prerequisite
and efficiency negatives, mutation receipts and validation logs. It identifies
the local Linux UAPI header and the pinned Android 16 UAPI used for Windows,
so the host adapter is not presented as the target ABI or transport result.
## Main982 integration checkpoint

The subsequent composition uses ordered parents `c61fa022341e38ca2c7e88ce229b0e08ac2170d6` and main `98215dd7e798090fb5ab6160490ae78f1793ab3a`. The production broker is the exact automatic merge, SHA-256 `1adfde0ff9fa29299333bc674d2b002f6b2f19d9da450daeb659ae5b391d6f5c`. Main adds real AppContainer pipe-admission/security handling outside this leaf's output operations. That handling, the incoming source ledger and all CI gates remain present.

The original Binder output manifest and patch are unchanged. The policy still validates their historical published source against the original baseline and keeps native runtime approval false. A separate current-source endpoint applies that **same immutable patch** in reverse to recover every byte of main982, then forward to recover every byte of the composed broker. This also preserves the separately reviewed zero-length-copy prerequisite. Main's incoming OFD ledger is retained exactly; its updated coupled digests are not replaced by the older ledger. All original controls pass, with seven additional repinned current-source controls covering output mutations and the incoming AppContainer/PID admission guards. Source policy passes 33 ordinary controls, four repinned property controls, six historical Binder controls and seven composed Binder controls; shared-stdio passes 23 controls, and supplier checks remain green.

The first strict host build caught an integration boundary issue: the extracted broker core now contains main's `g_sandbox` global, but its actual `ServeProc` handshake consumer is intentionally outside this component. `-Werror` correctly rejected that unused variable. The fixture generator now annotates only that exact declaration `[[maybe_unused]]`; it keeps the declaration/value, all warning flags and every original assertion. It does not activate, simulate or claim to test the omitted AppContainer admission path. The original failing build log is preserved in the JSON checkpoint.

The composed real broker/core and socket/FD boundary corpus passes all **1,613 checks and six precise compiled mutants** under ASan/UBSan. Complete actual `binderd.cc` plus main's real `app_container.cc`, and the complete output component fixture, each link with strict warnings on Windows x64 and ARM64 using the pinned toolchain. Those links exercise the real production dependencies; they are not Windows execution or security-runtime evidence. Full manual fork CI remains the runtime gate.

Every earlier timing record, summary and source receipt remains unchanged in the JSON; this checkpoint adds a separate `main982_composition` object. There was no current-main timing rerun, and the historical component numbers are not relabeled as Windows or application performance on this integrated revision.
