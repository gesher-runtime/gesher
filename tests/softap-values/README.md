# SoftAP configuration and WPS value qualification

The public fixture compiles against official SDK36 only. Its direct calls cover
all selected public methods and reflect the fifteen SDK-inlined integer constants
against actual runtime fields. Hidden reflection supplies system value state for
copy and Parcel tests, and invokes the real framework compatibility test overrides.
No manager, radio, capability, permission, or network operation is substituted.

Run the host corpus against fresh app output, including its freshly generated
compatibility table:

```sh
python3 tools/test_softap_values_contracts.py \
  --java-home "$JAVA_HOME" --sdk-jar work/sdk-36/android.jar \
  --framework-classes out/framework/classes --core-classes out/coremod \
  --app-classes out/app/gesher-app-classes.jar
```

The baseline control additionally accepts
`--source-ref d733cc0ffbff9e51892914c726c85b0da5f3c53c`. It verifies that both
production source files are absent at that exact commit, omits their candidate
classes, and executes the same SDK caller through a strict loader. Both targeted
provider failures must occur; cached compilation stubs cannot fill the gap.
`--output-dir` retains commands and negative-control receipts in a new directory.

`prepare_softap_values_oracle.py` accepts the same five build-input arguments,
plus `--d8-jar`, `--aosp-source-dir`, and a new `--output-dir`. The source directory
must contain the exact upstream files/hashes listed in the tool. It builds
separate stock API36, independently compiled pinned-r4, and candidate payloads
with one SDK corpus. It emits frozen hashes and explicit setup/execution ADB
arrays for the device owner; it never invokes ADB.

The stock image predates r4's final SoftAP band-optimization field, so its bytes
are recorded separately. The full r4 bytes are the Windows golden. The fixture
checks exact cursor consumption using a trailing sentinel, independent prefix
decoding, copied state, equality/hash, and diagnostics. No host Parcel substitute
is used.

`tests/windows/tracks/96-deep-compatibility.ps1` requires actual production
native execution and all twelve full wire matches. The checker can be exercised
with `tests/windows/SoftApValuesParcelChecks.ps1`; its logic checks do not replace
Windows execution. The latter remains pending while canceled workflows are
left canceled.

See the [implementation and limits report](../../docs/research/softap-values-contracts-2026-11-18.md)
and the [complete native reference receipt](../../docs/research/softap-values-android-reference-2026-11-18.json).
