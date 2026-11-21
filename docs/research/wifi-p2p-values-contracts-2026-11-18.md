# Wi-Fi Direct values: SDK36 and pinned Android16-r4

## Bounded increment and source inputs

This A02 increment for issue 56 supplies seven missing value source files,
covering eight SDK36 binary types and 172 public/protected declarations. It
does not implement WifiP2pManager operations, peer/service discovery,
connection, group creation, service registration or a radio feature declaration.
Constructing a request or copying supplied fields does not report a successful
radio operation.

The source combines exact PR144 dependencies with the verified main snapshot:

| Input | Remote commit | Local source mapping | Complete tree |
| --- | --- | --- | --- |
| PR144 | `c6731c78944abb35814b6227e3337fbf2dc90c74` | `c6af607b9f09a62f5642404cc3773107f35f871a` | `6198e54c76b58bf8d4cb093e4420d533783f291a` |
| Main | `1e14cbb8629e1defba0009f58996630e51bc0c19` | `ab254299ee7c66bc2d47e2e63d23e31e8821d7e7` | `a33e0765eaf122320e93c8ef3dea8d9922aa50fe` |
| Clean local integration | No reconstructed remote commit metadata | `f0dce23e5418add5af38dd7762a061593b3751f1` | `1632bf0e0f49b470348ba77028bb3d6ecf805d19` |

The main snapshot's thirteen changed blobs and complete tree were verified
against the selected-account GitHub response. Both parents' workflow additions
and existing Wi-Fi dependencies were preserved. WpsInfo comes from PR144;
ScanResult/InformationElement from its PR112 dependency; OuiKeyedData and
ParcelUtil from PR127. MacAddress remains the existing app provider. No
duplicate dependency implementations are introduced.

During finalization, main advanced to
`b67a818925df72b86557405405e375ed5a750f9c` through the unrelated JobScheduler
merge. The selected-account ownership refresh found no P2P owner. This work
keeps the input pair above and does not claim integration of that later delta.

## Actual providers and the SDK boundary

The retained distribution from workflow run `44208723771`, artifact
`14386510665`, source `63d8db3e8e4a7c992d8daa0c027860bb759dcdaa` and checkout
`76ac2cd2be8469ea88acc9145c4c51ffeb313a96` contains none of the 41 SDK36 P2P
types in its framework/app DEX archives. Framework has 34,764 definitions and
SHA-256 `fed525b7fd92cfe2b82ac547172d545dd2f4073a6bb67740906b15ec7c8c1bb7`;
app has 2,403 and SHA-256
`6ea29dee02b9ac9239a043bdf169aff73ead17dd10f5fd060792acc4a691441a`.
These are older distribution artifacts, not a fresh full Windows build.

Cached framework compilation classes include P2P declarations that throw
`RuntimeException("Stub!")`. They are not runtime providers. Current source
selection excludes those module stubs from framework-minus-apex, and the app
build does not copy arbitrary Wi-Fi compile stubs into its DEX. The new files
enter gesher-app through its normal source build.

Official SDK36 SHA-256 is
`d9eb9da824d9e247a352f570f01e1169e725b2954bca9e283a71786c59b59f9a`.
The member audit checks exact descriptors, access, flags and ancestry in fresh
classes, and the production DEX census checks the same declarations again.

| SDK36 type | Public/protected declared members |
| --- | ---: |
| WifiP2pConfig | 33 |
| WifiP2pConfig.Builder | 12 |
| WifiP2pPairingBootstrappingConfig | 11 |
| WifiP2pDevice | 32 |
| WifiP2pWfdInfo | 45 |
| WifiP2pDeviceList | 8 |
| WifiP2pGroup | 22 |
| WifiP2pInfo | 9 |
| Total | 172 |

This is a presence census, not a behavioral coverage percentage. The generic
`Device.getVendorElements(): List<ScanResult.InformationElement>` dependency
is also recorded; erased descriptors alone would miss it.

The r4 Pairing source additionally contains `getPairingBootstrappingMethod()`
and `getPairingBootstrappingPassword()`, both absent from SDK36. A bounded
comparison with stable SDK37.0 revision 2, SHA-256
`bf1b4387cc7ca94fc6ef684f040d9d16fbf16248e181819f020736ea2053f177`, confirms
exactly those two additional declarations. They remain source-preserved but
are outside the 172-member denominator. No API37 runtime qualification is claimed.

