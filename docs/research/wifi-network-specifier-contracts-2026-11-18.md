# SDK36 Wi-Fi network specifier contracts

This A02 increment supplies `WifiNetworkSpecifier`, its public `Builder`, and
the hidden `WifiNetworkAgentSpecifier` used by public matching dispatch.
Applications can construct, compare, redact, match and parcel these request
values. Validation and ownership follow pinned Android 16, including changes
to builder state caused by a failed `build()`.

Association, local-only admission, consent, scan acquisition and radio
capabilities remain separate service/backend work. The existing `WifiManager`
and connectivity services are unchanged. Constructing a request does not
establish that a corresponding connection can be fulfilled. A02 and #56 remain
open.

## Source and dependencies

Both production classes come from Wi-Fi module `android-16.0.0_r4`, immutable
commit `e3f09203409da7a91d2c5cf9f0109da6d5243319`, tree
`3bb82c9ea5e5e75dea583b966cbc4f9ce03aa15c`:

| Source | Upstream SHA-256 |
| --- | --- |
| [WifiNetworkSpecifier.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/e3f09203409da7a91d2c5cf9f0109da6d5243319/framework/java/android/net/wifi/WifiNetworkSpecifier.java) | `c764ebdbdb13da5f9941fcb7aee23ea3aed6007d8d2f1c32597ebd3c4984395e` |
| [WifiNetworkAgentSpecifier.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/e3f09203409da7a91d2c5cf9f0109da6d5243319/framework/java/android/net/wifi/WifiNetworkAgentSpecifier.java) | `afbe67e09af2e9a81ed3cee2ced239e2584dfc941d080a3ae9baa00ad69ec852` |

Their Apache 2.0 headers remain intact. The production adaptations are:

- Import the genuine framework jarjar `SdkLevel` at
  `android.internal.modules.utils.build.SdkLevel`, preserving the executable
  API-S guard in `redact()`.
- Use existing `MacAddress.ALL_ZEROS_ADDRESS` for the identical zero-MAC value.
  The shared broadcast-mask identity behavior remains intact.
- Read nested Parcelables using their defining class loaders. The native wire
  stays unchanged; actual value providers retain ownership in the isolated
  candidate loader. Windows separately tests the real boot-loader path.

The branch includes the independently reviewed #129 integration and harness
repair published at `d733cc0ffbff9e51892914c726c85b0da5f3c53c`, tree
`b5ddb5f5cba80e0394eb92cac9905db45dbfd4b6`. Its parents are original #129
`63d8db3e8e4a7c992d8daa0c027860bb759dcdaa` and main
`473beec761bd3005a23a8f746a6ba9216343f39d`. #129 already includes #112's
ScanResult and #127's configuration/enterprise providers. All seven repair
paths are included unchanged, including the real test-shards sparse checkout
of `docs/research`, standalone native initialization, and Android-defined
rejection of null configuration containers. No inherited production provider
changes are introduced by this specifier increment.

The complete changed-file inventory of 57 open PRs at 2026-11-18T01:40:05Z
found no competing specifier provider or fixture. Workflow and Track96 edits
are shared integration points, with inherited Wi-Fi gates preserved.

## Runtime provider and SDK denominator

The selected distributable framework/app JARs from #129 run `44208723771`,
producer `134484587998`, artifact `14386510665` contain none of the two public
specifier classes or hidden agent. Their SHA-256 values are:

- framework.jar: `fed525b7fd92cfe2b82ac547172d545dd2f4073a6bb67740906b15ec7c8c1bb7`
- gesher-app.jar: `6ea29dee02b9ac9239a043bdf169aff73ead17dd10f5fd060792acc4a691441a`

The required NetworkSpecifier, MatchAllNetworkSpecifier, PatternMatcher and
Wi-Fi dependencies have genuine providers. This is a bounded census of those
selected JARs. Framework compile-only Wi-Fi stubs are not runtime providers.

The SDK36 JAR SHA-256 is
`d9eb9da824d9e247a352f570f01e1169e725b2954bca9e283a71786c59b59f9a`.

| Public class | SDK declarations | Fresh production class | Candidate DEX |
| --- | ---: | ---: | ---: |
| WifiNetworkSpecifier | 10 | 10 | 10 |
| WifiNetworkSpecifier.Builder | 16 | 16 | 16 |
| Total | 26 | 26 | 26 |

The [member record](wifi-network-specifier-members-2026-11-18.json) preserves
exact descriptors, flags and Java-17 production class hashes. The required
hidden matcher is outside this SDK denominator. Declaration coverage remains
separate from behavior and runtime coverage.

## Behavioral corpus

Public call sites compile against the official SDK only. Reflection inspects
hidden state and invokes the real hidden matcher. The same corpus checks:

- Literal, prefix, glob, advanced-glob and suffix SSIDs; malformed UTF-16;
  absent/match-none/unrestricted selectors; exact/masked MACs; identical versus
  equal all-ones masks.
- Band/frequency validation, hidden low/high 5GHz bands, channel ownership,
  duplicates/order, band conflicts and reuse after failed construction.
- Open, enhanced-open, WPA2/WPA3 and enterprise modes, copied setter input and
  shared build snapshots. Genuine isolated DER/PKCS8 fixtures cover EC384,
  RSA3072, insufficient RSA2048 and SHA256 signatures. They are not installed
  or used for authentication.
- Credential-insensitive equality, channel order and secondary-STA hints;
  band-only redaction and the pinned unspecified-band error.
- Directional request/agent matching, null/MatchAll, local-only versus system
  band matching, malformed internal SSIDs and key-management mismatch.
- Full native Parcel consumption with sentinels, nested configuration and
  certificate ownership, channel arrays and the secondary-STA version tail.
  The real boot-loader NetworkRequest path is checked separately.

Twelve full Parcel observations cover ordinary, Unicode/credential,
exact-BSSID, pattern/OWE, hidden-SSID, four band-only, secondary-STA,
enterprise-192 and agent values. Windows compares length and SHA-256 against
the pinned writer observations below and rejects missing, duplicate or
mismatched records and a different wire profile.

## Native reference profiles

The [versioned reference record](wifi-network-specifier-android-reference-2026-11-18.json)
keeps three measurements distinct:

| Profile | Executed providers and purpose |
| --- | --- |
| Stock Android API36 | Genuine emulator boot providers, fingerprint `Android/sdk_phone64_x86_64/emu64x:16/BE2A.250530.026.D1/13818094:userdebug/test-keys`, security patch `2025-07-05`. The shared corpus passes 533 checks plus four boot `NetworkRequest` Parcel checks. |
| Gesher candidate on Android | Eighteen freshly compiled production families are owned by a strict loader, while Android supplies native Parcel and framework helpers. The same 533 checks pass. |
| Pinned r4 writers on API36 | Independently fetched, hash-verified complete sources for the specifier, agent, configuration and security parameters execute their constructors and unmodified writers on actual Android native Parcel. This qualifies the listed writer corpus, not a complete r4 system image. |

### The measured twelve-byte schema difference

The stock image and candidate initially differ in every one of the twelve
wire cases; that failed comparison remains part of the record. The image
predates the pinned Wi-Fi module commit dated 2025-10-30. The r4
`WifiConfiguration` writer appends three fields absent from the stock image:

| Field | Observed default | Native bytes |
| --- | --- | --- |
| `mIsAllowedToUpdateByOtherUsers` | `true` | `01 00 00 00` |
| `persistentMacRandomizationSeed` | `0` | `00 00 00 00` |
| `mCreatorUserId` | `-2` | `fe ff ff ff` |

The plain configuration is 944 bytes on the stock image and 956 bytes in the
candidate. Its first 944 bytes are identical. The enclosing specifier is
1,232 versus 1,244 bytes because its own channel-array and secondary-STA tail
follows the configuration. Raw native bytes and reflected field sets identify
the same three additions. Separate OWE and enterprise-192 diagnostic values
also have byte-identical configuration prefixes and precisely this tail.
Independent r1/r4 source comparison shows those three additional writer
calls. Production writer bodies match r4 exactly; no bytes are stripped,
rewritten or normalized for comparison.

### Independent source writer and its provider boundary

In addition to the two source hashes above, the pinned reference uses:

| Complete upstream source | Raw SHA-256 |
| --- | --- |
| [WifiConfiguration.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/e3f09203409da7a91d2c5cf9f0109da6d5243319/framework/java/android/net/wifi/WifiConfiguration.java) | `d1585fa9ab0ca3a9c4837920fd7e471df691fd9e0aae522b8c30f7fd9a75dac1` |
| [SecurityParams.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/e3f09203409da7a91d2c5cf9f0109da6d5243319/framework/java/android/net/wifi/SecurityParams.java) | `9aaccf30c9a81f6365248f97d63bf633eac06d0dfdb59e260c8c384dc7ebb060` |

The reference preparer compiles 36 fresh upstream classes from those four
files, retains all five writer bodies unchanged, and binds raw, adapted,
class and DEX hashes. Compilation removes unavailable build annotations and
uses the genuine framework jarjar `SdkLevel` import. Three external
compile-only declarations for `Flags`, `Environment` and the `WifiManager`
constant are explicitly excluded from the runtime payload. The unexecuted
API37 identity methods are outside this writer corpus; the executed manager
constant resolves to the asserted boot provider.

