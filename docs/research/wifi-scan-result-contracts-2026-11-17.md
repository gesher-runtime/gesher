# Android 16 Wi-Fi scan value contracts

This implements the SDK 36 `ScanResult` data family for issue #56 A02. The baseline
is main `0af3fd8333e43cdeb7a3b4bcd0403758e6d556c2`. It has a six-field ScanResult
and no runtime `ScanResult.InformationElement` or `MloLink`. Clients compiled
against the official SDK fail with `NoSuchMethodError` at the copy constructor
and channel conversion and with `NoSuchFieldError` at `centerFreq0`.

The data implementation now retains scan identity, raw SSID, channel geometry,
information elements, flags, MLO links and the Android 16 parcel format. Security
types are derived from the supplied capability text using Android's ordered
classification. None of these operations performs a scan, connects a network,
asserts support for a radio feature or supplies observations to an application.
The existing WifiManager still reports no Android Wi-Fi controller and returns
an empty scan list. No native Wi-Fi producer of these objects was found in the
current source; test observations remain isolated fixture data.

## Provider and inventory evidence

`tools/package/gesher-app.ps1` puts `core-oj`, `core-libart`, `core-icu4j`,
`framework` and `gesher-app` on the boot class path in that order. The framework
planner's SDK-module APIs are compile-only. Its compiled Wi-Fi stubs must not be
mistaken for runtime implementations. `app/build.sh` compiles these production
Java sources into `gesher-app.jar`; it does not copy every compile-only module
class into the boot runtime. Shared-library resolution is separate in
`SharedLibraryRegistry`/`SharedLibraryGraph`; no alternate Wi-Fi provider was
identified for this family.

The earlier saved boot inventory identified these gaps. A fresh compile of the
current baseline, SDK-compiled negative controls and the candidate application
DEX confirm this selected slice:

| SDK 36 class | SDK public/protected member denominator | Baseline missing | Candidate missing or incompatible |
| --- | ---: | ---: | ---: |
| `ScanResult` | 55 | 44 | 0 |
| `ScanResult.InformationElement` | 10 | 10; class absent | 0 |
| `MloLink` | 21 | 21; class absent | 0 |

The ScanResult baseline resolves `toString()` through Object; a direct declaration
subtraction would incorrectly count that as a 45th missing member. The checked-in
[descriptor report](wifi-scan-result-members-2026-11-17.json) records each SDK
descriptor, baseline status, candidate DEX result and input provenance. The host
runner also checks the freshly compiled selected declarations and access flags
using the existing `java_member_inventory` parser.

These are selected member-presence measurements, not a percentage of Android
behavior supported. A02, SDK 37, other Wi-Fi APIs and issue #56 remain open. All
37 open PRs were checked through palmoni5 before choosing the scope; the existing
36-PR changed-file inventory contains no Wi-Fi source overlap, and #108 owns
the separate APK/evidence tools.

## Source provenance and adaptation

All upstream sources are from `platform/packages/modules/Wifi` at
`android-16.0.0_r4`, under `framework/java/android/net/wifi/`. Their Apache 2.0
notices are retained.

| Upstream source | SHA-256 | Use |
| --- | --- | --- |
| [ScanResult.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/refs/tags/android-16.0.0_r4/framework/java/android/net/wifi/ScanResult.java) | `ca6c29c8c119dc6ba22fbd4c92a2e4f76a3f4a5b6d894f3d81ba6f89832d1875` | Imported data model, public helpers, nested information element, copy and parcel implementation. |
| [MloLink.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/refs/tags/android-16.0.0_r4/framework/java/android/net/wifi/MloLink.java) | `b78caf9cea90593f93bc03b1fba0e3582d3e962328fb8a8fe056b46af9c5f9c4` | Imported MLO data, equality, redacted copying and parcel implementation. |
| [AnqpInformationElement.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/refs/tags/android-16.0.0_r4/framework/java/android/net/wifi/AnqpInformationElement.java) | `565f496f653ac76d3c6a75da4512ba37388fd3726cdf85e676f1615da58e5661` | Unchanged hidden payload dependency of ScanResult's wire format. |
| [util/ScanResultUtil.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/refs/tags/android-16.0.0_r4/framework/java/android/net/wifi/util/ScanResultUtil.java) | `9845eb5dced5124beb22521c6d2e3008268254e03c09317065de6a1dd0da698e` | Capability predicates and ordered security classification in package-private ScanResultSecurity. |
| [WifiInfo.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/refs/tags/android-16.0.0_r4/framework/java/android/net/wifi/WifiInfo.java) | `9b6a82a813cb612866e2d11448016622fcd62c8a290687bcf2db5a36b3bc0da2` | Reference for security ID conversion and maximum RSSI 200. Existing facade is unchanged. |
| [WifiScanner.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/refs/tags/android-16.0.0_r4/framework/java/android/net/wifi/WifiScanner.java) | `1814e8c065636da89dfedd05dc393627ff0d87bcbe8cd41908b898b99fd0cfc9` | Reference for band values; scanner service remains unavailable. |

