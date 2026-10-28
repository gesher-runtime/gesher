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

## Fixture source checkpoint (not compiled or run)

Prepared two additive scenarios `readwrite`/`readonly` in the existing shared
actor and all nine contexts. Existing seven scenario bodies/assertions remain
unchanged. A real transferred owned descriptor is included in both new paths.
The receiver uses two real mapped pages, a supplied initial consumed offset
(not a transported prefix), stable RO/RW protection and whole-page sentinel.
RW checks actual command/payload/descriptor bytes and free/reply; RO checks
EFAULT, unchanged sentinel, unpublished consumed offset, invalid/reusable
descriptor slot after the normal claim/adopt/publication-abandon path.

The parent compiles OutputPageObserver.cs only inside CI, holds one actual
Windows process handle across ioctl, independently VirtualQueryEx/RPM-observes
the whole aligned page/protection/hash before and after, and checks that PID
against the later actual normally completed actor PID. Marker files hold the
actor alive for observation. Source hash and actual observations are emitted.
The existing actor completion/distinct-PID/open audits remain mandatory.
PowerShell parsing and diff whitespace checks passed locally; no C/C++/C#/
Java compilation or fixture execution occurred. This checkpoint still needs
independent source peer, refreshed exact component prerequisite/source guards,
fresh producers and actual both-Windows runtime. Do not push historical
component workflow wiring as a qualifying run.

## Explicit exclusions (continued)

## Independent fixture peer and strict archive dependency

Independent Native review at552c548808c45ae03e1ed62cd7ab86f9907d88ca found
no additional definite stable-pointer/actor/owned-FD/page-observer blocker
after the missing PSCustomObject OutputActorPid predeclaration was corrected.
Exact peer and four blob/SHA256 records are saved in
C:/work/gesher-ci-review/binder-stable-readonly-peer-552c5488.md and
its -hashes.json companion. This is source peer only, not compile/runtime proof.

A new binder2-stable-readonly-component.yml draft refreshes exact qualified
30db/full405961 ancestry plus ordinaryed741/a474, narrow allowed delta, fresh
host/broker/API3/framework/actors/boot suppliers, current artifact attempt
timestamps, whole host payload manifest and the already reviewed authoritative
compiler metadata helper. Its push trigger is deliberately absent until the
new strict archive dependency is integrated/reviewed. It is not dispatched.

Whole-archive raw SHA256-before-extraction and isolated assembly collision
rejection are now explicit component/canonical/release requirements. Terra
owns new CI-only download/extract/assembly helpers under tools/surface; reuse
exact independently reviewed helper bytes and execute their actual negative
controls in this component, then inspect real verified archive observations.
This helper proof can be independent of unrelated Surface visual acceptance.
Do not run draft metadata-plus-download-artifact wiring as qualifying proof.
No local artifact download, C# compilation, fixture run or primary code merge.

## Strict helper proof and reviewed launch wiring

The earlier draft hold is superseded only for the refreshed strict component:
Terra's independently source-reviewed21441571 helpers have actual bounded
proof in Surface40662612433/attempt1/source786e9535 (docs-only above214).
Integrator independently read both saved plain Windows logs: each has nine
actual adversarial rejections, thirteen authenticated raw-SHA-before-extract
archive records, isolated owned assembly, identity rejection and whole-host
missing/extra controls. The later Surface caller-PWD/image-copy failure leaves
Surface80/86 unexecuted and does not establish Surface qualification.

Binder imports precisely Git blobs c12018f7 downloader,8d01e508 assembler and
a97840fd negatives. The canonical raw Git blob downloader SHA256 is
d82ec81f14f879958eeae2489acecc2ec39cc903dc1d564b376ab5b829320b6e;
the Windows CRLF working-tree SHA8eb9e261 is a different representation and
is not used as its canonical digest. All three raw Git byte hashes were
independently recomputed. CI materializes exact Git bytes and validates them
before using the helpers; no local helper negative or native/C# execution ran.

Independent Terra wiring peer of3ee47cd8 found only that canonical downloader
digest discrepancy, corrected in770e8ae6. No additional definite source/
automation/provenance blocker was found. The component's cheap prerequisite
executes exact helper byte checks, actual nine negatives/positives and one
authenticated known30db boot archive before any expensive producer. That
archive is preflight-only and never enters the current runtime assembly.

Current boot and runtime consumers verify every exact current artifact's
source/run/attempt/repository/job/creation window and raw ZIP digest before
isolated extraction. All inputs are freshly built; no previous runtime is
overlaid. Binder owner names allow no collision replacements. Inline boot
compilation retains the original compile/Hello assertions, invokes pwsh -File
to isolate caller cwd and consumes two strict current archives. Both runtime
jobs run the same negatives before five strict current archives, whole-host
manifest/compiler/image/fixture guards and all old/new ordinary scenarios.
Source parsing passed for YAML, Python AST and twenty embedded PowerShell
blocks, without executing the scripts locally. Fresh actual CI remains the
only compilation/runtime proof. Narrow own-workflow push registration may now
launch this reviewed source once no active same-ref run exists.

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
