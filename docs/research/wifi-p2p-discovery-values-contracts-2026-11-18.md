# Wi-Fi Direct discovery and DIR values — A02 increment

## Scope and result

This increment adds five complete genuine Wi-Fi source files: DiscoveryConfig,
USD local-advertisement configuration, USD discovery configuration, DirInfo and
USD service response. Their builders bring the selected official SDK36r02
denominator to eight types and 41 declared public/protected members. All 41
declarations are present in the fresh app DEX, with the expected access,
descriptors, class kinds and ancestry.

Applications can construct and inspect these values and use their original
Parcelable contracts. The change preserves validation timing, array/list
aliasing, identity equality and the pinned builders' existing behavior. It
does not implement a manager, start discovery or advertising, resolve a peer,
derive a DIR tag, change permissions or report hardware/capability success.
The separately scoped hidden DirInfo carrier sites omitted in earlier Device
work are unchanged.

The stack parent is the independently reviewed PR154 integration, remote
`3d6daaed462f5de676fe7b0de4c14058e9202a11`, tree
`5a4c9b206f70fa3ec576bc2e2ef5305b43e32875`. The integration receipt records real
local parents and their exact remote/tree mapping. No remote commit was
reconstructed. The new production paths were unowned in the 71-PR inventory;
the final 74-PR inventory again found all nineteen new paths unowned. Track96
receives only an additive fixture block. Later main changes through `7537b0c`
concern automerge tooling and do not change this deliberately reviewed source
parent or its value providers.

## Exact SDK denominator and callers

The SDK36r02 jar SHA-256 is
`d9eb9da824d9e247a352f570f01e1169e725b2954bca9e283a71786c59b59f9a`.
The denominator counts declarations on these exact classfiles. It is not a
percentage of Wi-Fi behavior or Android compatibility.

| SDK type | Declared public/protected members |
| --- | ---: |
| `android.net.wifi.p2p.WifiP2pDiscoveryConfig` | 6 |
| `WifiP2pDiscoveryConfig.Builder` | 3 |
| `WifiP2pUsdBasedLocalServiceAdvertisementConfig` | 5 |
| `WifiP2pUsdBasedLocalServiceAdvertisementConfig.Builder` | 3 |
| `WifiP2pUsdBasedServiceDiscoveryConfig` | 6 |
| `WifiP2pUsdBasedServiceDiscoveryConfig.Builder` | 4 |
| `WifiP2pDirInfo` | 8 |
| `android.net.wifi.p2p.nsd.WifiP2pUsdBasedServiceResponse` | 6 |
| **Total** | **41** |

`WifiP2pDiscoveryValuesContractTest` compiles against that SDK alone. Its
disassembly binds 36 declarations directly; the five selected writers execute
through ordinary `Parcelable.writeToParcel` interface dispatch. All five
CREATORs, their array methods and complete readers are exercised by the native
corpus. `WifiP2pDiscoveryValuesExtendedTest` is separately source-compiled for
the hidden vendor list accessors and response constructor. Those hidden members
are excluded from the 41-declaration denominator. The public response observers
receive normally constructed response objects from the extended fixture.

The [provider census](wifi-p2p-discovery-values-provider-census-2026-11-18.json)
contains every SDK-to-DEX member mapping. The
[provenance receipt](wifi-p2p-discovery-values-provenance-2026-11-18.json)
contains source hashes, caller mappings and dependency bindings.

## Contracts preserved

All five top-level classes inherit Object identity equality and hashing. Two
separately constructed values remain unequal even when their fields match.
Tests verify the inherited method owners and stable per-object hash; they do
not assume different objects cannot have colliding identity hash values.

| Value | Pinned observable behavior |
| --- | --- |
| DiscoveryConfig | Builder construction stores the scan type; build validates FULL=0, SOCIAL=1 or SINGLE_FREQ=2. The frequency setter rejects nonpositive values. SINGLE_FREQ requires a supplied frequency. FULL and SOCIAL retain an explicitly supplied positive frequency. The builder aliases its vendor list; build makes a shallow list copy; the getter returns a mutable list and retains the element identities. Duplicate OUIs remain allowed. |
| USD advertisement config | Defaults to 2437 MHz. The setter rejects nonpositive values and otherwise retains the supplied integer. It does not perform a hardware/regulatory availability check. |
| USD discovery config | Defaults to UNSPECIFIED=-1 with frequency array `{2437}`. Frequency arrays of length 1..48 are accepted and aliased through the setter, constructor, repeated builds and getter. The source adds no per-element frequency validation. The band mask permits the 2.4/5/6 GHz bits and their combinations. |
| DirInfo | Stores the supplied MacAddress and nonce/tag array references directly. NonNull annotations do not add constructor validation. Getters return those same references. The reader reconstructs the MAC with its genuine CREATOR and reads fresh byte arrays. It performs no identity resolution or HMAC computation. |
| USD response | Its hidden constructor stores protocol and nullable information array without added validation. Public getters expose those fields and retain the array alias. The reader creates a new response and a fresh array when present. |