Adaptations are limited to dependencies absent from the local module surface:

- Unavailable WifiAnnotations, FlaggedApi/Flags and AndroidX Keep annotations are
  omitted. D8 does not shrink these classes.
- Band values use the pinned constants directly. MloLink imports the equivalent
  ScanResult constants. Its unavailable hidden WifiInfo maximum-RSSI and
  NetworkCapabilities redaction constants use the pinned values 200, 1 and 2.
- InformationElement's Android S equality gate uses `Build.VERSION.SDK_INT` in
  place of the module's SdkLevel helper. The runtime under test advertises API 36;
  no older preview-device equivalence is asserted.
- Security classification returns WifiInfo IDs directly instead of allocating
  SecurityParams/WifiConfiguration objects and converting their IDs. Predicate
  order, transition-network ordering, Passpoint supplements and unknown-AKM
  behavior follow the pinned implementation. No Wi-Fi configuration API or
  controller-support claim is added.

Upstream semantics are retained where they are easy to misread. The ScanResult
copy shares its information-element array and its element objects; MLO lists get
independent list structure while retaining their element objects. InformationElement
constructors clone payload bytes and return read-only ByteBuffers. An empty
ScanResult has no producer-populated information-element array or capabilities;
its two corresponding getters throw as in the pinned source. A null capability
string is not reported as an open network. Frequency conversion floors values
inside the upstream band bounds and is not a regulatory channel validator.

## Verification

The public caller fixture is compiled against the official SDK 36 android.jar,
SHA-256 `d9eb9da824d9e247a352f570f01e1169e725b2954bca9e283a71786c59b59f9a`.
It runs against freshly compiled production sources. The host native library is
the existing declared initialization adapter; it does not implement Parcel or a
Wi-Fi radio.

The host run passes **1,025 ScanResult-family checks** and the existing **133
Wi-Fi data checks**. It covers all supported channel ranges and conversion
boundaries, special 2.4/6 GHz channels, runtime constant fields, default unknowns,
copy/list/byte ownership, each capability bit, security transitions and unknown
AKMs, Passpoint ordering, MLO values and MAC-address redaction. The negative
control replaces only ScanResult with the baseline after compiling the current
dependencies; the same SDK caller then fails at the missing channel method.

The native fixture additionally checks actual Parcel round trips for empty and
populated scans, information elements and MLO values, exact stream consumption,
an independent reader of the pinned ScanResult wire order and an independent
minimal wire producer. `WifiDataContractTest` calls it, so the existing Windows
`96-deep-compatibility.ps1` gate runs it with the real android_runtime. These
native checks were compiled and wired but have not been executed locally.

All **282 current production app Java sources** compile against Android core and
framework inputs, and the production application D8 conversion passes. All
**149 service fixture sources** compile and their D8 conversion passes.
Compilation reuses the verified framework/core artifact from main `db5e76bd`
and an existing 85-class generated dependency jar. This is not a freshly rebuilt
exact-current full platform runtime or a Windows/ART execution result.

Reproduce the host gate:

```sh
python3 tools/test_wifi_data_contracts.py \
  --java-home "$JAVA_HOME" --sdk-jar "$SDK_JAR" \
  --framework-classes "$FRAMEWORK_CLASSES" --core-classes "$CORE_CLASSES" \
  --inventory-output selected-scan-declarations.json
```

For the expected failing linkage control, omit `--inventory-output` and add
`--scanresult-source-ref 0af3fd8333e43cdeb7a3b4bcd0403758e6d556c2`.

### Reference Android execution

The same fixture has an `app_process` entry point. Compile only
`tests/dex-services/gesher/services/WifiScanResultContractTest.java` against the
official SDK, dex its classes with D8 and package the resulting classes.dex as
`scanresult-reference.jar`. On a disposable API 36 reference device/emulator:

```sh
adb push scanresult-reference.jar /data/local/tmp/scanresult-reference.jar
adb shell 'CLASSPATH=/data/local/tmp/scanresult-reference.jar app_process /system/bin gesher.services.WifiScanResultContractTest'
```

app_process initializes the device's real native runtime. Hidden data setters
are accessed reflectively only to supply isolated producer metadata; those
reflection calls are not claims about a normal APK's hidden-API access. Record
the reference image, API/main+minor, module versions and execution result. No
reference-device or physical-radio result is claimed by this change.
