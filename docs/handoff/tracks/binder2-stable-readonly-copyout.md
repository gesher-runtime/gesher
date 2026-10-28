# Stable read-only Binder copyout successor, 2026-10-28

Source-only work; no new compile/runtime/full acceptance. The designated
integrator owns C:/work/gesher-binder2-sg on wip/binder2-stable-readonly.
Reconciliation merge a474aefaa553f53f3fdf14345c199b0a4ae3683c starts from
qualified phase4e837f8c1 (code-identical to full40596167427/source30db) and
ordinary component docs headdbe66a4f (runtimeed741/full component40579429217).
It merged cleanly. All selected Binder driver/broker/actors/component wiring
were independently compared to ordinary parentdbe66 and remain byte-identical;
all other qualified union code is retained. This successor is not integrated.

## Small production guard, pending fixture and CI

CopyToUser now prechecks the entire nonzero output range using VirtualQuery:
checked address/size and region additions, MEM_COMMIT, no PAGE_GUARD, progress,
and only RW/WRITECOPY/EXECUTE_RW/EXECUTE_WRITECOPY. Existing zero-size success,
WriteProcessMemory complete-size result, errno/publication order and allocation/
descriptor ownership remain unchanged. This deliberately addresses only stable
nonwritable protection. Concurrent protection/unmapping and partial-copy
atomicity remain open. CopyUser input is unchanged.

Native peer confirmed no ownership conflict and the immutable e0dd22b1
UserRange source's whitelist/overflow/region contract, while explicitly
retaining source-only and race limitations. No shared helper relocation or
unqualified Native54/callback code is imported. Local work has only read/edited
source, Git and lightweight script processing; all compilation and native/JVM/
C# fixture execution remain CI-only.

## Next required implementation and independent actual proof

Preserve every existing ordinary scenario/assertion and the nine host/ABI/mode
contexts. Add separate real RW and stable RO output controls with a writable
binder_write_read structure, real queued independent sender and transferred
owned descriptor. The same ordinary output body must prove RW delivery/payload/
actual receiver free, and RO EFAULT with unchanged whole-page sentinel bytes,
unchanged actual protection and correct unpublished consumed-byte state.

A fixture can supply preexisting read_consumed=sizeof(BR_NOOP) solely to avoid
an initial NOOP preceding the queued transaction; document that this supplied
starting offset is not an observed transported prefix. It must not become a
public transaction-prefix/ACK-loss experiment. Use a preceding mapped page for
the base pointer and the entire actual command destination on one stable RO
page. Publication then occurs after normal allocation/claim/FD adoption, so
the existing abandon path must close the newly owned FD and free the buffer.
Directly observe the descriptor slot is invalid before/after failure and
available for reuse; retain real sender normal completion/failure records.

Actual page protection needs an independent observation in every actor mode;
guest mprotect success alone is insufficient. Prefer a CI-only parent-owned
Windows VirtualQueryEx/ReadProcessMemory fixture against the identified actual
actor PID, with the actor held alive by ordinary marker files before and after
ioctl. Inspect the whole sentinel page and exact protection both times. Build
that C# or native observer only in CI, bind its source/hash to the component,
and record its actual PID/address/protection/bytes observations. No production
pause, transport failure or allocator hook is needed.

The existing component prerequisite restricts source changes to its historical
d280 build; it must be refreshed for this exact qualified baseline and reviewed
new delta before dispatch. Build fresh current host/broker/translator/framework/
actors and matching images, verify all source/run/attempt/artifact/member hashes,
read both actual Windows logs. Do not dispatch obsolete wiring or rerun red
source unchanged. Scoped proof must precede a later canonical full/package/e2e.

## Explicit exclusions

Existing prefixfault/adoptfail/invalid/mapped-capacity and normal FD/Bitmap/
InputChannel regressions remain untouched. This work does not resume filtered
b52/public-prefix/ACK-loss/raw-wire experiments or substitute for their missing
proof. Remote duplicate-close/PID identity, concurrent protection, partial-copy
atomicity and other Binder2 contracts remain open. Ordinary component evidence
is still exactly its prior bounded scope. Inventory55e03c68/all1157 remains
unchanged. Primary excludes this successor, SG, later reader ownership leaf,
Surface, after-resource and Native87/54/callbacks. All hardware/all-gap/main/
release requirements remain open.