## Pinned source adaptations

The implementations come from the official
[Android16-r4 Wi-Fi sources](https://android.googlesource.com/platform/packages/modules/Wifi/+/e3f09203409da7a91d2c5cf9f0109da6d5243319/framework/java/android/net/wifi/p2p/),
commit `e3f09203409da7a91d2c5cf9f0109da6d5243319`. Original Apache-2.0
headers remain; WifiP2pInfo is byte-identical. The reproducible source audit
compares complete non-comment token streams, preserving literals and operator
boundaries, after these enumerated adaptations:

1. Remove unavailable FlaggedApi/RequiresApi build annotations and imports.
   Retain the actual runtime SDK guards.
2. Use the genuine framework jarjar namespace for SdkLevel,
   `android.internal.modules.utils.build.SdkLevel`.
3. Inline the exact
   [Environment.isSdkAtLeastB predicate](https://android.googlesource.com/platform/packages/modules/Wifi/+/e3f09203409da7a91d2c5cf9f0109da6d5243319/framework/java/android/net/wifi/util/Environment.java):
   codename Baklava or SDK at least BAKLAVA. This avoids unrelated APEX/filesystem
   helpers. It is an actual predicate adaptation, not annotation-only removal.
4. Select Config/Group's genuine `wifi_direct_r2=true`, READ_ONLY build value.
   Official
   [bp4a](https://android.googlesource.com/platform/build/release/+/0998133091794b56253fd6bd5c33ed0e0e5bd9de/release_configs/bp4a.textproto)
   inherits bp3a/bp2a; the
   [bp2a value](https://android.googlesource.com/platform/build/release/+/0998133091794b56253fd6bd5c33ed0e0e5bd9de/aconfig/bp2a/com.android.wifi.flags/wifi_direct_r2_flag_values.textproto)
   enables this flag read-only. Genuine repository aconfig generation verifies
   the profile independently. This selects validation/wire behavior, not radio
   capability.
5. Omit the hidden Device.dirInfo field, including its annotation, and the one
   corresponding DeviceList.updateSupplicantDetails assignment. These are
   **two sites**. DIR is not globally unused: that assignment is a real upstream
   use. The field is absent from SDK36 Device and has no retained public
   observer, copy or Parcel state. The adjacent pairing-method update remains
   and is covered by the source-level corpus. Broader DIR support is outside
   this eight-type closure.

The initial investigation described DIR as unused too broadly; the two-site
audit corrects that claim. It also understated the R2 password boundary: the
actual source accepts 128 characters and rejects 129, while legacy build-time
validation accepts lengths 8 through 63. The implementation retains that
timing and does not add an ASCII restriction.

The token audit found an orphaned Nullable annotation left by the initial DIR
field removal. That annotation was removed before any Android execution. The
unexecuted v1 capsule remains frozen; v2 contains the corrected final source.

## Value contracts

Config preserves builder validation, WPS/pairing interaction, persistence,
band/frequency and IP-provisioning fields. Its copy duplicates WpsInfo and
shares the pairing value. Device keeps address-based equality, copies WFD and
list containers, and shares information-element objects and immutable address
objects. Updating a Device shares the supplied WFD value.

DeviceList copies devices in its constructors, exposes a live unmodifiable
view and preserves the original new object in its hidden update path. Group
copies its owner and client container while sharing client elements. Its hidden
interfaceAddress is neither copied nor parceled in r4; public BSSID behavior
follows that source. WfdInfo keeps its masks and real guards. Info retains
ordinary IPv4/IPv6 bytes and the copy's immutable address alias. The tests do
not replace these distinctions with uniform deep copying.

## Native provider boundary

Pinned framework
[Parcel](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/core/java/android/os/Parcel.java)
uses its defining loader when `readParcelable` receives null. The original
DeviceList CREATOR does exactly that. Config, Group and Device instead use
explicit dependency loaders or typed creators where their sources specify them.

The Android candidate and independently compiled r4 profiles own their complete
value graphs in child loaders while retaining genuine boot Parcel. A DeviceList
reader would then produce a stock Device for a child DeviceList. Pre-corpus
identity assertions establish this boundary and prevent that execution. All
three source-owned DeviceList cases are writer-only; no mixed-loader diagnostic
runs and no production Parcel call is patched. All other ordinary cases run
their original readers. This is not a full candidate-family native round trip.

The Windows package's intended topology is different: core-oj, core-libart,
core-icu4j, framework and gesher-app share one `-Xbootclasspath`, as checked in
`tools/package/gesher-app.ps1`. No static loader packaging defect was found.
The required actual Windows fixture must establish Parcel/Device boot identity
before running the complete DeviceList readers. Compilation and topology
inspection do not substitute for this runtime gate.

## Qualification and limits

The host corpus passes 222 checks (193 public caller and 29 source-level
alias/state checks), including 49 runtime constant comparisons. It supplies no
Parcel implementation. The absence control forbids compile-stub fallback.
Native payloads contain no android.os, java or dalvik replacement definitions.
Their provider/type identities are asserted before any ordinary records.

The final app build uses the unchanged app/build.sh over the retained genuine
framework/core compilation inputs: 296 selected app sources, 58 generated
Binder stubs, one generated compatibility table and 27 compatibility sources.
It produces 2,443 DEX classes, including all eight types and 172 declarations.
All 155 service test sources compile into 1,664 DEX classes without duplicating
the P2P implementation. The app build takes 24.32 seconds and the service build
7.52 seconds; Linux child resource accounting reports a cumulative peak of
890,724 KiB. This is a fresh app/service build, not a current full framework or
Windows distribution build.

The adjacent provider, provenance and Android reference JSON files bind exact
source/tool/artifact hashes and observations. Android app_process uses shell
identity; its scope is value/record compatibility, not ordinary APK permissions,
Wi-Fi hardware, Windows identity or service availability. Stock module/profile
results are recorded separately from the matching pinned-r4/candidate profiles.

Root-owned setup 058 verified all four payload hashes on the device before
executions 059–061. The observed device is API36, uid 2000, Dalvik, fingerprint
`Android/sdk_phone64_x86_64/emu64x:16/BE2A.250530.026.D1/13818094:userdebug/test-keys`.
All provider assertions preceded the corpus and all three executions exited 0.

| Profile | Fixture checks reported | Ordinary records | Complete readers | Writer-only records | Elapsed |
| --- | ---: | ---: | ---: | ---: | ---: |
| Installed stock module | 149 | 15 | 15 | 0 | 4.846 s |
| Independent pinned r4/bp4a | 220 + 40 | 23 | 20 | 3 | 4.754 s |
| Candidate | 220 + 40 | 23 | 20 | 3 | 4.476 s |

All 23 candidate/pinned records match in case, length, complete first-write
SHA-256 and reader qualification. Twelve of the fifteen shared stock cases
also match. Stock config_default, config_peer and config_unicode have equal
lengths but different hashes. The hash-only observation does not locate the
differing field or establish a flag cause. Stock contains Pairing on its boot
path, while the queried public `com.android.wifi.flags.Flags` name is absent;
that absence does not prove the flag is disabled. No stock result was rewritten
or forced to match the selected r4 profile.

The [native reference](wifi-p2p-values-android-reference-2026-11-18.json)
retains every case and raw receipt hash. The
[provider census](wifi-p2p-values-provider-census-2026-11-18.json) contains all
172 descriptors, and [provenance](wifi-p2p-values-provenance-2026-11-18.json)
binds source adaptations, inherited inputs, capsule bytes and build results.

Track96 requires actual production Windows execution, the pre-corpus boot
identity assertion, both fixture completions, and all 23 complete byte hashes
matching the independent pinned reference. Every Windows case must complete
a reader/writer round trip. Its checker rejects incomplete/duplicate cases,
writer-only output, wrong profiles and missing provider/completion evidence.
Checker controls use constructed logs and qualify only the gate's logic.
All twenty gate controls pass. Python AST, framework workflow YAML and all three
new or changed PowerShell scripts parse successfully.

Final-source Windows execution remains pending. No canceled workflow was rerun
or manually dispatched. Independent exact-tree QA is required for publication;
merge still requires the production Windows gate.
