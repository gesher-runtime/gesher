# Passive Fonts resource preparation: source slice only

This additive slice is based on immutable
`4d54ade1b53483f6d604bd989a7674ea6c376a1f`. It adds no native child, debugger
command, workflow, target, authority or process runner. The old five WCT modes,
their 15-second bounds, production owner, published preflight and parked runtime
6913 remain unchanged. Original Windows2025 WCT ownership is still RED. All
owner/WCT/private-symbol/full qualification flags remain false.

The exact architecture input is the 15151-byte proposal SHA256
ecee43546edaa6d413558e5a447914362faea78ac26c14e52310082682131538, independently
reviewed in the 12556-byte architecture report SHA256
0e45186b993387dbe7a38a908db4f642a2c5635597873d2905148d2030f12f30.
This implements only its passive parser and protocol metadata recommendation.

## Dependency reader boundary

`tools/fonts/resource-pe-imports.py` accepts immutable bytes. It validates AMD64
PE32+, DOS/COFF/optional/section headers, data-directory bounds and unique
file-backed RVA mappings, including validated header mappings. Virtual tails,
overlapping virtual/file sections, malformed directories and nonterminating
descriptor arrays fail closed. A missing directory is explicitly absent, not a
parse error converted to an empty dependency set.

Limits: 128 MiB input, 96 sections, COMBINED 512 visited regular/delay descriptor
slots INCLUDING each encountered zero terminator, and 128 printable
ASCII name bytes plus mandatory NUL. Only delay attributes exactly 1 (RVA
mode) are supported; legacy VA or unknown-bit representations are rejected with
the actual attribute value. Descriptor order, directory bounds, original names
and lowercase names remain separate. Thunk/symbol contents are not interpreted.
Names are passive text, never filesystem path admission or lookup instructions.
This inventory does not resolve API sets, dynamic loads or transitive closure.
No import name, including any msdia name, establishes a cause or installation
requirement.

The adapter accepts only the three exact manifest identities `cdb.exe`,
`dbgeng.dll`, `dbghelp.dll`; it requests at most 128 MiB plus one sentinel byte
from an already admitted, retained read-locked stage stream and requires exact
manifest name, byte count and SHA256. It opens no paths and discovers no files.
The adapter does not itself prove signatures, handle identity or lock lifetime.
The eventual CI runner must project those exact fields from the admitted stage
manifest, keep the validated stream lock, ensure initial cursor/full regular-file
read semantics, and log raw manifest/header facts before rejecting. No real
tool bytes have been read for this slice. Tests construct synthetic memory only;
no PE file is written, compiled, downloaded, mapped, loaded or executed.

## Exact controller protocol metadata

`tools/fonts/resource-protocol.py` is a pure state/receipt model. It creates no
actual CLR root, native state, clock, event, file or process. A future runner must
implement the receipts and root against real handles; passing this model is not
execution evidence.

One elapsed monotonic session starts at zero. Declared proposed limits are
330000 ms total, 90000 ms preparation, NEW diagnostic hold at most 180000 ms,
and a 30000 ms cleanup reserve. Hold deadline is set ONCE as the smaller of
hold-start plus 180000 and session-end minus cleanup reserve. Each role has at
most 1048576 captured pipe bytes; readiness metadata at most 512 bytes. These
are design constants, not executed timeout coverage or a modification of the old
15-second tests. Cleanup completion can be recorded after expiry, but cannot
change failure into success or extend the capture deadline.

Exactly four separately named roles exist: `child`, optional `help`, optionally
omitted `prewarm`, and `capture`. Omitted optional roles are explicit OMITTED;
enabled roles start NOT_STARTED. Required child/capture are never invented as
omitted successes. Starting records a nonzero DWORD PID once; reuse/overwriting,
unknown roles and duplicate PIDs are rejected. EXITED requires the same PID and
true kernel completion; DISPOSED additionally requires pipe closure. Failure may
leave a role NOT_STARTED, which means no process was created, not a successful
run. Help completion does not replace target preparation or target prewarming.

