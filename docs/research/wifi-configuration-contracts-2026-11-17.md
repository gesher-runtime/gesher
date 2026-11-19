# SDK36 Wi-Fi configuration value contracts

This increment supplies the public SDK36 `WifiConfiguration` and
`WifiEnterpriseConfig` value families for issue #56 A02. It replaces the former
abbreviated configuration implementation with the pinned Android value model:
security suites, enterprise identities and certificate material, copying,
proxy/IP state, network keys, randomization metadata, and Parcelable transport.
It does not provide a Wi-Fi radio or change the existing manager's unsupported
controller operations.

The base is `e80cd1854163bd18e4ea5392c25f423184048804`. A complete inventory of
54 open PRs and their changed files found no author for this family. PR #112 owns
the adjacent ScanResult/ANQP/MLO family; none of its production files, test
fixtures, or runner are changed here. The shared framework workflow and Windows
compatibility track receive a distinct configuration test invocation.

## Selected API denominator

The denominator is the public/protected declarations in the official API36
`android.jar`, SHA-256
`d9eb9da824d9e247a352f570f01e1169e725b2954bca9e283a71786c59b59f9a`.
SDK stubs are used to bind callers and define expected descriptors, never as
runtime providers.

| Class | SDK declarations | Base resolution-compatible | Candidate compatible |
| --- | ---: | ---: | ---: |
| WifiConfiguration | 52 | 20 | 52 |
| WifiConfiguration.KeyMgmt | 20 | 0 | 20 |
| WifiConfiguration.GroupMgmtCipher | 3 | 0 | 3 |
| WifiConfiguration.Status | 4 | 0 | 4 |
| WifiConfiguration.Protocol | 5 | 0 | 5 |
| WifiConfiguration.GroupCipher | 9 | 0 | 9 |
| WifiConfiguration.PairwiseCipher | 8 | 0 | 8 |
| WifiConfiguration.AuthAlgorithm | 6 | 0 | 6 |
| WifiEnterpriseConfig | 57 | 0 | 57 |
| WifiEnterpriseConfig.Eap | 10 | 0 | 10 |
| WifiEnterpriseConfig.Phase2 | 8 | 0 | 8 |
| **Total** | **182** | **20** | **182** |

The 162 resolved gaps comprise 32 on the original configuration class and 130
on the previously absent nested/enterprise classes. The base inherited
`Object.toString()`, so absence of a declared override is not counted as another
linkage gap. All 93 primitive/String constant values also match the SDK. The
[member report](wifi-configuration-members-2026-11-17.json) records every
descriptor, relevant access flag, base resolution, constant and source hash.
Fresh production DEX definitions retain all 182 selected members.

## Primary source and adaptations

All imported sources retain their Apache 2.0 copyright and license notices.
The Wi-Fi sources are pinned to `platform/packages/modules/Wifi` tag
`android-16.0.0_r4`, under `framework/java/android/net/wifi/`.

| Source | Upstream SHA-256 | Use |
| --- | --- | --- |
| [WifiConfiguration.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/refs/tags/android-16.0.0_r4/framework/java/android/net/wifi/WifiConfiguration.java) | `d1585fa9ab0ca3a9c4837920fd7e471df691fd9e0aae522b8c30f7fd9a75dac1` | Configuration, nested constants/status, copy and Parcel implementation, with the focused adaptations below. |
| [WifiEnterpriseConfig.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/refs/tags/android-16.0.0_r4/framework/java/android/net/wifi/WifiEnterpriseConfig.java) | `45bf5d988b89287e455342a30cb93b0778419d5b0bfdd1e8edc728f136ef03b3` | EAP, phase 2, TLS policy, field validation and certificate/key value semantics. |
| [SecurityParams.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/refs/tags/android-16.0.0_r4/framework/java/android/net/wifi/SecurityParams.java) | `9aaccf30c9a81f6365248f97d63bf633eac06d0dfdb59e260c8c384dc7ebb060` | Unchanged suite factory and security parameter values. |
| [ParcelUtil.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/refs/tags/android-16.0.0_r4/framework/java/android/net/wifi/ParcelUtil.java) | `339abf2b7118066a53c8517f84bbf83b0c8097c97ffe271e8cd3e355a31e520a` | Real CertificateFactory/KeyFactory serialization and typed vendor data. |
| [OuiKeyedData.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/refs/tags/android-16.0.0_r4/framework/java/android/net/wifi/OuiKeyedData.java) | `17c8a66b116333ce2cb454909676e6296e26496d142932fb92524a61919b6089` | Vendor metadata values required by the configuration's Parcel model. |
| [util/PersistableBundleUtils.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/refs/tags/android-16.0.0_r4/framework/java/android/net/wifi/util/PersistableBundleUtils.java) | `82a38ffde5621d1a0c7c6b8c71ce535502f8c71a68397eb8586742f171094730` | Unchanged bundle content equality/hash helpers. |

