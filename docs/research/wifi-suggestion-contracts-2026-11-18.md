# A02: SDK36 suggestion and Passpoint values

This increment supplies the complete selected `WifiNetworkSuggestion`/`Builder`
and Passpoint value family needed to construct, inspect, copy and parcel network
suggestions. It is a bounded increment of [issue #56](https://github.com/gesher-runtime/gesher/issues/56).
It does not complete A02, N45 or a Wi-Fi backend.

## Exact scope and prerequisites

The baseline is published [PR129](https://github.com/gesher-runtime/gesher/pull/129),
commit `d733cc0ffbff9e51892914c726c85b0da5f3c53c`,
tree `b5ddb5f5cba80e0394eb92cac9905db45dbfd4b6`.
Its Configuration, Enterprise, SecurityParams, ParcelUtil, WifiInfo, SSID and
related value providers are reused unchanged. The earlier scan/connection value
prerequisites remain in that baseline. The sibling
[specifier PR141](https://github.com/gesher-runtime/gesher/pull/141) and
[SoftAP/WPS PR144](https://github.com/gesher-runtime/gesher/pull/144) are separate
topics. This family has no `@ChangeId` declarations and therefore needs no new
CompatChanges generator input.

The official SDK36 `android.jar` SHA256 is
`d9eb9da824d9e247a352f570f01e1169e725b2954bca9e283a71786c59b59f9a`.
Freshly compiled provider class files satisfy these declared public/protected
members, with matching class kind, ancestry, descriptors, access and relevant
member flags:

| Selected SDK type | Declarations |
|---|---:|
| `WifiNetworkSuggestion` | 30 |
| `WifiNetworkSuggestion.Builder` | 31 |
| `PasspointConfiguration` | 18 |
| `HomeSp` | 20 |
| `Credential` | 24 |
| `Credential.UserCredential` | 16 |
| `Credential.CertificateCredential` | 12 |
| `Credential.SimCredential` | 12 |
| **Selected total** | **163** |

This denominator describes these eight types only. It is neither an Android
framework denominator nor a whole-runtime coverage percentage. The SDK's two
Suggestion randomization constants are checked against runtime reflection as
well as SDK-inlined public call sites.

The actual selected distributable `framework.jar` and `gesher-app.jar` were
reparsed from their DEX definitions. None defines these selected types, hidden
Policy/RoamingPartner/UpdateParameter, BuildProperties or EAPConstants. Those
archives have SHA256
`fed525b7fd92cfe2b82ac547172d545dd2f4073a6bb67740906b15ec7c8c1bb7` and
`6ea29dee02b9ac9239a043bdf169aff73ead17dd10f5fd060792acc4a691441a`.
They come from run `44208723771`, artifact `14386510665`,
source `63d8db3e8e4a7c992d8daa0c027860bb759dcdaa` and checkout
`76ac2cd2be8469ea88acc9145c4c51ffeb313a96`. This is a selected two-archive
defining-provider audit, not a census of every possible boot-path artifact.
Framework compilation classes and official SDK stubs are not runtime evidence.

Fresh remote ownership checks found 72 open PRs and live main
`473beec761bd3005a23a8f746a6ba9216343f39d`. Actual changed paths were compared with
the saved complete inventory; the final refresh includes new PR146 and moved
heads143/137/135/131/94 after the earlier PR144/145/110/76 refresh. There is no
other Suggestion/Passpoint production owner. Shared
framework workflow and deep Windows track additions preserve the existing gates.

## Implementation and provenance

The eight new Java files come from Android16-r4 Wi-Fi module commit
`e3f09203409da7a91d2c5cf9f0109da6d5243319`:

- [WifiNetworkSuggestion](https://android.googlesource.com/platform/packages/modules/Wifi/+/e3f09203409da7a91d2c5cf9f0109da6d5243319/framework/java/android/net/wifi/WifiNetworkSuggestion.java)
  supplies the full Builder and suggestion value.
- [PasspointConfiguration](https://android.googlesource.com/platform/packages/modules/Wifi/+/e3f09203409da7a91d2c5cf9f0109da6d5243319/framework/java/android/net/wifi/hotspot2/PasspointConfiguration.java),
  [HomeSp](https://android.googlesource.com/platform/packages/modules/Wifi/+/e3f09203409da7a91d2c5cf9f0109da6d5243319/framework/java/android/net/wifi/hotspot2/pps/HomeSp.java)
  and [Credential](https://android.googlesource.com/platform/packages/modules/Wifi/+/e3f09203409da7a91d2c5cf9f0109da6d5243319/framework/java/android/net/wifi/hotspot2/pps/Credential.java)
  supply the complete public Passpoint graph.
- [Policy](https://android.googlesource.com/platform/packages/modules/Wifi/+/e3f09203409da7a91d2c5cf9f0109da6d5243319/framework/java/android/net/wifi/hotspot2/pps/Policy.java)
  and [UpdateParameter](https://android.googlesource.com/platform/packages/modules/Wifi/+/e3f09203409da7a91d2c5cf9f0109da6d5243319/framework/java/android/net/wifi/hotspot2/pps/UpdateParameter.java)
  provide the actual hidden validation/copy/Parcelable dependencies.
- [BuildProperties](https://android.googlesource.com/platform/packages/modules/Wifi/+/e3f09203409da7a91d2c5cf9f0109da6d5243319/framework/java/android/net/wifi/util/BuildProperties.java)
  and [EAPConstants](https://android.googlesource.com/platform/packages/modules/Wifi/+/e3f09203409da7a91d2c5cf9f0109da6d5243319/framework/java/android/net/wifi/EAPConstants.java)
  are complete genuine helpers.

All upstream Apache2 notices remain. HomeSp, UpdateParameter, BuildProperties
and EAPConstants are byte-identical copies. The other changes are limited to
unavailable build annotations/imports, the real jarjar name for SdkLevel, the
equivalent existing zero-MAC constant, and defining-loader resolution in twelve
nested Parcelable reads. The new values' complete writer bodies are unchanged.

The explicit loader selects the provider that defines the outer value. It
preserves normal single-provider boot behavior and keeps nested values coherent
when a compatibility provider is loaded separately. It changes neither the
wire format nor field order and does not alter Parcel's global creator caches.

The original Builder handles SSID and binary WifiSsid values, BSSID, open/OWE,
WPA2/WPA3/WAPI passwords and enterprise configurations, Suite-B certificate
requirements, Passpoint, interaction/autojoin/sharing rules, priority/group,
carrier/subscription identity, restricted/untrusted requests, MAC randomization
and Wi-Fi7 preference. API-level guards remain real SdkLevel checks.

Passpoint retains required HomeSp and credential validation, UTF-8 byte/count
limits, realms, SIM value syntax, TTLS inner methods, real X.509 SHA256 matching,
TLS bounds, policy/update validation, trust-root maps and decorated identities.
BuildProperties reads actual `Build.TYPE`; the upstream eng/userdebug exception
for carrier-merged non-enterprise values is preserved. No flag is forced.

Copy and ownership behavior deliberately follows upstream. For example,
credential fingerprints and HomeSp OI arrays are copied by copy constructors,
while some Passpoint maps/arrays retain upstream aliasing or unmodifiable live
views. Suggestion's enterprise and Passpoint setters copy inputs; their getters
expose the Builder-owned values. Equality and hash behavior are preserved,
including the upstream trust-root map's array-identity hash caveat.

## Qualification

The detailed source, SDK and Android receipts accompany this report. The
[fixture instructions](../../tests/wifi-suggestion/README.md) describe reproduction.

| Layer | Result or boundary |
|---|---|
| Full production compilation | Fresh compilation of all 295 app sources passed |
| SDK declaration audit | All 163 selected declarations and both constants passed |
| Host value corpus | 291 checks passed over 24 freshly compiled production families |
| Old-source controls | All eight files absent at exact d733; strict Suggestion and Passpoint linkage both fail as intended |
| Service fixture build | Fresh compilation of all 153 service fixtures passed |
| Stock Android native Parcel | 443 checks and 24 complete wire cases passed |
| Independent pinned-r4 native Parcel | 443 checks and 24 complete wire cases passed |
| Candidate Android native Parcel | 443 checks and 24 complete wire cases passed; all 24 first-write encodings equal the pinned reference |
| Required Windows gate | Wired with native completion plus all 24 independent full-wire cases; 14 PowerShell evidence-checker checks passed; actual Windows execution pending |

The shared host native boundary reports `Build.TYPE=unknown` and exercises the
non-debug predicate branch; it is not presented as an actual Android user build.
The real Android image is API36.0, July2025, userdebug. Pinned sources and stock
providers have separately qualified provenance and are not conflated.

The independent pinned reference compiles verified upstream sources, plus
Configuration/SecurityParams as one owner and genuine Environment. Its nested
readers explicitly choose that defining owner; exact adaptation diffs are saved.
All source-owned and platform helper identities are checked before corpus calls.
Original upstream writer bodies are preserved. The excluded compile-only
`WifiManager.ALL_ZEROS_MAC_ADDRESS` declaration resolves to the verified real
platform field, whose type identity and zero value are asserted in the executed
reference launcher.

The other compile-only declaration is `Flags.multiUserWifiEnhancement()Z`.
Reference bytecode contains one direct call, from
`WifiConfiguration.getCreatorUserIdInternal()I`; its only caller in that class is
`getCreatorUserId()I`. Both are absent from the official SDK36 class and from this
163-declaration corpus. The pinned source marks the internal method `@hide`;
the outer method additionally has `@SystemApi` and `@RequiresApi(37)`. Neither path
was executed or qualified for runtime linkage. Excluding the compile declaration
from every payload establishes that no fake flag implementation was supplied;
it does not establish support for those hidden paths. The candidate reuses the
published PR129 Configuration provider unchanged. Exact descriptors, bytecode
observations and the primary source hash are in the provenance receipt.

### Native results and version profiles

The stock image is
`Android/sdk_phone64_x86_64/emu64x:16/BE2A.250530.026.D1/13818094:userdebug/test-keys`.
Controlled runs 046–048 each completed successfully. The
[Android reference receipt](wifi-suggestion-android-reference-2026-11-18.json)
records all 24 complete byte lengths and SHA256 values, each process's check
count, source and payload hashes, and the original controller receipt hashes.

All 24 candidate first-write encodings match the independently compiled r4
reference exactly. The stock image's ten Suggestion wrappers are each 12 bytes
shorter; all 14 standalone Passpoint, credential, policy and update encodings
match across all three profiles. The r4 nested Configuration writer retains the
three four-byte fields `mIsAllowedToUpdateByOtherUsers`,
`persistentMacRandomizationSeed` and `mCreatorUserId`. The older stock result is
qualified separately; no field was removed to force cross-version equality.

Carrier-merged validation also has separate observed profiles. Stock requires
metered enterprise values and a valid subscription identity. The pinned-r4 and
candidate profiles preserve r4's relaxed metered constraint and its genuine
eng/userdebug enterprise exception. The common wire fixtures use metered
enterprise values valid in every profile. Stock lacks the later internal
BuildProperties helper; that absence is recorded rather than replaced. The
linked Android16-r1 source comparison supports the earlier behavior, without
claiming exact source identity for the image.

Two full Passpoint cases have a non-idempotent Java-serialized map representation
after their first semantic round trip. In every profile the difference is
confined to `Passpoint.serviceFriendlyNames`: the component remains 176 bytes,
with its first changed byte at offset 80 and identical before/after component
hashes across the three profiles. Replacing only that parsed map representation
in a fresh test copy reproduces the entire parsed Passpoint and outer Suggestion
encoding. Full semantic equality, exact reader cursor and trailing sentinel,
named dispatch, and subsequent encoding stability all pass. The independent
first-write comparison uses the original full bytes without normalization.

The initial v1 attempts are retained as incomplete evidence. Stock failed an
overly broad internal-helper presence check; both source profiles failed the
harness's unconditional byte-idempotence assumption for the serialized map.
The retained crash log identifies those Java failures; Android RuntimeInit's
uncaught-exception handler explains exit 137. No out-of-memory cause is claimed.
The v2 repair qualifies helper availability and map representation explicitly,
preserves every production source/class and adds direct stderr error reporting.
All 29 original v1 inputs and all v2 inputs remain hash-bound snapshots.

The required standalone Windows entry point loads the genuine `android_runtime`
library before graph checks or corpus calls. It is separately compiled into the
final service DEX; the executed Android v2 caller and launcher remain unchanged.

No workflow was manually started or rerun. The separate Windows result remains
pending following the user's CI cancellation.

## Boundaries

Constructing or parceling these values does not authorize a suggestion, install a
credential, authenticate a carrier, provision Passpoint, connect to an access
point or create an available Wi-Fi capability. Permission, admission and real
Windows Wi-Fi backend work remain separate. No N09/N15 hold is changed and no
issue checklist is marked complete by this increment.
