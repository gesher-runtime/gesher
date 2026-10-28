# 0068 - Owned Binder scatter/gather pointer transport

Status: source candidate,2026-10-27. No compile/runtime/full acceptance.
This uses Binder2's existing reservedADR0068 and manager-assigned test88.

## Contract and source

The canonical Binder2 brief requires real BINDER_TYPE_PTR scatter/gather,
parent fixups, ownership/bounds and cross-process transactions. Current
integration rejects every nonzero buffers_size. FD arrays, security contexts,
freeze/priority and binder polling remain distinct open contracts.

The source reference is Android16's kernel/common android16-6.12 binder.c,
captured with verified git-blob identity d6e9e83b5da85c9fb789f0f8dc8a20288fa8c0a7:
https://android.googlesource.com/kernel/common/+/refs/heads/android16-6.12/drivers/android/binder.c
The saved source is C:/work/gesher-ci-review/binder-kernel-android16-d6e9e83.c.
The implementation follows the UAPI shape and observable ordering rather
than copying kernel implementation code:8-byte extra-buffer alignment,
nonoverlapping ordered object offsets, earlier PTR parents, in-range pointer
slots and forward fixups only along the last pointer's ancestry. The pinned
last-fixup offset's zero sentinel is preserved as a boundary control.

## Owned wire representation

The sender safely snapshots transaction data, offsets and each pointed-to
range through ReadProcessMemory on its own process. Failed copies report
transaction failure; invalid caller pointers never reach a broker dereference.
Every PTR object carries a relative offset into owned zero-padded extra bytes.
The broker independently validates the shape and retains those bytes within
the same Buffer lifetime as ordinary transaction data. The receiver allocates
data, offsets and extras together, then rebases each object and its parent
pointer slot into that allocation. Free/process/thread teardown follows the
existing transaction-buffer lifecycle; sender mutation/death cannot alter a
copied payload. The existing64MiB wire cap is checked before new allocations.

Wire version2 adds an explicit guarded-copy status and owned extra payload
length; old endpoints reject the hello. Both host client and broker must be
rebuilt together, including for component CI. No old/new endpoint mixing.
JNI/ELF pointers remain the public64-bit Linux layout; no Win32 pointer or
process handle is substituted for a guest PTR field.

## Verification required

Source review must precede CI. All compilation and executable fixtures run
in CI, including small probes. Actual independent client/broker/service
processes must prove request/reply/nested parents, receiver-address rebasing,
byte identity, sender mutation/death and receiver free/lifetime. Malformed
size/offset/overflow/parent/order/user pointers must fail without broker
faults or leaked/stuck transactions. Keep regular FD transfer, Bitmap and
InputChannel regressions, both native ABIs and relevant ART/APK modes.
Then require current accepted integration, both full Windows suites0,
installed package and both required e2e sets0. Neither this source nor a
shape-helper test closes the transport or aggregate Binder2 scope.

Current candidate has new fixture source but no compile/CI evidence yet. Receiver user-copy/
allocation failure cleanup and untrusted raw-wire dimensions require explicit
review/tests alongside the new SG lifetime. Identity authority and other
process/fd hooks remain coordinated with their owners, not implemented here.

## Pending delivery and descriptor ownership

The broker permits one uncommitted delivery per receiver thread. Claim only
validates that delivery; it never transfers descriptor ownership. Each raw
handle duplicated into the receiver is broker-owned staging until finish,
abort or thread teardown. The receiver duplicates each staging handle again,
then adopts its distinct owned copy. The broker closes and zeros staging
handles exactly once before accepting or aborting the delivery. A lost claim
acknowledgement therefore cannot leave ambiguous ownership, and a later
broker abort cannot close a receiver descriptor whose handle value was reused.
After local publication the caller owns its allocation/descriptors and the
read_consumed prefix remains valid even if the finish acknowledgement fails.
Before publication every failure rolls back local resources and remote
transaction/oneway ownership; even failure publishing an earlier BR_NOOP
closes the thread pipe because a later transaction may already be pending.
These are source invariants awaiting real disconnect/ACK-loss/death fixtures.

The shared POSIX actor source tests nested request/reply, two queued oneways
after actual sender process exit, malformed layouts and invalid user pointers,
real mapped-capacity ENOMEM and bad receiver read memory. The host parent
uses separate processes and ordinary control files with finite deadlines.
Raw-wire lifetime/FD reuse controls and Android ABI/ART/APK wiring remain
unfinished; the fixture source itself is not verification.

The focused source peer ofdf80f63b found no new definite blocker in the staging
correction. Additional raw-peer fixture source now peeks the exact next
length4/status0 ACK without consuming it before disconnect; tests pre-claim,
claim loss, installed-copy abort, finish loss and real receiver exit at each
boundary. It checks staging closure, real file identity through distinct owned
copies, actual handle-number reuse, repeat old-buffer cleanup and queued
one-way progress. The common actor adds real EMFILE descriptor exhaustion and
a protected-page read destination retaining a valid BR_NOOP prefix. None has
compiled or run yet. Android ABI/mode wiring and final peer remain pending.

Wider descriptor cleanup/identity remains open: binderd's existing
CloseRemoteHandle uses PID-only OpenProcess and ignores live duplicate-close
failure. This leaf must not imply permission/identity completion. Future gates
need controlled live PROCESS_DUP_HANDLE denial/recovery with observable
retained-handle accounting and process-creation identity/PID-reuse controls,
in addition to ordinary FD/Bitmap/InputChannel regressions.
