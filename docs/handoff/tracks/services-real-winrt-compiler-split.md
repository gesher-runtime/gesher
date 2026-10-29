# Services actual WinRT compiler/runtime split

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