The USD discovery builder has a significant pinned behavior: `setBand` does
not clear its default frequency array. Calling `build` after setting any valid
band therefore throws `IllegalStateException` for mutually exclusive band and
frequency fields. The same applies when explicit frequencies and a band are
set in either order. This increment preserves that source behavior. The native
corpus constructs valid frequency-based configurations; it does not manipulate
private fields to manufacture a successful band-only configuration.

Exact exception classes/messages, rejection timing, state retained after a
rejected setter, complete strings, defaults and aliases are checked against
fresh candidate and fresh pinned-source classes. Host-only checks also observe
the original lack of added constructor/per-element validation. Those values
are not serialized or sent to a device.

## Pinned source and build adaptations

All five values come from Wi-Fi commit
`e3f09203409da7a91d2c5cf9f0109da6d5243319`, the project's Android16-r4 source
profile. Original Apache-2.0 notices remain. These are the complete files:

- [WifiP2pDiscoveryConfig.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/e3f09203409da7a91d2c5cf9f0109da6d5243319/framework/java/android/net/wifi/p2p/WifiP2pDiscoveryConfig.java)
- [WifiP2pUsdBasedLocalServiceAdvertisementConfig.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/e3f09203409da7a91d2c5cf9f0109da6d5243319/framework/java/android/net/wifi/p2p/WifiP2pUsdBasedLocalServiceAdvertisementConfig.java)
- [WifiP2pUsdBasedServiceDiscoveryConfig.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/e3f09203409da7a91d2c5cf9f0109da6d5243319/framework/java/android/net/wifi/p2p/WifiP2pUsdBasedServiceDiscoveryConfig.java)
- [WifiP2pDirInfo.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/e3f09203409da7a91d2c5cf9f0109da6d5243319/framework/java/android/net/wifi/p2p/WifiP2pDirInfo.java)
- [WifiP2pUsdBasedServiceResponse.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/e3f09203409da7a91d2c5cf9f0109da6d5243319/framework/java/android/net/wifi/p2p/nsd/WifiP2pUsdBasedServiceResponse.java)

The complete significant-token audit permits only unavailable FlaggedApi and
RequiresApi annotations/imports, their now-unused Flags import, four
SOURCE-retained manager scan annotations, three compile-time manager constant
references and the genuine framework jarjar SdkLevel import. No dynamic flag
call is removed: these five source bodies contain none. Original fields,
constructors, validation and complete reader/writer bodies remain.

The full pinned manager source supplies FULL=0, SOCIAL=1 and SINGLE_FREQ=2.
Both the official SDK and cached framework compile class independently expose
those values. The candidate uses 0 once and 2 twice in the validation body.
The pinned reference retains the original qualified manager constants and
compiles them against the official SDK class, isolated from its runtime
payload. The manager is never introduced as a production definition.

The existing ScanResult supplies compile-time band constants. The audit follows
the exact raw chain from WifiScanner band indices 0/1/3 to shifts 1/2/8, then
ScanResult's references, then its already-qualified production literals.
UNSPECIFIED=-1 is also verified. Scanner operations are not added.

## Provider boundaries

| Dependency | Production app/framework | Fresh host comparison | Isolated Android comparison |
| --- | --- | --- | --- |
| Five values, builders and original CREATORs | New complete app definitions | Fresh selected candidate or pinned source | Fresh selected candidate or pinned source, closed owned loader |
| OuiKeyedData and PersistableBundleUtils | Existing complete app providers | Fresh complete source providers | Fresh complete source providers in the same loader as DiscoveryConfig |
| SdkLevel | Existing complete genuine framework provider | Complete freshly compiled pinned source with framework jarjar namespace | Same complete freshly compiled helper, confined to isolated graph |
| MacAddress | Existing genuine app provider | Existing app classes jar, identity checked | Explicit genuine Android boot leaf, constructor/getter/CREATOR identity checked |
| Parcel, Parcelable and PersistableBundle | Existing framework/native providers | Genuine framework classes; no host Parcel execution | Genuine Android boot/native providers |
| Manager and ScanResult constants | Compile-time constants only | SDK manager reference constants independently bound; no new runtime definitions | No manager or ScanResult definitions packaged |

Complete SdkLevel is pinned to modules-utils commit
`e945bbce0ccb83e1111b6031a3bba89e6b5cac6f`, raw SHA-256
`74580e2fcc7cac5b405ae727e90a1ce1530e02d944cee375513ce29ad785b929`.
Its only source changes are the actual framework jarjar package relocation and
unavailable AndroidX build/nullability annotations. A fresh compilation has
exactly the cached genuine provider's `javap -c -p` output. Classfile bytes are
not claimed identical across compiler targets.

