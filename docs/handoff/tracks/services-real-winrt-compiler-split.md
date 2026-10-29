# Services actual WinRT compiler/runtime split

## Private compiler host correction (component accepted)

Component [40801812064](https://github.com/gesher-runtime/gesher/actions/runs/40801812064),
attempt 1, source `1b3ee904d586fd2a2f06841b04a46d6c07fce28d`, is terminal
green on Windows-2022 and Windows-2025. Integration source-peer approved this
exact commit before its account-gated push. Independent raw-log reread found
seven TSF supervisor records and five controlled-worker cases per runner, all
four actual AsyncInfo/AsTask success/error/timeout/late records with disposal,
and `WINRT_ORDER case=late bridge_completed_before_cancel=True`.

Both runners report `WINRT_ROSLYN_EMIT success=True external_compilers=False`,
`PASS WinRT fixture with actual bridge and owned child retirement`,
`PASS WinRT provider with actual bridge and owned child retirement`, and the
typed `Windows.Services.Maps.MapLocationFinderResult`, one result read,
resources disposed, `status: InvalidCredentials`. Compiler, fixture and provider
each retain two processes (root + console host), 9/9 closed handles and 0-2ms
cleanup; no compiler descendants were admitted. Both producer-file cleanup and
`WINRT_PRIVATE_COMPILER_CLEANUP directory_removed=True` are present.

The actual orchestrators differ: 2022 is 7.6.9; 2025 is 7.6.7. Both private
compiler roots are exactly 7.6.9 / CLR 10.0.14, with identical package SHA,
109620855 downloaded bytes, 661 entries and 256666658 expanded bytes. Both emit
DLL SHA256 `deec6ef8ebf0c6b8e8e405d1b65690ee290a54941290c06765bc960382544a14`.
Raw logs: `C:\work\gesher-ci-review\services-winrt-408018-{2022,2025}.log`.
This accepts only the component prerequisite. Fresh integrated full remains
mandatory; main, release and aggregate Services completion are not claimed.

The 408009 Windows-2025 host-pin failure is corrected by acquiring the same
official PowerShell 7.6.9 portable x64 ZIP on both runners, without consulting
or changing the installed host. Official release 423544544 / asset 675639558
publishes SHA256 ce7484804c0f0ad4dda10c958e71acdaf7eebfb22505c1b480f89e0e9f1da94d,
also independently listed by the release's hashes.sha256. This fixed checksum
and exact 109620855-byte length are enforced before extraction. Network reads
have a 120-second deadline and byte bound; extraction checks paths, duplicates,
links, entry count and expanded size. A unique RUNNER_TEMP tool directory is
removed in producer cleanup, including partial acquisition failures. It never
enters PATH or shipping artifacts. The private pwsh remains a separately owned
supervisor root; runtime remains Windows PowerShell 5 with unchanged WinRT,
typed result, source/controller identities, descendant rules and all controls.
Exact original Roslyn DLL hashes and compiler APIs/options remain mandatory.
Both orchestrator and actual private host versions are printed, and the receipt
adds package provenance. Local verification is source/parser/diff only.

## Full port readiness (read-only audit)

Against integration `0baa011a47caf31598ad779771aaad213e92d4ec`, the normal
Windows test sparse checkout includes `tests`, covering every helper and helper
C# source, but MUST also receive `tools/windows4` for the producer supervisor.
The Services branch's ci.yml has that additive sparse entry; do not copy its
whole ci.yml because other union adaptations belong to integration.

Selected Services sources relative to its f0f52116 base are the component
workflow; helpers geocoder-compile, geocoder-compiler-host,
geocoder-provider-probe, geocoder-roslyn-worker, geocoder-winrt-fixture and
run-geocoder-winrt-component (all .ps1); tracks 58-devices.ps1,
geocoder-operation.ps1, geocoder-probe-exit.cs and geocoder-winrt-supplier.cs.
Canonical supervisor/control blobs remain d1cf9319, 08f29f78, 91e6946c and
4474bae1; canonical TSF workflow blob remains 6b025963. Preserve integration's
actual qualified TSF source and all component source-identity assertions.

Normal 58-devices creates the compiler in its owned supervisor role, keeps the
private compiler files until the PS5 child validates its receipt and DLL bytes,
and cleans both directories in finally. Receipt checks use the actual union
repository, run, attempt, source revision and producer role, not fixed component
identities. The normal provider still uses existing DevRun's 45-second process
bound. Keep the mandatory union-services reusable component job for the strict
owned fixture/provider child evidence, seven/five controls and four real cases.

Acquisition network reads have an overall 120-second cancellation deadline and
exact byte bound; extraction has 5000-entry / 600MB-total / 150MB-file bounds,
but no independent extraction wall-clock deadline. The normal Windows test job
has its existing 60-minute outer timeout. Compiler cancellation remains 40s
inside a 45s supervisor bound; runtime fixture/provider each have 45s bounds.

## Current correction: owned in-process Roslyn

40799322272 at af5883f3 is terminal red on both runners: the direct Framework
csc root launches cvtres.exe, correctly rejected by the unchanged supervisor.
That also left compiler scratch files, so the known-file-only cleanup failed.
Do not retry that source or qualify it from the earlier seven/five controls.

Source 06863a070e14a4ff0b04354a9b7c31ed8ca7facd replaces that compiler role
with a separately supervised pwsh root running Roslyn APIs in-process. It pins
PowerShell 7.6.9 and both bundled Microsoft.CodeAnalysis assembly SHA256 values.
No CodeDOM, csc, cvtres, external linker, native resources or compiler server
is invoked by the producer. Explicit Framework64 mscorlib/System/System.Core
references target Framework 4 for the later PS5 runtime load. Compilation uses
CSharp5, library, Release, AnyCpu, deterministic output, no concurrent build,
40-second cancellation, and the original 45-second supervisor bound.

The receipt retains source/compiler/reference hashes and adds the worker
path/hash and fixed options. Source hash and syntax parsing use the same bytes.
Runtime still verifies and loads the same DLL bytes. Original supervisor source,
runtime descendant classification, seven/five controls and all four actual
AsyncInfo/AsTask cases plus real Maps result remain unchanged. Finally removes
only the three known producer files and its checked unique temporary directory.

Integration source peer approved the exact clean commit before push. Local
checks were PowerShell syntax parsing, hash/metadata inspection and diff-check;
no C# compilation or fixture execution occurred locally. Changed-source
component 40800919965 is now terminal: integration reports windows-2022 success
with all four actual AsTask cases, compiler/source/owned-child evidence, typed
Maps InvalidCredentials result and cleanup. Windows-2025 fails before compilation
at the strict `Unpinned compiler host` gate (geocoder-compile.ps1 line 17).
The actual 2025 host version is not present in that failure. Raw saved logs:
`C:\work\gesher-ci-review\services-winrt-408009-{2022,2025}.log`.
The successor must diagnose the actual host and use verified pinned acquisition
or an equally explicit reviewed version/hash contract; do not assume its version
or relax the pin. Both-Windows component acceptance and union/full remain open.
Integration owns the sole watcher; the implementation worker stopped its local
watch session without cancelling CI. Source 06863a07 remains the exact tested
source; the checkpoint above it is documentation only and was not pushed.

## Historical compiler split (superseded by correction above)

Component 40798004312 at 747c68a6 failed on both Windows runners because
Windows PowerShell CodeDOM launched csc.exe inside the strict runtime job.
That rejection was correct; no runtime descendant allowlist is changed.

The corrected producer runs Framework64 csc.exe itself as the suspended,
assigned, bounded root of a separate TsfJobSupervisor job. It uses explicit
/noconfig /nostdlib+ framework references and records repository, run, attempt,
source revision, producer role, source/reference hashes, exact compiler
path/hash/version, exact argument vector/command, and output DLL SHA256.
The two helper sources use no Windows SDK metadata at compilation; Windows
PowerShell 5 performs the actual WinRT projection at runtime.

Runtime children verify producer context and all hashes, then hash and load
the same DLL byte array. They never compile source. The unchanged supervisor
still rejects compiler descendants in runtime children. Compiler and runtime
roles require identical original kernel identity, membership, stream retirement,
process census and handle cleanup assertions. Both normal-suite and component
paths delete only the producer's two known temporary files and unique directory
in finally and clear inherited receipt variables. The seven supervisor controls,
five controlled workers, four AsyncInfo cases and real Maps result gate remain.

Status: source-peer reviewed after receipt context/cleanup corrections; local
PowerShell parser and diff checks only. No local C# compilation or execution.
Fresh component proof on both Windows versions is required before union/full.
407980 is historical red evidence and cannot qualify this correction.
