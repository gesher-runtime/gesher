# A02: SDK36 SoftAP configuration and WPS values

## Scope and prerequisites

This increment supplies the missing `SoftApConfiguration`, its `Builder`, and
`WpsInfo` value providers. It does not complete A02 or issue #56. It adds no
hotspot service, WPS transaction, radio capability, permission grant, or network
admission result.

The branch is based on the exact published PR #129 head
`d733cc0ffbff9e51892914c726c85b0da5f3c53c`, tree
`b5ddb5f5cba80e0394eb92cac9905db45dbfd4b6`. That prerequisite already contains
the reviewed configuration and ScanResult value work from PRs #127 and #112.
Their production implementations are reused without changes or duplicate copies.
Live main at the ownership review was
`473beec761bd3005a23a8f746a6ba9216343f39d`. The 67-open-PR ownership inventory,
including #140 and #141, contained no other SoftAP/WPS implementation. Shared
framework workflow and Windows track edits are additive.

The official SDK36 denominator is 40 public/protected declarations:

| Class | Declarations |
| --- | ---: |
| `SoftApConfiguration` | 23 |
| `SoftApConfiguration.Builder` | 3 |
| `WpsInfo` | 14 |

These counts include declared fields, constructors, and methods. They are not
percentages of Android, Wi-Fi, or runtime compatibility. The selected baseline
`framework.jar` and `gesher-app.jar` DEX files contain none of these three
providers. The similarly named framework compilation classes throw `Stub!` and
are not runtime providers. The archive audit is explicitly a two-archive
provider audit, not a complete boot-path census. Exact SDK/archive hashes,
declarations, class access checks, and candidate DEX checks are in
[the selected member inventory](softap-values-sdk36-members-2026-11-18.json).

## Source and implementation

Both values come from the immutable Android16-r4 Wi-Fi module revision
`e3f09203409da7a91d2c5cf9f0109da6d5243319`:

- [SoftApConfiguration.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/e3f09203409da7a91d2c5cf9f0109da6d5243319/framework/java/android/net/wifi/SoftApConfiguration.java),
  raw SHA-256 `3c5d8887e861393f3d83a3dbcd4eb9fcfba552ce49a1812ee595c1cb58990f2a`.
- [WpsInfo.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/e3f09203409da7a91d2c5cf9f0109da6d5243319/framework/java/android/net/wifi/WpsInfo.java),
  raw SHA-256 `8e0f8020e96e6a2fe5365df41c8e05d92e5b3da6f03196f04547ffccbe8dbc50`.

The original Apache-2.0 notices remain. `WpsInfo` is byte-for-byte upstream
source. `SoftApConfiguration` preserves the complete value state, constructors,
Builder validation, copy behavior, equality, hash, diagnostics, and full native
Parcel schema, including system fields. System and later flagged methods retained
with the source are not counted as additional public SDK36 coverage.

