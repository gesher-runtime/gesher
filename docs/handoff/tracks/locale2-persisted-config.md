# Locale2 persisted-config first slice: scoped proof, product gates pending

Source `004fff103e7b31af519a24f63b0893cb16e8ef79` passes scoped CI
[40512781936](https://github.com/gesher-runtime/gesher/actions/runs/40512781936).
All four actual executed job logs and conclusions were personally read:

| Job | ID | Actual result |
|---|---|---|
| Framework host tools |122218620007| Cached tools restored and outputs published |
| Framework and APKs |122218689677| javac0; production jar and both actual APKs built |
| Windows 2025 |122222254599| 21 PASS groups, no FAIL, `0 test(s) failed` |
| Windows 2022 |122222254971| Same actual 21 groups and zero summary |

This run compiles current Java/APKs and verifies exact native runtime provenance
`40498782924` / `8ec4bf3c30160575c8df794ad2ca1406957adc98` inside CI. Execution
is explicitly imageless. It is not a native rebuild, boot-image, installer or
e2e acceptance claim. There was no local compilation, fixture execution or
artifact download.

## Actual scope

The APIs are real framework `LocaleManager`, `LocaleConfig`, and the
`ILocaleManager` Binder boundary in two real APKs. Both Windows hosts verify:

- Override set/replacement/clear and app/device restarts, retaining ordered
  selected locales through `containsLocale` language/script matching. Actual
  French/Hebrew resource qualifiers and configuration/default locale lists
  reflect the selected list. An explicit empty override stays distinct from
  absence, including a non-null configuration with null supported list.
- Reordered equivalent supported configuration preserves original stored order
  and skips reconciliation, as the pinned service does. Setting selected
  locales itself still permits locales outside the supported configuration.
- Every required single-process callback before headless EndLoop: exact counts
  2/1/2/3/2/2/1 for seed/equality/replacement/clear/empty/null-list/failure-seed,
  each ordered locales/greeting payload, and the final real resource/default
  snapshot. Read/failure/boundary-only calls require zero callbacks. Device
  mode still reaches its original settled snapshot and actual Activity finish;
  intermediate configuration coalescing remains visible rather than waived.
- Malformed committed state returns null, leaves the malformed bytes unchanged
  on GET, survives restart and recovers after restoring valid state. Real
  staging and source-rename failures preserve old committed bytes and selected
  locales. Both actual rename controls show 65 newly written staging bytes
  before the locked source prevents replacement.
- Null and unknown-package errors, user 1/CURRENT/ALL denial and CURRENT_OR_SELF
  fallback. A distinct normally registered package can read public supported
  configuration, but private selected reads, delegated selected writes,
  supported writes/clear and cross-user queries are rejected. Its own clear
  with no original resource LocaleConfig clears its selection and preserves
  the target package. This Java API slice makes no NDK ABI claim.

Primary hashes and exact baseline semantics are in
[ADR0066](../../decisions/0066-locale2-persisted-config.md) and the
[source audit](locale2-source-audit.json).

## Coherent full candidate

Merge `acb144fbe14d191ce6b476d7f8c4a299b92e4b0c` imports current accepted
integration `bc6e05bea341d1dc79920deaf3f069ad690fedc4`, including Net4.
Merge `d698a74e51f8aee7b0042ef93f0c70dbbce47f4c` imports immutable verified
Wide/FPCR/FPSR stage `75e7ef29645302a860e702207b43131f9f28b1d3`.
The two Native2 include/Run conflicts retain all four headers and
Extras/Unicode/Wide/Dlopen/Files/ifaddrs plus ARM-only ArmFpcr/ArmFpsr.
Both reference builds, APK ifaddrs markers and all captured evidence remain.

Reviewed source comparisons show ARM/common wide production byte-identical to
the frozen FPSR stage, Net4 wrappers/headers byte-identical to accepted Net4,
and Locale Java/APK/test81 source unchanged from scoped `004fff10`. Lightweight
PowerShell/YAML/JSON/Python/Bash parsing passes. Full verification of this
combined tree, both suites, all existing Native2/clock/ABI/oracle controls,
installed input/uninstall and both required e2e jobs remains pending. No pending
spawn, AOT or unaccepted Media continuation was imported. Root alone merges
into integration/main; the original Wide, FPCR and FPSR refs do not move.

## Failed/superseded runs and open limits

Initial `40507901609` was canceled as superseded. Source `f9f3f629` / compile
`40508467291` passed production/framework javac but failed shell invocation126
because the new wrappers had mode100644. Source `71ee7a0c` / scoped
`40509891830` compiled both APKs and passed all ten device groups per host, but
nine single-mode groups failed: the delayed fixture report never ran before
Main's documented headless EndLoop. The current run retains the settled report,
adds the required queued observation, and strengthens callback counts/payloads;
no failure was ignored, warmed away, retried unchanged or reclassified green.

The complete privileged permission/installer/IME model, process-registration
attestation, permission-gated/installer notifications, uninstall/retain-data,
real Windows NLS/event/sign-in behavior, regional formats and native locale/
time-zone contracts remain open. The shared-host-UID/all-granted service
answers are concrete Services3 gaps, not grants used by this slice. Separate
files are separate commits; power-loss/multi-file transaction and concurrent
independent standalone writers are not claimed. See ADR0066 for the exact
storage/concurrency/ordinary-caller boundary. `GESHER_LOCALES=en-US` is a
controlled input, not proof of an actual Windows display-language change.

Locale-owned changes outside its service/fixtures are the additive two-APK
framework build/copy lines, its scoped CI workflow/runner, and the explicit
Native2 merge resolution. No generic Device.write/Installer, global permission,
ATMS, bionic or translator implementation was edited for Locale2.