Unavailable build annotations (`FlaggedApi`, AndroidX `Keep`/`RequiresApi`) and
their imports are removed. D8 does not shrink these classes. Runtime SDK guards
remain operative: imports use the framework's actual jarjar-relocated
`android.internal.modules.utils.build.SdkLevel`, whose implementation was
verified in the framework compile artifact. Its primary source is
[modules-utils SdkLevel](https://android.googlesource.com/platform/frameworks/libs/modules-utils/+/refs/tags/android-16.0.0_r4/java/com/android/modules/utils/build/SdkLevel.java),
SHA-256 `74580e2fcc7cac5b405ae727e90a1ce1530e02d944cee375513ce29ad785b929`.
The existing real `MacAddressUtils` implementation is reused; no feature-flag or
capability shim is added.

`getPrintableSsid()` embeds the exact double-quote removal from the pinned
[WifiInfo source](https://android.googlesource.com/platform/packages/modules/Wifi/+/refs/tags/android-16.0.0_r4/framework/java/android/net/wifi/WifiInfo.java)
(`9b6a82a813cb612866e2d11448016622fcd62c8a290687bcf2db5a36b3bc0da2`).
It avoids changing the separate disconnected WifiInfo implementation.

The hidden `isMetered(config, info)` helper is omitted because its connection
hint requires WifiInfo's missing mutable snapshot model. It is absent from
the public SDK36 denominator. API37 multi-user Wi-Fi behavior is also outside
this runtime profile. The three version guards inline the exact
`SDK_INT > VERSION_CODES.BAKLAVA` predicate from
[Environment.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/refs/tags/android-16.0.0_r4/framework/java/android/net/wifi/util/Environment.java)
(`883021cad62185fd0ff650ef7b309888f4661268179b0f48b61caac3dd6a106d`);
they throw on API36. The hidden creator-user helper uses the UID-derived API36
identity rather than enabling the future multi-user flag. The r4 stored fields
and Parcelable tail are retained.

## Behavior and ownership

The port retains Android's distinctions between defaults, absent state and
errors. A newly constructed configuration owns empty security masks and an
unassigned randomized address; selecting a security suite fills its actual
protocol, key-management and cipher masks. Unknown suite values throw. A DPP
security type alone does not mark a value as a DPP configurator. Passpoint
classification requires the provider's unique identity as well as complete
enterprise metadata.

Copy constructors retain Android's exact ownership and null behavior. Mutable
configuration arrays, bitsets, enterprise state and IP configuration are copied.
Enterprise CA/client arrays are copied by setters and copy constructors, while
the public getters expose their stored arrays, as upstream does. Private-key
objects retain their original identity on an in-process copy. Parcel transport
reconstructs real certificate/key encodings. Invalid CA material can clear
stored CAs; invalid client chains fail before replacing existing key/chain
state. Oversized string fields are ignored where upstream ignores them; EAP,
phase-2, TLS, alias and certificate structural errors retain the upstream
exception behavior.

These are configuration values. The fixture's public test-only certificates
are never installed as trust anchors or granted access to a network/key store.
The existing service still decides whether an operation is supported and which
permissions apply. This increment makes no claim of EAP authentication, DPP
provisioning, radio availability or a successful connection.

The new Parcel layout follows the pinned r4 writer/reader, replacing the old
abbreviated Gesher format. Old serialized configuration Parcels are not a
versioned storage format and are not promised to remain readable across this
change. The public `getKey()` result is likewise not a stable persistence key.

## Validation and provider boundaries

`tools/test_wifi_configuration_contracts.py` compiles every public test call
against the official SDK alone, then runs with freshly compiled production
providers first on the class path. A host bootstrap asserts the source location
of all six imported value/helper families. Its native adapter provides existing
framework initialization/logging and Linux's real `CLOCK_BOOTTIME` reading;
it supplies neither Parcel transport nor Wi-Fi/certificate behavior.

The host corpus passes **396 checks**. It covers runtime constant values, all
14 accepted security-suite masks, defaults, identity and copy ownership,
invalid inputs/state transitions, IP/proxy metadata, EAP/phase-2/TLS behavior,
field limits, password masking, real DER/PKCS8 material and vendor bundle/list
semantics. Holding the new dependencies and callers fixed while replacing
WifiConfiguration with the base source fails with the expected
`NoSuchMethodError` at its public copy constructor.

The same SDK fixture's native mode adds certificate/key, complete configuration
and nonempty vendor-data Parcel round trips, sentinel-based exact stream
consumption and an independent check of the writer's networkId/status prefix.
It also preserves positive Passpoint and DPP producer state. The Windows ART
gate runs this native mode through the existing `FixesRun` boot configuration.

For an Android reference run, `tests/wifi-configuration/WifiConfigurationOracle.java`
loads either the reference classes or the candidate family. Candidate mode uses
child-first loading for only these families and the test corpus, verifies each
provider identity, and prohibits falling back to a boot Wi-Fi implementation.
Android supplies genuine native Parcel and the surrounding framework types.
This isolates value behavior; it is not a test of Gesher's Windows native layer.
Oracle-02 passed **510 checks with native Parcel for each provider**, with
exit 0 for both the original Android reference and the isolated candidate. All
six candidate families were verified as coming from the child-first loader,
and all three on-device JAR SHA256 values matched the build artifacts. The
reference image is API36 / Android16, fingerprint
`Android/sdk_phone64_x86_64/emu64x:16/BE2A.250530.026.D1/13818094:userdebug/test-keys`,
with the July2025 security patch. Exact source, artifact, device and execution
evidence is in [the Android reference receipt](wifi-configuration-android-reference-2026-11-17.json).
This does not establish a Windows runtime pass.

The existing Wi-Fi data corpus passes **133** host checks and the existing
network snapshot/proxy corpus passes **175**. All **284** production Java sources
compile against the available Android framework/core inputs; their D8 output
retains all selected descriptors. All **150** service-fixture Java sources,
including the new corpus, compile and D8 successfully. Local Windows runtime
execution is unavailable; the added current-head Windows gate is required
before claiming that platform passed.

The build distinction matters: framework javac output contains compile-only
updatable-module stubs. `tools/framework/plan.py` marks SDK-library stubs as
compile-only, and `tools/framework/build.py`/`tools/gen/split_classes.py` package
only bundled source classes into the framework DEX. The app build packages the
actual Java sources into gesher-app.jar after framework on the boot class path.
The local class/DEX checks establish this candidate's definitions; they do not
substitute for checking a complete ordered Windows boot artifact. The native
fixture must execute successfully through the distribution's actual boot path.

## Integration fixture repair (2026-11-18)

The Windows services run on PR #129's original head failed in the older
`WifiUnsupportedTest` fixture after that fixture set both `wepKeys` and
`allowedProtocols` to null. The pinned
[AOSP Android 16 writer](https://android.googlesource.com/platform/packages/modules/Wifi/+/refs/tags/android-16.0.0_r4/framework/java/android/net/wifi/WifiConfiguration.java)
iterates the WEP array and calls `cardinality()` on security bitsets directly.
The containers are required; nullable WEP elements remain valid. The fixture
now independently expects `NullPointerException` for each invalid container,
restoring a valid WEP array before testing the bitset. Production serialization
is unchanged. Its existing completion count remains 42.

The SDK-compiled configuration corpus now independently exercises both invalid
containers against each provider with real native Parcel. Actual Android's
reference and both isolated PR #127 and PR #129 candidate families each pass
**512 checks**, including the two new rejection cases. The updated
[execution record](wifi-ci-repair-android-reference-2026-11-18.json) includes the
device profile, complete outputs, source and payload hashes. Each PR records
its own production candidate because PR #129 includes the metered helper.
These runs establish the fixture's Android behavior; they do not establish a
Windows runtime pass or complete configuration wire-byte equivalence. Both
Windows services jobs remain required on the final published source.

## Remaining A02 scope

This closes the selected 162 value-family descriptor gaps, not issue #56 A02 as
a whole. ScanResult/ANQP/MLO remains PR #112's separate family. WifiInfo's full
mutable/builder/redaction model, the omitted metered helper, other absent module
APIs, API37 multi-user behavior, and real Wi-Fi controller workflows remain
outside this increment. Descriptor coverage and focused value behavior do not
establish arbitrary-APK or full Android framework compatibility.
