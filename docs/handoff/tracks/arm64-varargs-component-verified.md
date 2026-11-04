# Coherent API3 Native2/ART component verification

Exact source942f2c16b1219cd25bcb1c1ca4215f41639cc88f passes
[component40556015168](https://github.com/gesher-runtime/gesher/actions/runs/40556015168),
attempt1: nine conclusions, eight success/one reusable smoke skip. All eight
actual executed job logs and final metadata were read. This is a scoped
prerequisite, not full/package/e2e/integration/Surface acceptance.

| Executed job | ID |
|---|---|
| Current API3 runtime and all executed boot tools |122372715039|
| Framework host tools |122372718775|
| Current both-ABI Native2 libraries/APKs |122372718950|
| Independent pinned QEMU FPSR oracle |122372719526|
| Current framework |122372827415|
| Current API3 translator and guest/JNI fixtures |122372918578|
| Windows2022 |122393236254|
| Windows2025 |122393236300|

## Actual Windows scope, both hosts

- Complete original Native2 dalvikvm/single/device execution in x86_64 and
  ARM64: six actual process/context records and every original native-library,
  program self-test, public-import/error and APK assertion preserved. The six
  current variadic reports are each required from actual child results.
- Original shallow and separate deep Surface printf/vprintf logger return0,
  both ABI native ELF formatted-output/mixed overflow/va_copy/nested/thread/
  wide/binary128-source/gzip retry consumers pass, and original API3 smoke
  includes incompatible version rejection. Actual old-host/new-DLL mismatch
  proof remains scoped40528739976; no old executable is overlaid with only a
  new DLL.
- Actual ART JNI primitive/mixed V and independent copied lists pass:
  `CallStaticIntMethodV independent va_copy`,
  `CallStaticObjectMethodV independent va_copy`, `Arm64Jni all passed`.
- Five FPCR and five FPSR cases pass, including native host restoration,
  three actual ARM JNI/APK contexts and native ELF contexts. The independent
  oracle retains8192+144+2048=10384 numeric/status rows, matrix
  f2b99a23a8071d84f2700ca7fa725574c763303fe594e173c6f2bd7375641d5b.
- Each actual final summary is
  `ACTUAL_ARM64_COMPONENT_SUMMARY 0 test(s) failed; current API3 runtime/DLL and guarded boot jars`.

## Provenance and image coherence

All six executed native tools (dalvikvm, dex2oat, profman, dexdump,
gesher-exec, arm64_elf_test) and the matching API3 translator are freshly built
from this exact source, with explicit Temurin21 for Java. Current host artifact
ID12761691179 has zip digest
0d4f9b9f387a53a6376180eb339c1d2ba3dced1c02e2061d108f132ef930680d;
translator ID12756314642 has
eca4789b1432c3574c0cbee1c0ec1319ee990f9597a9776a7256b8e14a69b644.

Scaffolding run40516054280/ca4ed3cfff46d4843485d89859370dc5b86a4146
remains overall red for Web3, not accepted as a product. Each of its eight
consumed artifact producers is independently required success with exact run/
attempt/SHA; every artifact's repository, source, unique ID, nonexpired state
and SHA256 digest metadata is checked. All eight actual rows were read.

All five current jars are byte-hashed against the pinned producer. Four
differ and gesher-app is identical, so both Windows runners actually rebuild
the image with current API3 compiler/profile tools and current jars. All15
produced art/oat/vdex members match their actual consumed copies by SHA256.
Both hosts use identical current jar hashes and identical rebuilt image
member hashes. No stale image reuse or missing-tool conditional was skipped.

Plain logs/metadata/watch are preserved under
`C:\work\gesher-ci-review\native-arm64-component-40556015168-*`.
No artifact was downloaded locally, no local compiler or native/JVM/C# fixture
ran, and no broad full was dispatched by this verification.

## Remaining gates

Environment85, original Surface80, coherent full Windows suites, package and
both complete e2e remain separate gates. Future qualifying unions must include
the accepted Libcore integrationa7889e76 and the relevant Web3 fix. Existing
full environment/default-object, host CRT/libm, JIT/signal/LSE/HFA/quad/cache/
stack contracts are not closed by this component. No roadmap box is changed.
