# Suggestion and Passpoint value contracts

These callers compile solely against the official Android SDK36 declarations.
They exercise `WifiNetworkSuggestion`/`Builder`, `PasspointConfiguration`,
`HomeSp` and all four credential types. Reflection reaches the genuine hidden
`Policy`, `RoamingPartner` and `UpdateParameter` values needed by Passpoint.
The certificate material is public test data and is never installed.

## Host qualification

```sh
python3 tools/test_wifi_suggestion_values.py \
  --java-home "$JAVA_HOME" --sdk-jar /path/to/sdk36/android.jar \
  --framework-classes /path/to/framework/classes \
  --core-classes /path/to/coremod --app-classes /path/to/gesher-app-classes.jar \
  --output-dir /new/evidence/host --inventory-output /new/evidence/sdk-members.json
```

The host driver recompiles 24 production value families and checks their defining
paths before executing the corpus. The shared native host boundary supplies
framework initialization/logging and a real Linux elapsed clock. It does not
implement Parcel, certificates, Wi-Fi, Passpoint or security decisions.
The host's actual `Build.TYPE` is printed: the existing initialization boundary
reports `unknown`, so this run qualifies the non-debug predicate branch only.
The separate Android run retains its actual `userdebug` build type.

The negative control verifies that all eight new production source files are
absent at the exact named revision and uses a strict loader that forbids falling
back to framework compilation classes:

```sh
python3 tools/test_wifi_suggestion_values.py \
  --java-home "$JAVA_HOME" --sdk-jar /path/to/sdk36/android.jar \
  --framework-classes /path/to/framework/classes \
  --core-classes /path/to/coremod --app-classes /path/to/gesher-app-classes.jar \
  --source-ref d733cc0ffbff9e51892914c726c85b0da5f3c53c \
  --output-dir /new/evidence/negative
```

Both the Suggestion Builder and Passpoint constructor must fail at their intended
absent defining providers. A passing baseline or a different failure is an error.

## Real Android native Parcel

`tools/prepare_wifi_suggestion_oracle.py` takes the same compilation inputs plus
`--d8-jar`, `--aosp-source-dir` and a new `--output-dir`. The pinned source directory
contains the eleven exact upstream files and hashes listed in that tool. It
prepares four immutable DEX jars, eight artifact hashes, complete class hashes,
source hashes, exact upstream adaptation diffs, and explicit ADB request arrays.
It never invokes ADB or changes an emulator.

Three processes run the same SDK caller corpus:

1. The stock API36 boot providers from the selected image.
2. Independently compiled Android16-r4 Suggestion/Passpoint sources and a coherent
   r4 `WifiConfiguration`/`SecurityParams` pair, with genuine platform helpers.
3. Freshly compiled Gesher production sources and their complete value closure.

Every process checks provider ownership and helper parameter/return identities
before any corpus call. The reference's four new nested Parcelable readers and
its source-owned Configuration reader use their defining loader. This is an
explicitly adapted reference; the full original writer bodies remain unchanged.
No Parcel creator cache is patched and no mixed-owner graph is accepted.
The stock image may lack the later internal BuildProperties/Environment helper;
their observed presence/absence is printed separately. All public value providers
and all source-owned helpers remain mandatory. No missing helper is fabricated.

Two compile-only declarations are excluded from every executable archive:
`WifiManager.ALL_ZEROS_MAC_ADDRESS` resolves to the verified real boot field;
`Flags.multiUserWifiEnhancement()Z` is called by
`WifiConfiguration.getCreatorUserIdInternal()I`, whose only caller in that class
is `getCreatorUserId()I`. Both declarations are absent from official SDK36 and
the selected 163-declaration corpus. The pinned source marks both `@hide` and
only the outer method `@RequiresApi(37)`. These paths are not executed and their
runtime linkage remains unqualified; payload exclusion alone does not establish
linkage. There is no executable substitute flag, manager, Environment or
BuildProperties. The provenance receipt records exact source and bytecode checks.

The native corpus checks 24 complete wire values, direct creators, exact cursor
consumption, trailing sentinels, named Parcelable dispatch and re-marshalling.
Unordered map reconstruction can change iteration or Java-serialization layout
while preserving a value. If bytes differ, the fixture identifies the individual
map-bearing component, substitutes only its parsed map representation into a
fresh test copy, and requires exact full component and outer byte equality plus
full value equality and subsequent encoding stability. First-write reference/
candidate bytes are compared without normalization. An unexplained change fails.
It includes all security families, binary SSIDs, subscription metadata, Passpoint
with policy/update/real X.509 keys and certificates, every credential kind, and
null/default values. Stock-image differences are recorded separately from r4.

The recorded Android run passes 443 checks and 24 native cases in each profile.
All 24 complete first-write encodings match between pinned-r4 and candidate.
Stock's ten Suggestion wrappers are each 12 bytes shorter; the 14 standalone
values match. The only observed roundtrip representation changes are the
`serviceFriendlyNames` Bundle in the two full Passpoint cases. The retained v1
failures and precise v2 harness repair are documented in the
[qualification report](../../docs/research/wifi-suggestion-contracts-2026-11-18.md).

## Required Windows gate

`tests/windows/tracks/96-deep-compatibility.ps1` invokes
`WifiSuggestionValuesNativeTest` with `--require-parcel`. This standalone ART
entry point loads the genuine `android_runtime` library before calling the
unchanged corpus; the Android oracle runs inside an initialized `app_process`. It requires successful native completion and all 24 complete
byte lengths/SHA256 values from the independent pinned-r4 reference. Missing,
duplicate, malformed, shortened or mismatched wire observations fail the gate.
`WifiSuggestionValuesParcelChecks.ps1` exercises those rejection rules.

Actual Windows execution remains pending for this candidate. Canceled CI is not
restarted by this change or the qualification tools. Host and Android results
cannot satisfy the separate Windows qualification.

## Scope boundary

These are values and validation rules. They do not admit network suggestions,
provision Passpoint, authenticate a subscription, install a trust anchor, request
permissions, connect to a radio or create a capability. Wi-Fi manager/backend
work remains separate. A syntactically valid SIM credential is a value, not
evidence that a SIM or carrier subscription exists.
