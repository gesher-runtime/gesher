# arm64-2 sticky FPSR: verified bounded stage

Exact combined source `8ec4bf3c30160575c8df794ad2ca1406957adc98` passes full
CI [40498782924](https://github.com/gesher-runtime/gesher/actions/runs/40498782924),
attempt 1. All 29 job conclusions (23 success/six intentional skips) and all
23 actual executed job logs were personally read. Both Windows suites print
`0 test(s) failed`, with 723 actual PASS and no FAIL lines each. This count
describes these logs; no native export inventory or historical count changes.

Full acceptance is limited to FPCR and the five sticky-status operations,
including the narrow eight-scalar-operation status correction, as detailed
below. It does not close the full fenv/ARM track. The immutable verification
branch is `wip/arm64-2-fpsr-verified`, with documentation only above this source.
Earlier frozen FPCR `dab49ab0` and Wide `e7c1b06` references are unchanged.

Source 93f7637a090e40f3bfc3615a04bed2076dbf501c passes scoped
ci 40475532910, six success/one skipped. All six actual executed job logs
were personally read. Both Windows hosts match all 10384 independent QEMU
numeric/status tuples, every diagnostic classification counter is 0, and
both FPCR and FPSR fixtures end with `0 test(s) failed`.

Actual FPSR execution through native2 JNI/dalvikvm, single/device APKs,
native ELF, package and both required e2e suites passes in the combined full.
Merge b839c307 imports accepted
root integration db84749b (docs above 30deb08c, including verified exit
freshness) into the working branch. Root then independently accepted exact
Windows source160e25e8/full40466021634. Merge b946a69a imports that exact
source, preserving all ARM production code unchanged from scoped93f7637a.
Merge c39767a8 includes latest integration6297e80a; its additional non-doc
diff is empty. Unaccepted spawn/Media follow-ups are not imported. Later
accepted Net4 integration `bc6e05be` is not in this candidate; root must run
combined integration CI when its non-doc tree includes both stages.
Root alone merges into integration/main. Immutable FPCR reference
wip/arm64-2-fpcr-verified/dab49ab0 and original wide/e7c1b06 are unchanged.

## Full job evidence

| Executed job | ID | Personally read actual evidence |
|---|---|---|
| Main build |122170898774| Complete production build and published Windows binaries |
| Framework host tools |122170899154| Cached/verified host tools and publication |
| Assembly |122170899177| Required assembly objects and artifact |
| Exec |122170899190| Actual launcher tail/command-line PASS |
| Independent QEMU |122170899260| 10384 inputs; unchanged original prefix and cumulative/boundary provenance PASS |
| POSIX build |122170899293| Final 629/629 links, including ART base/dex tests |
| Surface apps |122170899358| Actual fixture APK builds |
| Native libraries |122170899389| Both NDK ABIs build |
| Graphics |122170899412| Final 824/824 libhwui build |
| Native2 |122170899441| Both NDK/APK libraries; extracted ARM original-body bytes/definitions/no-unresolved audit PASS |
| ANGLE |122170899443| Built artifact |
| Libraries |122170899476| Built artifact |
| Framework |122170986483| Framework/app javac and fixture APKs build |
| ANGLE probe build |122170994352| Built probe |
| Translator |122171017177| Production -Werror build, exact two patched backend hashes |
| Media |122174132838| Built native/JNI media artifact |
| WebView |122174133338| Built WebView artifact |
| Boot image |122188597383| Actual 3-space Hello load and framework/app image compilation |
| Package |122188810941| All 23 installer PASS, real installed UI input and complete uninstall |
| Windows 2022 |122188811005| Final zero; six test66, five test69, five test75 PASS |
| Windows 2025 |122188811041| Same actual groups and final zero |
| E2E 2025 |122189672528| All 10 apps nonblank/input/exit, four transport cases, association/shortcut; 0 unexpected |
| E2E 2022 |122189672566| Same ten apps and gates; 0 unexpected |

Actual full PASS lines on both Windows runners include:

```
PASS  arm64-2 FPSR: real flags, arithmetic, nested callbacks, thread isolation and restored native fenv
PASS  arm64-2 FPSR (dalvikvm): pinned flags, masks, real arithmetic, QC, pthread inheritance/isolation and nested callbacks
PASS  arm64-2 FPSR (single): pinned flags, masks, real arithmetic, QC, pthread inheritance/isolation and nested callbacks
PASS  arm64-2 FPSR (device): pinned flags, masks, real arithmetic, QC, pthread inheritance/isolation and nested callbacks
PASS  arm64-2 FPSR: an actual native ELF process verifies pinned status, arithmetic, threads and nested callbacks
0 test(s) failed
```

The host Check requires all original 8192, cumulative 144 and boundary 2048
independent comparisons, every cold/nested/thread assertion and a zero summary;
the detailed all-zero classification counters were also personally read in the
same-production scoped run below. Full test66 retains both ABIs and all three
modes. Actual Unicode records verify separate signed/unsigned wchar32 full-range
width digests; CPU/QPC clocks, latest-child exit and both-ABI action-registration
contracts also pass. No full-run output is inferred from export presence alone.

Both actual TIC80 e2e records are nonblank and react to input: Windows 2025 has
6635 non-background/3416 text pixels and changes 5836 pixels after the click;
Windows 2022 has 8713/2441 and changes 612. Package evidence includes input to
the installed app, device shutdown, association/shortcut removal and uninstall.

## Implementation and scope

Use the real pinned dynarmic GetFpsr/SetFpsr for the five flag operations:
feclearexcept, fetestexcept, fegetexceptflag, fesetexceptflag and
feraiseexcept. Preserve QC outside pinned FE_ALL_EXCEPT 0x9f, including ARM
denormal 0x80; fexcept_t pointers address exactly four bytes. Status inherits
and persists through actual nested entries and pthread_create independently
of native host flags and rounding. The original pinned bionic bodies are
compiled as guest references with renamed symbols.

Two native implementation boundaries polluted otherwise-correct cold guest
status. Protect dynarmic cache-miss compilation and native executable-address
classification/cache work with masked-nearest MXCSR, restoring exact live
guest MXCSR afterward. Original cold assertions remain; identical warm
controls are additional diagnostics. The dependency-cache key includes the
patch helper digest, so an older native backend cannot satisfy a newer key.

For only the eight scalar32/64 add/sub/mul/div emitters, classify operand
bits under FZ to record IDC before the unchanged arithmetic/NaN path. Per
FZ instruction isolate native UF/IX, remove that instruction's FTZ IX only
when its own UF appears, then merge the complete previous hardware status.
This preserves seeded IX and real IX from earlier arithmetic in the same
translated block. The numeric/NaN path is unchanged. Other instruction
families are outside this correction and are not claimed complete.

Pinned dynarmic source 9d4582339990d4eae53f1dc7160686920fc2075c is strictly
hash checked, including reverse reconstruction and idempotence. Actual CI
records these patched-source digests:

- a64_interface.cpp:
  7743b21c4bd62c37c60fbd0eaf42c05c5a3f1133bd25944b3b30a4da33374851.
- emit_x64_floating_point.cpp:
  3a2181e72a70fba90f4be6704bd5c5899254ee156babdbfce4ae09e670b11da5.

## Independent reference and actual results

Unmodified QEMU 9.2.4 source b2995afed254e4804611df52210067a4d5dbdf0b
executes a freestanding NDK r27c AArch64 ELF as Cortex-A53. Actual
MSR/MRS/FADD/FSUB/FMUL/FDIV supply the numeric/status results; Linux
write/exit only serialize output. It uses no libc or copied production
fenv implementation. The actual QEMU binary SHA256 is
4ccb5bea9e24502b0e938248e42e2aa68586a6b766d769de882b54c7350122da.

The original 8192 rows cover all rounding/DN/FZ combinations, both widths,
32 signed-zero/normal/overflow/underflow/subnormal/infinity/quiet and
signaling NaN operand pairs, payload/sign/ordering, and cumulative seeds.
Their complete original byte prefix must retain SHA256
434b73d47dbd4d73fadda6a8879b0cf3fe4bf5a1419f5359148bc7b30b8210bb.
Appended 144 cases test preseeded IX plus numerically dependent
inexact→underflow and underflow→inexact sequences within one block.
Appended 2048 normal-operand cases cover the boundary near the smallest
normal across the same operations/control modes. Those boundary cases pass
without any additional production correction.

Current producer reference-source/vector/ELF SHA256 values, respectively:

- a597f9a5de44dcf11785996c2ee347ba55b8e1abe8d3b4a4ec83c9939bd23d88.
- 7a8f7553634321617e69783d6fa7bcdc3ec7231ed1e82d9cf16a69f6fbf4d003.
- 4a2cf57bd41fd0b2ef00466771abb87a8c1f6265ae3f34a807504fab1862ff4e.

Complete 10384-row matrix SHA256:
f2b99a23a8071d84f2700ca7fa725574c763303fe594e173c6f2bd7375641d5b.

Actual scoped40475532910 logs read:

| Job | ID | Actual evidence |
|---|---|---|
| Pinned bionic source |122092720590| Complete licensed source/header provenance PASS |
| NDK/native2 |122092720924| Both ABI/APK builds; extracted ARM reference bytes/definitions/no-unresolved audit PASS |
| Independent QEMU |122092721227| Original prefix unchanged;10384 numeric/status inputs and cumulative/boundary controls PASS |
| Translator build |122092851515| Production and host fixtures compile under-Werror; both exact patched hashes |
| Windows 2022 |122093491451| All10384 cases, every classification counter 0, all FPCR/FPSR groups, both zero summaries |
| Windows 2025 |122093491619| Same actual independent matrix, cold/nested/thread controls and zero summaries |

The actual host PASS groups include original 8192, cumulative 144 and
boundary 2048 independent cases, all six architectural flags plus SQADD QC,
three guest levels with direct MSR, concurrent guest isolation and restored
native host fenv. NDK fixtures additionally verify 48 seeded FZ32/64 output
checks in each actual full-run dalvikvm/single/device/native ELF context.

## Investigation history and open contracts

Initial nine cold failures were retained and diagnosed using actual raw
values. Compiler guard 40463561077 reduced them to two; executable-cache
guard 40464902850 left only input-denormal. Independent8192 baseline
40468480689 exposed 672 mismatching tuples per host. IDC-only
40471466364/77fafc18 then proved numeric 0/status 32, missing_IDC 0 and
extra_IXC 32 on both. Per-operation output status 40474911020/a4d17876
passes 8336; boundary-only 40475532910/93f7637a passes 10384. No assertion was
warmed away, ignored or converted into an expected failure.

Full fenv_t save/restore, the ARM eight-byte __fe_dfl_env object and
rounding-sensitive host CRT/libm remain open. Pinned trap-enable returns -1,
disable/get return 0 and raise sets masked status; working traps are not
claimed. Other ISA families, guest JIT pages, signals/ucontext, LSE,
HFA/variadic binary128 and cache/stack allocation/cleanup remain open.

FPSR edits outside compat/arm64 are the focused probes, Native2 module,
native ABI/ELF/assembly fixtures, test 75 and reusable/scoped ARM workflows.
There is no new FPSR edit to test 44 or the APK builder; test 75 reuses the
complete actual records captured for FPCR. Earlier accepted Wide/FPCR shared
edits and imported root/Media/Exit prerequisites retain their own reports.
No inventory count, export presence claim or other track ownership changed.
All compilation and native/JVM/emulated fixture execution occurred in CI;
local work was source inspection/edits, hashing/parsing and Git only.
