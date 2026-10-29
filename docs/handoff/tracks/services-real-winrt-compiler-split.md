# Services actual WinRT compiler/runtime split

## Private compiler host correction (pending CI)

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
adds package provenance. Local verification is source/parser/diff only. Fresh
both-Windows component proof and integration union/full remain pending.

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
