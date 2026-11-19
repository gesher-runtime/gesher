# Wi-Fi channel and blocking option value contracts

This increment supplies the complete `WifiAvailableChannel` and `BlockingOption`
sources, including the latter's builder. Applications can construct, compare,
inspect and parcel these local values. No manager call, radio operation, channel
enumeration, regulatory decision or network blocklist is added.

## Pinned source and declared surface

Both complete files come from Android16-r4 Wi-Fi commit
[`e3f09203409da7a91d2c5cf9f0109da6d5243319`](https://android.googlesource.com/platform/packages/modules/Wifi/+/e3f09203409da7a91d2c5cf9f0109da6d5243319/framework/java/android/net/wifi/).
The [provenance record](wifi-channel-blocking-values-provenance-2026-11-18.json)
binds raw SHA256 and Git blob IDs to directory tree
`bc6fa389e4813423eb82cfcc71379058c497f0e9`, complete dependency sources and SDK36r02.
The public/protected denominator is three types and 27 declarations: channel 16,
blocking option 8 and builder 3. Hidden constructors, filters and nested
annotations remain separate source contracts.

The original Apache-2.0 notices are preserved. Only unavailable flag annotations
and imports, three SOURCE-retained `WifiAnnotations.ChannelWidth` uses, and four
AndroidX `NonNull` uses plus their import are removed. The audit compares every
remaining token, including full fields, constructors, readers, writers and method
bodies. The original `ScanResult.CHANNEL_WIDTH_20MHZ` expression remains; pinned
and inherited source independently declare 0. No replacement ScanResult, Parcel,
manager or runtime helper is packaged.

## Observable contracts

| Value | Preserved behavior |
| --- | --- |
| Channel construction | The public two-integer constructor sets width to 20 MHz (0). The hidden three-integer constructor stores all arguments without validation. |
| Channel identity | Equality and hash include frequency, modes and width, in that order. The string prints the mode mask in lowercase hexadecimal. |
| Channel filters | Regulatory is 0, cellular coexistence 1, concurrency 2, NAN instant mode 4 and usable filter 3. These values do not establish hardware capabilities. |
| Blocking builder | Its constructor eagerly requires duration 1 through 86400 with the original exception message. BSSID-only defaults to false; setters return the same builder and builds take primitive snapshots. |
| Direct blocking construction | The hidden integer/boolean and original Parcel constructors do not impose builder validation. |
| Blocking identity | Equality includes both fields. Hash order is boolean then duration. The original string intentionally has no closing bracket. |
| Parcelable | Channel retains three consecutive integer fields. Blocking retains duration then boolean. Both original CREATORs and the direct blocking Parcel constructor remain. |

Primary source:
[WifiAvailableChannel.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/e3f09203409da7a91d2c5cf9f0109da6d5243319/framework/java/android/net/wifi/WifiAvailableChannel.java),
[BlockingOption.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/e3f09203409da7a91d2c5cf9f0109da6d5243319/framework/java/android/net/wifi/BlockingOption.java),
and [WifiAnnotations.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/e3f09203409da7a91d2c5cf9f0109da6d5243319/framework/java/android/net/wifi/WifiAnnotations.java).

## Independent integration on main

This increment is based directly on canonical main
`826ba8a0e87335607350a7ced029b97cc232e324`. It has no pending Wi-Fi PR
prerequisite. Main provides the unchanged ScanResult constant and compilation
dependencies. Its build scripts discover the new Java sources automatically.
The source audit contains its own literal-preserving tokenizer and does not
import pending P2P tooling.

Only the channel/blocking check is appended to main's Windows Track96. All
existing main track bytes and the temporary main-push CI pause are retained.
Eight open Wi-Fi increments independently append other Track96 checks; their
implementations and test calls are not copied into this PR.

## Measured builds and provider coverage

The [build/provider evidence](wifi-channel-blocking-values-build-evidence-2026-11-18.json)
records actual main-before and main-plus-feature application and service builds.
All four builds pass.

| Artifact | Main before | Main plus values |
| --- | ---: | ---: |
| Application DEX definitions | 2,356 | 2,364 |
| Compile-only application classes | 1,330 | 1,337 |
| Compressed application JAR bytes | 1,352,644 | 1,353,891 |
| Selected public types in application DEX | 0 | 3 |

The genuine retained framework DEX has 34,764 definitions and none of the three
selected types. All three are absent before and present in both after artifacts.
The other added definitions are original creators, nested annotations and a D8
access marker; they are not extra public API declarations.

Observed application build times were 31.89 and 24.45 seconds; service builds
were 10.01 and 9.36 seconds. Maximum child RSS for application builds was 658,812
and 673,756 KiB. These are single build observations with concurrent work, not a
runtime performance comparison or aggregate process-tree memory measurement.

## Behavior validation and evidence boundaries

Public callers compile against official SDK36r02 only; hidden callers compile
separately. All 27 declarations are checked: 21 direct constant-pool references
plus six inlined constants also inspected as runtime fields. Candidate and
pinned Android16-r4 sources each pass 96 host checks. The exact-main absence
control rejects SDK-stub fallback through its closed loader. Host execution
supplies no Parcel implementation.

The Android capsule packages stock callers, pinned-source values, candidate
values and a provider-checking launcher separately. It binds source inputs,
compiler/dependency hashes, classfile/DEX providers and eight deterministic JARs.
Rebuilding against the actual main-before compilation JAR reproduces all eight
artifacts byte-for-byte identically to the independently reviewed capsule.
Compiled production and Java fixture inputs remain unchanged.

The [Android reference](wifi-channel-blocking-values-android-reference-2026-11-18.json)
records sixteen ordinary complete records: original readers, field comparisons,
exact consumption and matching rewrite bytes. Historical observations retain
their provenance. Fresh stock, pinned-r4 and candidate runs each passed **289**
checks with **16** complete native reader/writer/rewrite records; all profiles
and all reference bytes match. Seven setup steps and two cleanup steps passed.
[Full native observations](wifi-channel-blocking-values-native-observations-2026-11-18.json)
retain the actual output and profile, with the capsule and log hashes.
SDK36r02, pinned API36.1 source and the actual Android36.0 image are separate
profiles. A stock image's exposed module members are checked explicitly.
UID2000 app_process checks do not establish ordinary-APK permission behavior.

Windows Track96 requires the production boot graph, exact profile, sixteen
ordered reader records, both digests and the final fixture counter. Host and
isolated Android output cannot satisfy it. Its checker passes 42 logic controls,
which do not qualify native Windows execution. Actual Windows/ART remains a
separate gate; A02 remains open.

## Reproduction

Use JDK21, pinned SDK36r02, genuine framework/core classes and the actual
main-before application compilation JAR. Native capsule preparation uses the
pinned Build Tools 36.1 D8 artifact, separately from the app build's R8 dependency.

```sh
python3 tools/audit_wifi_channel_blocking_sources.py \
  --upstream-dir "$PINNED_WIFI_VALUES" --output source-audit.json
python3 tools/test_wifi_channel_blocking_values.py \
  --java-home "$JDK21" --sdk-jar "$SDK36_JAR" \
  --framework-classes "$FRAMEWORK_CLASSES" --core-classes "$CORE_CLASSES" \
  --app-classes "$MAIN_APP_CLASSES" --upstream-dir "$PINNED_WIFI_VALUES" \
  --profile candidate --output-dir candidate-host
```

Repeat with `--profile pinned-r4`. For the absence control use
`--source-ref 826ba8a0e87335607350a7ced029b97cc232e324` and a separate output
directory. The native preparer accepts the same inputs plus `--d8-jar` and
`--output-dir`; it writes operator requests but never executes device commands.
