# ARM variadic-list correction: scoped verification

Exact source928a09b71b0e2a6d3a9e284583c8d9002479bdad passes
[scoped40528739976](https://github.com/gesher-runtime/gesher/actions/runs/40528739976),
attempt1. All five conclusions and four actual executed logs were personally
read: four success and one intentional reusable-workflow smoke skip. The real
Windows acceptance job itself executes the original smoke test on each host.

| Job | ID | Actual result |
|---|---|---|
| Host loader/shared shim |122275365617| Current arm64_elf_test and gesher-exec built with shared shim/loader warnings as errors |
| Translator/NDK |122275366561| Current DLL/smoke and two ABI C++ consumer executables built; original baseline and copied-JNI-V fixtures compile |
| Windows2022 |122276739649| Every required positive consumer and negative compatibility control passes |
| Windows2025 |122276740688| Same actual complete positive/negative controls pass |

## Actual runtime scope

Both Windows logs contain:

- `arm64_smoke_test all passed`, including existing constant/add/host-entry/
  callback/LL-SC controls, null initialization and four incompatible-version
  rejection checks (0,1,2,future).
- `PASS arm64 varargs: original shallow Surface printf/vprintf body returns unchanged`
  and the corresponding deep PASS. Both processes actually return0, print the
  original full literal and `arm64_elf_test all passed`. Trace records seven
  GP/eight FP slots and eager_bytes0. The original C baseline source is unchanged.
- Actual x86_64 and arm64-v8a ELF processes each return0 and report the complete
  `literal/few/many GP/FP; ordered shared overflow; va_copy; nested/thread; wide
  and binary128 source; gzvprintf retry` contract as all passed. These checks
  retain 80 integer arguments, both GP/FP overflow orderings and interleaving,
  width/precision, independent copies, live-list qsort callbacks, two threads,
  exact wide bytes through a separate stream and real large-output gzip retry.
- Actual API2-host/API3-DLL and API3-host/API2-DLL processes each return1 with
  version mismatch and no successful guest-library load. Their FAIL-looking
  unavailable-translator rows are required negative controls, followed by
  `PASS arm64 varargs: actual API2/API3 host-DLL mismatches reject before any guest library runs`.
  The driver explicitly exits0 only after both controls and all positive checks.

The positive host loader/shared shim and translator are rebuilt from this
exact source. Pinned full40498782924/8ec4bf3c supplies the proven runtime
scaffolding and old API2 compatibility binaries inside CI only. No local
artifact download, compiler or native/JVM/C# fixture execution occurred.

The private host list preserves24 bytes and independent copied-list cursors.
Its immutable descriptor retains separate GP/FP bank end pointers; conversion
reads no payload or overflow words. Shared NextGp/NextFpArea select one
requested bank or shared-stack argument. The direct binary128 reader uses
that same source helper while preserving existing narrowing and stack alignment.
This is not full binary128 accuracy, direct-variadic or HFA acceptance.

## Source/history and pending gates

Resume checkpoint: the guarded coherent component now has a narrow push
trigger for `wip/arm64-varargs`, limited to its workflow and driver. This
registers the workflow absent from main without dispatching an unrelated full
run. Its exact CI-built API3 host/DLL, current Native2 and five-jar/image
guards remain unchanged. Runtime evidence is pending; no acceptance or
roadmap closure follows from registering the workflow.

Baseline3b607e75/diagnostic40525678026 establishes the prior actual valid-call
eager-copy defect on both hosts: shallow139, eager_end=top+0xa0 versus a
separate deep0 control. This does not independently establish all original
Surface failures' causality. Original Surface source/assertions stay with Media.

First correction d9bc86f3/scoped40527840216 is red before runtime because the
reusable sparse checkout omitted tests/native2; host and strict DLL compilation
pass. Additive checkout correction and bounded reviewed fixture corrections
produce the green928a09b7 scoped source above. No original runtime failure or
assertion is skipped, warmed away, retried unchanged or relabeled green.

Current64f33ff3 adds the same contract body to Native2's actual JNI/APK library,
requires its marker in both ABI dalvikvm/single/device contexts, and reserves
test86 for the original baseline/standalone/API controls. No production source
changes follow scoped928. Compile-only40529197932 at exact
64f33ff3a27364bcb46aeb3a0f75f38921fd2462 passes attempt1. The actual completed
job122277019774 log was personally read: both NDK shared libraries and
corresponding real APK native-library entries build, javac/d8 and the existing
third-party/symbol pipeline complete, and the final artifact is published.
This establishes compilation/packaging only. Its actual log and metadata use
the40529197932 prefix in the same external review directory. The copied JNI V controls compile
in the scoped source but have not yet executed through actual ART here.

JNI/APK execution, all existing FPCR/FPSR/Wide regressions, original Surface,
whole suites, package and both complete e2e jobs require fresh coherent full
verification. Locale2 full40516054280 is red for three Windows2025 Web3 failures;
neither that union nor the ABI correction is merged/accepted as a product.
Root owns source sequencing, integration/main merges and final release.

Actual local log evidence lives outside worktrees under
`C:\work\gesher-ci-review\native-arm64-varargs-40528739976-`:
jobs.json, host.log, build.log, test2022.log and test2025.log. Baseline and
failed correction logs retain their respective run-number prefixes.
