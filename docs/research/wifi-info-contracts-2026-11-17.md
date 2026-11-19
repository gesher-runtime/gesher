# SDK36 WifiInfo value contracts

This increment supplies the pinned Android 16 `WifiInfo` and `WifiInfo.Builder`
value model for issue #56 A02. The previous class exposed most getter signatures
but lost populated state during copying, reported no applicable redactions,
omitted the Builder and MLO/IE accessors, and serialized only an abbreviated
snapshot. The implementation now retains the upstream value, ownership,
redaction, equality and native Parcelable contracts.

The existing `WifiManager` source is unchanged. A connection-info request still
returns a disconnected default value. This work does not acquire scans, connect
a network, admit service permissions, report host radio capabilities or supply
MLO hardware. It is one value-family increment, not completion of A02.

## Exact dependency composition

The implementation follows an explicit integration of the existing ScanResult
and configuration work. It adds no second MloLink or information-element provider.

| Input | Exact remote commit | Tree |
| --- | --- | --- |
| Main | `e80cd1854163bd18e4ea5392c25f423184048804` | `bb9983408687409d12fdf2a1f8d7e47ccbd01cf0` |
| [PR #112](https://github.com/gesher-runtime/gesher/pull/112), ScanResult/MloLink | `f0d826fdb845247e295829b84bfd19a2525b8924` | `1e10c96e834c953b593e58aa78fafad8bec5021f` |
| [PR #127](https://github.com/gesher-runtime/gesher/pull/127), configuration plus reviewed CI composition | `53462c5dca0b440b03650f415c368512345b6c70` | `7c39148eb9694ae6b16a2010ec22bf55d4dfef8f` |

PR #112's actual parent is `0af3fd8333e43cdeb7a3b4bcd0403758e6d556c2`.
The GitHub PR API's current target `base.sha` is not that parent. Its exact
nine-file source delta was reconstructed on its actual parent and verified
against the complete remote tree before integration. The local reconstruction
is `5001775f11cc4d45bacf5f4b73e4ce262de89b0c`; it has the remote tree, not the
remote commit identity. PR #127's local exact-tree representation is
`72bbbcd5408c7ae14853b9fa242ab7469bf7f482`.

Their local integration is `045cb8e0db47b3de553a2d26910573f017b839a0`, tree
`a84a7bb7df4d6bac4ba9912d69dfa33ac8d3c5bb`. It preserves newer main and the
PR #112, #125 and #127 test gates. The merge had no content conflicts and no
manual conflict resolutions. An initial import attempted on newer main failed
the required full-tree comparison before any reconstruction commit; it was
discarded and repeated on the verified parent. Publication must preserve the
actual remote dependency ancestry or explicitly compose the same reviewed tree.

A fresh read-only inventory fetched all 57 open PR changed-file lists using
palmoni5. No other open PR owned `WifiInfo.java` or this corpus. The known shared
paths are the existing dependency files and workflow/Windows test tracks. The
only change to PR #112's runner adds the six reviewed configuration-family
sources now required to compile WifiInfo. Its original assertions remain intact;
the previous source list's real compilation failure is preserved in author
evidence. No other session's checkout or PR was modified.

## Selected API denominator

The official API36 `android.jar` has SHA-256
`d9eb9da824d9e247a352f570f01e1169e725b2954bca9e283a71786c59b59f9a`.
It defines the public/protected declaration denominator and compiles the public
call sites independently of the implementation.

| Class | SDK declarations | Base resolution-compatible | Candidate declarations |
| --- | ---: | ---: | ---: |
| WifiInfo | 52 | 47 | 52 |
| WifiInfo.Builder | 8 | 0 | 8 |
| **Total** | **60** | **47** | **60** |

The five missing WifiInfo methods were `getAffiliatedMloLinks`,
`getAssociatedMloLinks`, `getApMldMacAddress`, `getApMloLinkId` and
`getInformationElements`. The eight Builder declarations are its constructor,
`build`, and its six public fluent setters. Inherited Object methods already
resolved on the base, so their new value implementations are behavioral repairs,
not three extra linkage gaps. All 18 primitive/String constants match at runtime.

The [member report](wifi-info-members-2026-11-17.json) contains exact descriptors,
access flags, baseline resolutions and constants. The runner checks only fresh
production class files, including public class kind/ancestry and the public
synthetic `makeCopy(J)Landroid/net/TransportInfo;` bridge (`0x1041`). Cached SDK or
framework classes do not contribute to the candidate declaration count. These
60 declarations do not constitute a general Android compatibility percentage.

## Pinned primary implementation

The port retains the Apache 2.0 notice from
[AOSP WifiInfo.java, android-16.0.0_r4](https://android.googlesource.com/platform/packages/modules/Wifi/+/refs/tags/android-16.0.0_r4/framework/java/android/net/wifi/WifiInfo.java),
SHA-256 `9b6a82a813cb612866e2d11448016622fcd62c8a290687bcf2db5a36b3bc0da2`.
The original is 76,088 bytes. Mechanical adaptations are limited to:

- Removing unavailable build annotations and their imports: `FlaggedApi`,
  AndroidX `Keep`/`RequiresApi`, and `WifiAnnotations` type-use annotations.
  The corresponding Flags import supplies no executable behavior here.
- Importing the real bundled jarjar-relocated
  `android.internal.modules.utils.build.SdkLevel`. Executable SDK guards remain.
- Using private exact hidden redaction value aliases 0, 1, 2 and 4, consistent
  with the reviewed MloLink port, because the full hidden NetworkCapabilities
  constant namespace is not exposed by this runtime. These are value masks,
  not new permission or capability decisions.
- Removing whitespace from otherwise blank lines.

The genuine metered hint now exists, is copied and is transported by Parcel.
That permits restoring the exact hidden `WifiConfiguration.isMetered(config,
info)` helper from the pinned
[WifiConfiguration.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/refs/tags/android-16.0.0_r4/framework/java/android/net/wifi/WifiConfiguration.java),
SHA-256 `d1585fa9ab0ca3a9c4837920fd7e471df691fd9e0aae522b8c30f7fd9a75dac1`.
It combines the real hints, then applies METERED and NOT_METERED overrides in
upstream order, including null inputs. This hidden helper adds no SDK declaration
to the denominator. The earlier configuration report describes its deliberate
omission at that increment; this follow-up removes that prerequisite.

## Measured behavior and upstream quirks

The same SDK-compiled corpus passed against both actual Android API36 framework
providers and isolated candidate providers. It preserves details that cannot be
inferred safely from getter signatures alone:

| Contract | Observed behavior retained |
| --- | --- |
| Defaults and reset | Disconnected identity and empty MLO/vendor lists; null IE list. Directional/max link rates start at zero but reset to -1. Reset retains supplicant state and Wi-Fi standard. RSSI updates use actual elapsed time. |
| Builder and copy | Fluent setters, clamped RSSI, independent repeated snapshots, arbitrary network/subscription IDs, null/empty/UTF-8 and binary SSIDs. Copy omits the hidden-SSID flag and producer RSSI timestamp, matching upstream. |
| SSID ownership | WifiSsid shares the supplied input byte array; its returned bytes are defensive. WifiInfo copying retains the upstream SSID reference behavior. |
| Security conversion | Recognized configuration security types map exactly. The pinned converter omits OSEN, so input 10 maps to UNKNOWN (-1), as do unrecognized inputs. |
| Redaction | Exact location/local-MAC/network-settings masks 1/2/4, all combinations, and unknown bits. Location removes identities and IE data; local MAC is masked under either relevant mask. Settings redaction removes requesting package and makes primary-state access throw SecurityException. |
| IE ownership | Lists are copied while their IE elements remain aliased. No invented universal deep-copy guarantee. |
| MLO ownership | Copies deep-copy link values when AP MLD identity exists. Associated links include IDLE and ACTIVE states. Copying an MLO list without AP MLD identity drops the list, as upstream does. |
| Equality and hashing | API36 content equality, including equal and changed state. MLO equality ignores list order, while its hash remains sensitive to stored list order. The observed upstream inconsistency is explicit; the port does not silently repair it. |
| Address and metering | IPv4 integer byte order, IPv6/no-address behavior, metered hints and explicit override precedence. No host address is injected into a connection snapshot. |
| Parcel | Defaults, populated values, redaction variants, IPv6, empty lists and vendor-bearing state use the full native wire layout and creator. |

The source's pre-S equality fallback and other executable SDK checks remain;
the actual runtime observations here are API36. No older-device execution claim
is made.

## Genuine runtime providers

Both host and candidate Android loaders assert ownership of all 16 production
value-provider families. The candidate Android loader also owns the actual
framework IPv4 helper and never fills a missing owned class from Android's boot
provider. The reference loader asserts nine public framework provider identities
and runs the identical SDK fixture. Neither supplies a Parcel substitute.

The IPv4 helper is not merely an entry in a compile classpath. An authenticated
completed framework artifact from [run 44190791143](https://github.com/gesher-runtime/gesher/actions/runs/44190791143),
source `2242e7383c7b8937738273fd643010c1023db840`, producer job
`134424356988` (`framework / framework`, success), artifact `14376399307`,
was downloaded and inspected. Its 61,071,634-byte archive has SHA-256
`93d733abaae3f44279fea154096a2b10770ef7e672d3368cd164a2f403338f57`.
The actual bundled `framework.jar` has SHA-256
`dbd0b1c8db31ed73c18a45b533e3897587e8a9b5a1f69c066a4854db5bbbcefa`.
Parsing all five DEX files verifies genuine definitions of
`com.android.net.module.util.Inet4AddressUtils` and
`android.internal.modules.utils.build.SdkLevel`. WifiInfo and WifiConfiguration
are absent from that framework bundle and are supplied by the app output.

The actual helper class used by the candidate has SHA-256
`a993d33346712ce7f60648d636d7a6e8e2f38204674ea223ca5e9dde37beca4e`;
its HTL conversion calls the genuine HTH conversion and reverses the integer
bytes. Independent numeric IPv4 assertions exercise this code. This artifact
inspection establishes the existing runtime provider; it is not execution of
the new candidate on Windows.

## Validation boundaries and results

| Boundary | Result |
| --- | --- |
| Fresh production Java build | 287 source files; exit 0. |
| SDK-bound fresh-provider host corpus | 334 checks; exit 0; all 60 selected declarations and 18 constants checked. Native initialization/logging and real elapsed clock come from the existing declared host adapter. No host Parcel claim. |
| Old-value negative control | Replacing only WifiInfo from main fails the exact applicable-redaction-mask assertion. |
| Old-linkage negative control | The same baseline fails to load WifiInfo.Builder. A strict loader prevents cached framework/app Builder fallback. |
| Existing ScanResult and Wi-Fi data regression | 1,025 ScanResult checks and 133 Wi-Fi data checks pass; selected declaration inventory remains 86. |
| Existing configuration regression | 396 host checks pass. |
| Service fixture Java/D8 build | 150 Java sources; real build script exits 0. An initial missing-javac PATH failure is preserved separately from the corrected process environment. |
| Actual Android reference | 631 checks; exit 0; native Parcel required. |
| Actual Android candidate | The same 631 checks; exit 0; native Parcel required and candidate ownership asserted. |
| Windows evidence parser | Both PowerShell files parse; authentic Android output passes and seven missing/duplicate/unknown/altered/extra/empty-output controls fail. This is Linux PowerShell parser validation, not Windows execution. |
| Windows ART runtime | A real mandatory contract invocation is added to the existing compatibility track. Execution remains pending in the candidate CI environment. |

The Android fingerprint is
`Android/sdk_phone64_x86_64/emu64x:16/BE2A.250530.026.D1/13818094:userdebug/test-keys`,
SDK 36, Android 16, security patch 2025-07-05, x86_64. Reference and candidate
executions are separate immutable requests and receipts. The 10.520-second and
13.118-second observations are execution records, not performance benchmarks.

Nine deterministic cases independently hash the complete native `marshall()`
output before appending a sentinel. Reference and candidate agree on every case
identifier, byte count and complete SHA-256 digest:

| Case | Bytes |
| --- | ---: |
| Vendor-bearing_Parcel | 1,396 |
| Default_Parcel | 280 |
| Populated_Parcel | 1,260 |
| Redacted_Parcel_1 | 832 |
| Redacted_Parcel_2 | 1,092 |
| Redacted_Parcel_4 | 1,224 |
| Redacted_Parcel_7 | 628 |
| IPv6_Parcel | 1,272 |
| Empty-list_Parcel | 584 |

The [Android reference record](wifi-info-android-reference-2026-11-17.json)
contains all nine full digests, source hashes, device profile, payload hashes
and raw-receipt hashes. The fixture also independently reads the first six
scalars, round-trips through the real CREATOR, compares values/equality/hash,
and requires exact end position, sentinel and no remaining data. Whole-wire
comparison supplements these assertions rather than replacing them.

`tools/prepare_wifi_info_oracle.py` prepares immutable deterministic reference,
candidate and launcher payloads with every class hash and explicit separate ADB
setup/reference/candidate requests. It never starts an emulator or executes ADB.
The fixture and production sources were unchanged between the final recorded
execution and the clean-commit reproducibility build; those payload and class
hashes bind the execution to the reviewed source.

## CI and remaining integration requirement

`framework.yml` adds the actual fresh-provider host runner after the existing
configuration test. `96-deep-compatibility.ps1` requires successful native ART
execution, the corpus completion marker, `parcel=true`, and all nine complete
Parcel observations matching the committed Android record. Missing or altered
evidence fails the Check. Existing tests and their exit codes are preserved.

[PR #126](https://github.com/gesher-runtime/gesher/pull/126) separately changes the
immutable framework policy. Before claiming combined CI coverage, its policy
composition must include this added host invocation and retained dependency
gates. This increment does not change that PR, a runner allowlist, an approval
rule or the immutable policy. Independent QA must review the exact integration
base and implementation tree; a local host or Android pass does not replace the
candidate's actual Windows ART result.

## Standalone services harness repair (2026-11-18)

The original-head Windows services run reached `WifiInfoContractTest` through
standalone `dalvikvm` and failed at the unregistered native
`SystemClock.elapsedRealtime`. The direct `main` entry now loads the production
`android_runtime` library before invoking the corpus. Its `JNI_OnLoad` registers
SystemClock and Parcel. Android's oracle entry remains `run(true)`, using the
normal Android runtime bootstrap. No production implementation changes.

The services job's sparse checkout also omitted the committed Android wire
reference. The checkout now includes `docs/research`. A fresh sparse checkout
using the old paths reproduces the absent required JSON; the updated paths
materialize it. The strict nine-case evidence validator and its required check
remain unchanged. Authentic Android output passes; six controls (missing case,
duplicate case, wrong byte count, wrong digest, unknown case, and missing
reference file) all fail.

Fresh reference and isolated-candidate Android executions each pass **631
checks**. All nine complete native Parcel case names, byte counts and SHA-256
digests match each other and the committed reference. The
[repair execution record](wifi-ci-repair-android-reference-2026-11-18.json)
binds these observations to the fixture and production source hashes. The
complete service fixture also compiles and D8 succeeds. These Android and build
results do not execute the standalone Windows entry; both final-source Windows
services jobs remain required before claiming the repaired harness passed.