`SecurityParams` must share the configuration's defining loader: its
`updateLegacyWifiConfiguration(WifiConfiguration)` writes directly into that
object. An initial mixed-loader reference retained the boot security helper
and matched only ten of twelve cases. Exact diagnostics found
`requirePmf=false` and `mIsRepeaterEnabled=true` in the two affected source
configurations, where both stock and candidate had `true` and `false`.
Each full Parcel differed at exactly those two boolean bytes. All security
parameter bytes, enterprise input certificate bytes and nested enterprise
Parcel bytes were identical across all three profiles. The r1 and r4
`SecurityParams` sources are themselves byte-identical. This was an invalid
reference provider boundary, not evidence of an upstream security behavior
change.

The corrected reference owns both classes, asserts the method parameter's
class identity, and requires PMF in both affected cases before writing.
The discarded mixed reference and its exact differences remain recorded.
Other dependencies, including `WifiEnterpriseConfig`, `MacAddress`,
`WifiManager`, `SdkLevel` and native `Parcel`, remain genuine Android boot
providers with asserted identities. The final profile is named
`aosp-wifi-android-16.0.0_r4-pinned-writers-on-api36` so the Windows gate cannot
accidentally consume the older stock-image schema.

## Verification

| Check | Observed result |
| --- | --- |
| Fresh production Java-17 compilation | 289 sources, 789 classes, exit 0; 4.342 seconds. Genuine framework/core/generated compile inputs reused. |
| SDK-only callers and production host corpus | 395 checks, exit 0; final integrated run 3.224 seconds including compilation. Eighteen fresh value families and genuine framework dependencies have checked origins. |
| Missing-provider baseline | Expected Builder NoClassDefFoundError; strict loader prevents cached API-stub fallback. |
| Existing Wi-Fi regressions | 1,025 ScanResult + 133 data + 396 configuration + 334 WifiInfo checks pass. |
| Service fixture build | Final integrated 154 Java inputs and genuine D8 output, exit 0; 8.007 seconds. DEX SHA-256 `df50f64895a9767151ecfb56afb14b937b14f5db217eb49aa7a4d312891c28bf`. |
| Android stock reference and candidate | 533 checks each, native Parcel enabled; stock boot NetworkRequest adds four checks. Their distinct versioned wire schemas are preserved above. |
| Independent pinned source writer | Exit 0, 4.604 seconds; all twelve complete lengths and SHA-256 values match the candidate exactly. Six source identities (four provider families and two fixture classes) and five genuine platform identities are asserted. |
| Native-wire evidence predicate | Ten PowerShell controls pass: authentic Android output is accepted; missing, duplicate, extra, truncated, unknown-label, wrong-length, wrong-hash, wrong-profile and wrong-source-commit observations are rejected. Both changed PowerShell files parse. Executed on Linux, separate from Windows runtime validation. |
| Windows ART/native Parcel | Requires candidate Windows CI. Host/Android execution is not a Windows pass. |

The host JNI boundary supplies initialization, logging and the real Linux
elapsed clock; it implements neither Wi-Fi values/matching nor Parcel.
`prepare_wifi_network_specifier_oracle.py` uses one SDK corpus for both DEX
payloads and records source/class/artifact hashes. The candidate must own all
18 value families; the real Android framework supplies PatternMatcher,
NetworkRequest and native Parcel. Boot NetworkRequest cannot look up isolated
candidate classes, so candidate nested dispatch is explicitly qualified by
Windows's real boot class path; Android reference mode runs the nested check.

The framework workflow runs the host corpus. Windows Track96 runs native
Parcel, the twelve-case wire comparison and nested NetworkRequest. Its direct
fixture entry loads `android_runtime`, following existing native service
fixtures. Existing ScanResult, configuration and WifiInfo gates stay active.

### Reproduction

After a genuine framework/app build:

```sh
python3 tools/test_wifi_network_specifier_contracts.py \
  --java-home "$JAVA_HOME" --sdk-jar work/sdk-36/android.jar \
  --framework-classes out/framework/classes --core-classes out/coremod \
  --app-classes out/app/gesher-app-classes.jar
```

Add `--baseline-control` for the expected missing-provider failure. The oracle
preparer takes the same inputs plus `--d8-jar` and a new `--output-dir`.
Its manifest and explicit setup/reference/candidate ADB requests bind every
payload; the preparer never starts or controls a device.

For the pinned writer reference, fetch the four complete raw sources from the
immutable URLs in the reference record into `r4-<ClassName>.java` files. Run
`tools/prepare_wifi_specifier_pinned_reference.py` with the same build inputs,
`--d8-jar`, `--source-dir` and a new `--output-dir`. The preparer rejects any
source hash mismatch and produces separate explicit setup and reference
requests. Its SDK-only corpus uses the same twelve case inputs and test-only
certificate fixture as the production corpus.