The Android graph owns D8's four empty synthetic private-constructor access
markers as well. The build checks their exact synthetic/final/public flags,
Object ancestry and lack of members. Before any record, the launcher checks
the selected classes and original CREATORs, typed vendor-list element identity,
boot PersistableBundle constructor/getter identity and the DirInfo MacAddress
boundary. Owned names cannot fall back to boot classes or SDK stubs.

The separate stock artifact contains only callers. Stock Android36.0, pinned
Android16-r4/API36.1 source, SDK36r02 and SDK37 remain distinct profiles. Stock
presence is recorded for every selected type. Modern absence attempts only six
common DiscoveryConfig records, with coverage claimed only after actual
execution. The public response observer accepts Parcelable and casts inside
the modern-only path, avoiding eager public-signature resolution. Private
lambda descriptors still mention modern builders; successful host reflection
does not guarantee ART verification behavior. Any stock behavior difference
is reported separately. The observed stock image contained all eight types and
ran the complete 17-case profile, so the modern-absent fallback was not exercised.

## Validation and limits

Fresh candidate and pinned host graphs each pass 177 shared checks. The counter
is printed once, after both public and hidden suites finish. The host supplies
no Parcel or Wi-Fi value implementation. The exact old-source control proves
all five source paths absent and fails in the public caller's baseline body at
the absent DiscoveryConfig.Builder; its closed loader forbids compile-stub
fallback even with the new full app jar present on the parent classpath.

Unmodified full app/services scripts pass against the retained genuine
framework/core inputs. The app DEX contains 2,478 classes and the services DEX
1,720. All eight selected types and 41 declarations are verified in the app;
the tests introduce no selected value/helper definitions. There is no new app
manager, Parcel or SdkLevel definition. The build receipt binds 561 input
files. After the fixture-only signature correction, the full services build
was repeated. All app inputs were unchanged and its original artifacts were
verified byte-identical. This is an app/services build, not a newly built
framework or Windows distribution.

The native corpus has 17 normal records: four scan configurations, two
advertisement configurations, three frequency-based USD discovery configs,
three normal DIR values, two populated vendor lists and three responses with
null, empty and populated information. Each claimed case requires complete
original reader consumption and byte-identical rewrite. The full-reader gate
accepts no writer-only cases.

Independent capsule preflight verified the official Git blobs for the five
values and six support sources, all twenty frozen source bindings, 561 build
inputs and eight byte-identical rebuilt artifacts. The oracle owner then ran
these profiles with genuine Android boot Parcel/PersistableBundle/MacAddress:

| Observed profile | Fixture checks after both suites | Full original-reader cases | Initial/rewrite records matching the other profiles |
| --- | ---: | ---: | ---: |
| Stock Android 36.0 | 330 | 17 | 17 |
| Pinned Android16-r4 sources | 330 | 17 | 17 |
| Candidate sources | 330 | 17 | 17 |

All initial and rewritten SHA-256 digests match across the three profiles.
All eight selected types were present on the stock image; modern=true was
observed for every run. The 330 count is the shared fixture counter printed
after both suites, including their reader checks; launcher/provider checks
are separate. Setup passed all seven steps with matching device artifact
hashes, and cleanup passed both steps. No networking or radio operation ran.

The observed fingerprint is
`Android/sdk_phone64_x86_64/emu64x:16/BE2A.250530.026.D1/13818094:userdebug/test-keys`,
SDK36, UID2000. This does not establish ordinary APK permission behavior.
The [immutable Android reference](wifi-p2p-discovery-values-android-reference-2026-11-18.json)
records all seventeen ordered reader types, lengths, initial/rewrite hashes,
profile artifact hashes and raw execution-receipt bindings. The first four
capsule versions were never executed on the device; preparation checks and
the corrected public-signature dependency remain separate evidence.

Track96 adds a production-provider check followed by the exact 17-case gate.
Every original byte of the previous Track96 script is preserved around the
added block. The checker rejects missing/duplicate/reordered records, wrong
reader classes, wrong initial or rewritten hashes, host-only markers, incorrect
profiles and a counter printed before all records. All three PowerShell files
pass AST parsing, and the evidence checker passes 44 acceptance/rejection
checks using the retained reference. Those logic checks are separate from
actual Windows ART/Parcel execution, which remains an explicit required gate.
The checker also rejects the actual isolated Android candidate log as Windows
production evidence because that run has a different provider boundary.
These values do not close A02, SDK37 or issue56.

See the [fixture guide](../../tests/wifi-p2p-discovery/README.md) for source
inputs, fresh graph commands, absence control and capsule preparation.