| Phase | Required receipts and next admission |
|---|---|
| PREPARED | Reserve entire-session Normal-root graph BEFORE any actual resource. Stage/copy/identity/signature/module work and all native storage/event/invalid-input allocation occur here. Start child in normal nonheld prepared state; optional help has distinct ownership. Prewarm, if configured, attaches to this same prepared child only after help terminates. |
| PREWARM_COMPLETE | Exact child remains running; configured help/prewarm each disposed with exit 0, others explicitly omitted. Actual target-ready, final-stage-check and symbols-prepared receipts required. Omitting prewarm is declared and cannot be described as an actual attach/prewarm. No preparation is deferred into held callback. |
| HOLD_READY | Explicit START may now be admitted by a later runner. Exact child PID plus distinct nonzero DWORD writer/handler OS TIDs, both real native handles still pending, fixed readiness record; one immutable hold deadline. These are capture preconditions ONLY, never resource or owner proof. |
| CAPTURE_COMPLETE | One separately owned capture role has truly exited, drained and disposed with exit 0 before deadline, and raw evidence is recorded. Missing symbols/unsupported output cannot manufacture ownership. No capture command is admitted by this source slice. |
| RELEASED | Release is delivered to the same child before the original hold deadline; next require actual child completion and all pipe/file cleanup. This does not prove the native wait resource. |
| COMPLETE | Every configured role disposed with exit 0; all copied handles closed; within original session budget. Release the actual Normal root exactly once only after these facts. Qualification flags remain false even here. |
| FAILED | Preserve exact bounded reason; signal release/perform bounded actual owned-process cleanup. NOT_STARTED/OMITTED roles remain explicit. No root release while any process or copied handle may remain live. Late cleanup does not turn this into COMPLETE. |

The entire staged file-owner/list/stream and ALL role process/pipe graph must be
rooted by the real Normal GCHandle before resources, following approved 4d54
lifetime semantics. On failed completion the graph stays rooted for surviving
CLR-parent lifetime, not after parent termination. The pure root reservation and
release predicate here are metadata, not an implementation or lifetime claim.

Native lifetime is SEPARATE: preallocate storage/events/invalid input and prestart
writer/IO helpers before the hold; no allocation, formatting, module discovery or
printf in the invalid handler. Storage/events/handler installation must remain
quarantined in the live child until BOTH retained handler/writer handles truly
complete. Only then may native storage retire and the handler restore. Exact
child-process termination permits OS reclamation, not a claim that native cleanup
executed. A parent CLR root cannot make a native stack frame survive unwinding.
Failure must not return/unwind referenced native state before those conditions.

## Deferred command and startup admission

The chosen honest startup policy is observed vendor defaults **NOT_DISABLED /
NOT_EXHAUSTIVE**. No installation/update/global-setting command or loader closure
claim follows. Optional help is capability data only: not actual-target prewarm,
not pre-gallery proof and not permission to execute options discovered in output.
This slice defines no command strings, help invocation, stack syntax or uf.
Any later exact plain-stack descriptor must freeze explicit radix/count and
trusted DWORD OS-thread selection plus output bounds. uf is omitted pending its
separate precise export/range/decoded-text admission.

Future evidence still requires a real waiter-derived resource ADDRESS and the
SAMEADDRESS ownerTID matching held handler under exact module/symbol provenance.
No CRITICAL_SECTION layout may be assumed for an unknown/SRW primitive. Genuine
replacement witness and fresh runtime/JNI/guest/JavaOs/OEM/retention qualification
remain open; parser/protocol controls cannot replace them or release Gesher.

## Pure controls

Run only the two Python metadata suites. No native processes or real tool bytes:

```
python tools/fonts/test-resource-pe-imports.py
python tools/fonts/test-resource-protocol.py
```

No workflow/guard/pin/authority is added in this finite slice. A future whole
candidate must retain every old full/CORE/ENV/HOST/CDB inner/outer/selected/whole
history/DAG/noop/shadow/selfpin/current-producer gate, exact additive CMake
reversibility and current source/attempt role evidence before possible CI.