The integration changes are limited to unavailable build annotations, the real
framework jarjar imports, the existing `MacAddress.ALL_ZEROS_ADDRESS` value in
place of Wi-Fi manager's equivalent zero address, and the original private SDK
check copied from [the module Environment helper](https://android.googlesource.com/platform/packages/modules/Wifi/+/e3f09203409da7a91d2c5cf9f0109da6d5243319/framework/java/android/net/wifi/util/Environment.java).
That guard retains both released SDK36 and the `Baklava` preview codename.
Real `SdkLevel` and `CompatChanges` branches remain.

`SoftApInfo` references supply compile-time integer constants only. Bytecode
inspection confirms no executable dependency on `SoftApInfo`, `WifiManager`,
or a generated Wi-Fi flags provider. No such runtime provider or fake flag
implementation is introduced.

The source-defined rules include all valid channel ranges and zero-channel band
masks, cloned channel arrays, Builder reuse isolation, legacy SSID decoding,
passphrase diagnostic masking, lists and vendor data, and exact WPS copy rules.
For example, `new WpsInfo()` initializes `setup` to `INVALID`, while
`new WpsInfo(null)` leaves it at the Java default `PBC`. The value layer preserves
that difference and does not invent hardware validation.

The two upstream `@EnabledAfter(S)` changes require generated compatibility
metadata. `app/build.sh` now feeds the Wi-Fi source directory into the existing
`CompatChangesGen`, alongside the existing framework source input. This retains
target-SDK-dependent timeout rules and BSSID/randomization exclusion.

## Qualification

The same Java corpus compiles exclusively against official SDK36 declarations.
Reflection is limited to seeding retained system value fields and exercising
the real compatibility test overrides. It never calls hotspot/WPS services.

| Evidence | Result |
| --- | --- |
| Fresh full app-source compilation | Passed: 289 production sources plus the generated compatibility table |
| Fresh services DEX compilation | Passed; SHA-256 `1f6e6f11106c857f329cea8365ba5b891f13bd32efabfe4c6a7519c90c88d191` |
| Selected SDK class and DEX declaration audit | All 40 declarations, class ancestry/access, and 15 runtime constant checks passed |
| Host corpus | 1,125 checks passed, plus generated-table checks at target SDKs 30, 31, 32, and 36 |
| Exact baseline absence controls | Both failed at the intended missing SDK provider; compilation-stub fallback is forbidden |
| Stock Android API36 corpus | 1,252 checks passed with native Parcel |
| Independent pinned-r4 corpus | 1,257 checks passed with native Parcel |
| Candidate corpus on Android | 1,257 checks passed with native Parcel |
| Complete native wire comparison | All 12 pinned-r4/candidate byte counts and SHA-256 hashes match |
| PowerShell evidence-checker logic | 14 positive/negative checks passed under Linux PowerShell |
| Actual Windows 2022/2025 services gate | **Pending**; canceled workflows were not restarted |

The channel corpus exercises every source-defined valid channel, all 15 automatic
band masks, invalid ranges/masks/cardinalities, null input, atomic setter failure,
and copy ownership. Target-SDK tests exercise both compatibility changes through
the genuine framework `Compatibility` override mechanism and restore it after
each case. The generated-table checks additionally verify that the production
selection code disables both changes through target 31 and enables them from 32.

The local cached framework source checkout is sparse. Consequently the local
generator output validates the two selected Wi-Fi entries only, not the complete
framework compatibility table. CI retains its original full framework source
input. The required Windows corpus checks the generated table from that actual
producer output.

The host uses the existing declared native-initialization boundary and fresh
production value classes. It does not implement Parcel. The native results
were obtained by the session's root-owned Android controller, using immutable
request arrays and verified uploaded hashes; the preparation tools never run ADB.

## Version-qualified native wire evidence

The stock reference image is API36 with fingerprint
`Android/sdk_phone64_x86_64/emu64x:16/BE2A.250530.026.D1/13818094:userdebug/test-keys`.
Its Wi-Fi schema predates r4's appended `mIsBandOptimizationEnabled` field.
The [Android16-r1 writer](https://android.googlesource.com/platform/packages/modules/Wifi/+/android-16.0.0_r1/framework/java/android/net/wifi/SoftApConfiguration.java)
also lacks that final field. All eight observed stock SoftAP values are four
bytes shorter; all four WPS values match across all profiles. No field was
removed and no bytes were normalized to force agreement.

The independent r4 oracle compiles the hash-verified complete upstream
`SoftApConfiguration`, `WpsInfo`, and original `Environment` source. Only build
annotations and genuine jarjar imports change. Its compile-only `WifiManager`
field declaration is excluded from every payload; the launcher verifies the
executed field belongs to the real Android boot provider and has the correct
MAC-address type and zero value. Source-owned and boot-owned parameter types
are checked for identity before the corpus runs.

The final candidate payload SHA-256 is
`65ca7f469748051831823ccd009ae7f242b723e5a58f82b49f56574764781ea9`;
the independent r4 payload is
`915cd72fdc443943d6bc8223e8020feba45ed771f6b9e53b508574a9b01c20f5`.
All executed source hashes, payload hashes, native logs, case hashes, and profile
limits are retained in
[the Android reference receipt](softap-values-android-reference-2026-11-18.json).

The Windows track requires the actual production fixture to finish with native
Parcel, pass the generated compatibility-table boundaries, and match all twelve
complete r4 wire observations. Missing, duplicate, malformed, incorrectly
versioned, or mismatched evidence fails the gate. Wiring and local checker
validation are not a claim that Windows execution has passed.
