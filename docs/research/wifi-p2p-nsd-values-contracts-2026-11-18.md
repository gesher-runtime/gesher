# Wi-Fi Direct service-discovery values: Android16-r4 contracts

This bounded A02 increment supplies the SDK36 Wi-Fi Direct service-discovery
value and request objects used to describe DNS-SD, UPnP and USD services.
Applications can construct and inspect the values, generate their query data,
compare them according to Android's contracts, and use the original Parcelable
readers and writers. It does not implement service registration, discovery,
radio operations, a `WifiP2pManager`, or a Windows Wi-Fi Direct backend.

Seven complete genuine source files provide eight SDK36 types and all 40
declared public/protected members in the selected official SDK jar. The exact
declaration denominator is separate from behavioral and native execution
evidence. This is an incremental part of issue #56, not completion of A02.

## Profiles and source provenance

| Evidence dimension | Selected profile |
| --- | --- |
| Project source | Android16-r4 / API36.1 source profile from `aosp.lock` |
| Wi-Fi source | `platform/packages/modules/Wifi`, commit `e3f09203409da7a91d2c5cf9f0109da6d5243319` |
| TXT helper source | `platform/packages/modules/Connectivity`, commit `f930245ec39a510f37a9a7dfaded96d287491a61` |
| Public SDK artifact | SDK36r02, SHA-256 `d9eb9da824d9e247a352f570f01e1169e725b2954bca9e283a71786c59b59f9a` |
| Selected Wi-Fi release flag | `bp4a`, inherited READ_ONLY `wifi_direct_r2=true` |
| Stock Android oracle | API36.0 profile; its ordinary legacy observations are recorded separately |
| Windows runtime | Exact pinned value profile and coherent production boot graph required; execution remains pending |

The sources are the complete files under the pinned Wi-Fi
[NSD source directory](https://android.googlesource.com/platform/packages/modules/Wifi/+/e3f09203409da7a91d2c5cf9f0109da6d5243319/framework/java/android/net/wifi/p2p/nsd/).
Raw Git blob IDs, raw and adapted SHA-256 hashes, the capsule artifacts, release
inputs and adaptation counts are in the
[provenance record](wifi-p2p-nsd-values-provenance-2026-11-18.json).
Original Apache-2.0 notices remain in every imported file.

| SDK36 type | Declared public/protected members |
| --- | ---: |
| `WifiP2pServiceInfo` | 10 |
| `WifiP2pServiceRequest` | 8 |
| `WifiP2pDnsSdServiceInfo` | 1 |
| `WifiP2pDnsSdServiceRequest` | 3 |
| `WifiP2pUpnpServiceInfo` | 1 |
| `WifiP2pUpnpServiceRequest` | 2 |
| `WifiP2pUsdBasedServiceConfig` | 11 |
| `WifiP2pUsdBasedServiceConfig.Builder` | 4 |
| **Total** | **40** |

The public SDK already exposes the base Info/Request constructors and getters
using `WifiP2pUsdBasedServiceConfig`. The selected source profile also writes
nullable Config and session state for ordinary legacy values. Including the
complete Config and Builder is therefore necessary to preserve both SDK36 API
and the ordinary record layout. The Config's implicit public no-argument
constructor is part of the SDK denominator. Newer USD discovery/advertisement
wrappers and service-response types are outside this increment.

The base Info/Request `CREATOR` fields, protected constructors, supplicant
queries, transaction IDs and service-session IDs are relevant hidden source
contracts. A separate source-level fixture covers these; they are not silently
added to the 40-member public SDK denominator.

## Complete helper and production providers

DNS-SD encoding uses the genuine complete
[`DnsSdTxtRecord`](https://android.googlesource.com/platform/packages/modules/Connectivity/+/f930245ec39a510f37a9a7dfaded96d287491a61/staticlibs/framework/com/android/net/module/util/DnsSdTxtRecord.java),
raw SHA-256 `65b124f656064af1dee6de5646305ac9b06668ae7fc2a423c0a84a56ec27962f`.
The existing framework supplies this class and its CREATOR; the app adds no
duplicate helper. Pinned framework build metadata selects
`net-utils-framework-common-srcs`, whose Connectivity filegroup includes the
complete helper. The pinned framework jarjar rules do not rename it.

The retained older distribution's framework DEX contains both helper classes,
and its app DEX contains neither. Freshly compiling the complete pinned helper
with the corresponding Java language mode and inline string concatenation
produced exactly matching `javap -c -p` bodies for both cached helper classes.
The raw classfile bytes differ because the cached and fresh classfile target
versions differ; the evidence does not claim byte identity or a fresh Windows
framework build. The source preserves its Apple copyright and Apache-2.0
license notice.

The production provider census is in
[the provider record](wifi-p2p-nsd-values-provider-census-2026-11-18.json).
The unchanged full app build produces 2,462 DEX classes, including all eight
selected SDK types and all 40 declarations. The unchanged service-test build
produces 1,705 DEX classes. The new service tests do not redefine the value
classes or the helper. No `WifiP2pManager` definition is introduced.
These are fresh app and test artifacts against retained genuine framework/core
inputs, not a newly built full Windows distribution.

For isolated native Android comparisons, the complete pinned helper is freshly
compiled into each source-owned candidate/reference graph. The loader owns all
selected NSD classes, nested classes, original CREATORs and this helper, with no
fallback for those names. Genuine boot `Parcel` and `Parcelable` remain outside
that graph. The stock profile instead uses the stock boot implementations and
contains no replacement Android value classes.

## Enumerated build adaptations

`audit_wifi_p2p_nsd_sources.py` compares the complete significant Java token
stream after these explicit adaptations:

1. Remove unavailable `FlaggedApi` and `RequiresApi` build annotations and
   imports. No annotated method or constructor is removed.
2. Inline the exact `Environment.isSdkAtLeastB` predicate in the two base
   classes: codename `Baklava` or `SDK_INT >= BAKLAVA`.
3. Select the verified `bp4a` READ_ONLY `wifi_direct_r2=true` build value for the
   two base Parcel guards. This is a source profile selection, not a claim of
   radio capability or available discovery operations.
4. Remove three unused `WifiP2pManager` imports referenced only by Javadoc.

All other fields, constructors, validation, query encoding, aliases,
`equals`/`hashCode`, and original Parcel bodies remain. The DNS-SD and UPnP
service-info files are byte-identical to their pinned originals. The isolated
reference uses the complete pinned `Environment` and flag implementations
freshly generated from the genuine pinned Wi-Fi declarations and release data.

## Preserved application-visible value behavior

The host and native fixtures exercise the ordinary factories, controlled API
rejections, and these distinctions from the original source:

- Generic service requests preserve the supplied hexadecimal query's case.
  Null and empty queries remain unequal even where their hashes and supplicant
  representations collide. Protocol values, exact little-endian query prefixes,
  transaction IDs and session state retain the source behavior.
- DNS-SD service records preserve PTR instance spelling while the TXT query
  follows the source's `Locale.ROOT` lowercasing. Map iteration order determines
  TXT encoding order, so ordered maps are used for deterministic fixtures.
  Null/empty TXT maps, null TXT values, and empty TXT values remain distinct
  where the source distinguishes them. Factory output snapshots the map into
  query strings.
- UPnP factories preserve validated UUID/device/service text and service-list
  order when constructing query strings. Their ordinary null/empty inputs and
  Java UUID validation behavior are retained.
- The protected Info list constructor and getter preserve their list alias;
  factory-generated strings remain independent of later caller collection
  changes. Equality and hashing retain their actual source fields, including
  exclusion of transaction/session IDs.
- Config uses identity equality and identity hashing inherited from `Object`.
  Builder construction and getters retain byte-array aliasing; original Parcel
  reconstruction creates a new Config and a fresh byte array. The Config name,
  protocol and byte-array limits are checked by ordinary constructor/builder
  calls, including the 100 UTF-16-unit name and 1,024-byte information bounds.
- DNS-SD/UPnP subclasses inherit the base Parcelable readers. A subclass writer
  is consequently reconstructed as the base Info or Request type. Tests check
  the actual restored type rather than inventing subtype-preserving readers.
- A USD Info starts with a null query list. Its original CREATOR allocates an
  empty list and reads into it, so the reader's rewrite changes the initial
  list-length word from `-1` to `0`. The fixture requires exactly that change,
  with every other byte preserved. It records distinct initial and rewritten
  hashes and does not force equality between the original and restored value.

These are value/data operations. No network packet, manager call or radio
operation is sent by the fixtures. Oversized or invalid examples are bounded
ordinary API-rejection calls and are never serialized to a device.

## Verification and native gates

The SDK-facing caller is compiled against the official SDK jar alone. It runs
125 host checks over the fresh candidate classes. The separately compiled
source-level fixture runs 44 host checks. An independently compiled complete
pinned-source graph passes the same 169 host checks. The host uses the existing
genuine TXT helper; it supplies no substitute `Parcel`, so these host results
explicitly report `parcel=false`.

The exact integrated pre-change source lacks all seven files. The absence
control proves that source absence and fails with the missing value family,
while explicitly forbidding fallback to cached compile-only SDK stubs. The
candidate full app/services build succeeds and its DEX census verifies the
real providers and the 40 declarations.

The frozen Android capsule contains three independently identifiable profiles:
13 ordinary legacy native cases on stock API36.0, and 20 complete original-reader
cases on each matching pinned-r4 and candidate graph. Native execution records
and their exact byte hashes are retained separately in
[the Android reference](wifi-p2p-nsd-values-android-reference-2026-11-18.json).
The SDK36.0 stock profile is not substituted for the selected r4/bp4a profile,
and optional stock USD presence does not qualify USD behavior there.

| Executed profile | Public checkpoint | Extended checkpoint | Complete native reader cases | Result |
| --- | ---: | ---: | ---: | --- |
| Stock Android36.0, legacy corpus | 68 | 91 | 13 | Passed |
| Genuine pinned Android16-r4/bp4a value graph | 189 | 127 | 20 | Passed |
| Candidate value graph | 189 | 127 | 20 | Passed |

The first two numeric columns are the printed checkpoint counters, not an
exhaustive assertion total. The extended fixture invokes the public fixture's
shared `wire` helper after its counter has been printed, so later shared-helper
assertions are absent from the sum. The native coverage denominator is the
explicit 13 or 20 complete original-reader cases.

The root-owned oracle ran these profiles with genuine Android Parcel natives,
SDK_INT 36, UID 2000 and the recorded Android emulator fingerprint. The candidate
matches the pinned reference in all 20 initial and rewritten record hashes;
the 13 stock legacy records also match the common selected records. The stock
Config class is present, while the canonical `com.android.wifi.flags.Flags`
class is absent; the recorded stock result consequently remains the explicitly
selected legacy corpus. It does not claim that stock lacks the Config API or
that its USD configuration is the selected r4/bp4a profile.

The 20 required cases comprise five Config records and fifteen source-level
Info/Request records. Each successful observation requires a genuine native
write, original full CREATOR read, complete data consumption, and rewrite.
There are no writer-only cases in this increment. PR #150's separate DeviceList
writer-only observations are not counted as NSD reader evidence.

Track96 invokes the production boot graph and requires exactly the selected
profile, provider checks, both fixture completion markers, all 20 ordered
records, and exact initial/rewrite byte lengths and SHA-256 hashes. Missing
records, duplicate or out-of-order records, different profiles, writer-only
markers, false round trips and changed rewrite bytes fail the gate. The original
process exit code remains part of the Track96 result. PowerShell checker tests
use constructed logs only and do not count as native Windows execution.

Actual Windows execution remains a required gate. This increment makes no
claim of Wi-Fi Direct radio support, working service discovery, Windows runtime
qualification, API37 support, or completion of the surrounding compatibility
roadmap.

## Reproduction

See [the fixture README](../../tests/wifi-p2p-nsd/README.md) for host checks,
complete-source verification, local capsule preparation, full-build inputs and
the Windows gate. The preparation tool compiles local artifacts and writes
explicit setup/run/cleanup requests; it does not invoke or queue device access.
