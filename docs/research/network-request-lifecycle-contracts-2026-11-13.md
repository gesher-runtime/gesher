# Network request values and callback lifecycle — 2026-11-13

This work starts from main `6bfc980c335a8b8ae1a9271dc5da43078561b516` and
does not repeat open PR #13's Wi-Fi and LinkProperties/RouteInfo repairs.
All behavioral comparisons use pinned Android `android-16.0.0_r4` sources.

## Request and value contracts

NetworkCapabilities now constrains capability IDs to 0–37 and transport IDs to
0–10. Android 16 logs and ignores invalid capability mutations; invalid transport
mutations throw before changing state. Queries outside these ranges return false.
This prevents Java shift-distance wrapping from altering unrelated valid bits.

A non-null network specifier requires one non-test transport, TEST alone, TEST
plus one other transport, or the CELLULAR/SATELLITE pair, optionally with TEST.
Transport additions/removals revalidate that invariant. Android mutates the
transport before this particular invariant check, so a thrown check leaves the
new transport bits and the previous specifier; the fixture preserves that order.
NetworkRequest rejects MatchAllNetworkSpecifier, while network offers retain
their existing ability to use it.

Legacy string conversion clears null/empty strings, constructs telephony values
for parseable integer subscription IDs, uses TestNetworkSpecifier with TEST,
and otherwise uses EthernetNetworkSpecifier. Integer.MIN_VALUE remains the
Telephony builder's rejected unset sentinel; numeric overflow becomes an
interface name, as in the pinned conversion. Both complete missing value types
are imported unchanged under their Apache-2.0 headers. Ethernet interface/MAC
matching remains asymmetric and is tested independently from equality.

An enterprise capability without an explicit enterprise-ID set implies ID 1.
This change implements that existing supported state; it does not add an
enterprise network backend or hidden explicit-ID setters.

## Callback ownership

The callback registry now uses object identity, independent of application-defined
equals/hashCode. Previously successful callbacks record that fact on the callback
itself, avoiding a global history collection. Repeated unregister, including
cleanup after onUnavailable, is a no-op. Never-registered callbacks still throw;
a failed first registration does not count as success.

Timeout eligibility and removal of its captured registration are one operation
under the registry lock. Stale queued timeouts/snapshots cannot remove a replacement
registration. The last listener is detached atomically, then removed from its
own service outside the registry lock. Replacement registration can proceed while
old remote removal is blocked, and removal still names the old listener. User
onUnavailable callbacks run after retirement and outside that lock.

## Evidence and reproduction

- `tools/test_network_builder_contracts.py`: 120 SDK-only caller assertions and
  65 hidden module/value assertions pass on the host. The SDK caller is compiled
  separately against the official SDK36 JAR, without runtime implementation
  classes on that compilation classpath.
- `tools/test_connectivity_callback_contracts.py`: 28 host assertions pass,
  including distinct equal callbacks, failed registration, repeated cleanup,
  queued old events, timeout callback reuse, once-satisfied requests, concurrent
  replacement during blocked remote removal and remote removal failure.
- With `--source-ref 6bfc980c335a8b8ae1a9271dc5da43078561b516`, the four network
  groups `bounds`, `specifiers`, `legacy`, `enterprise` fail at their intended
  assertions. New value dependencies remain available in this control to isolate
  the old NetworkCapabilities/NetworkRequest behavior.
- The same old-source control fails callback groups `basic`, `identity`,
  `timeouts` and `teardown` at the missing idempotence, identity collision,
  timeout cleanup and registry-lock boundaries respectively.
- Fresh app and services-test javac/D8 builds pass against the verified reused
  core/framework compile artifacts; the existing 15 request-clear assertions
  also pass. This is a fresh Java build, not a full native platform build.

Host native adapters provide declared initialization/logging/clock/Binder-holder
boundaries. They do not substitute Parcel. Callback scheduling uses a legal
Handler subclass with deterministic message admission, and the service supplies
constructed network snapshots. This is not a Windows or live network test.

Required Windows main classes are `gesher.services.NetworkBuilderContractTest`,
`gesher.services.NetworkBuilderInternalContractTest` and
`gesher.services.ConnectivityCallbackContractTest`. Their Windows entrypoints
require actual native Parcel round trips. The callback fixture additionally
routes service requests through actual Binder Parcel dispatch and executes a
timeout/cleanup on a real HandlerThread. Their output must report `parcel=true`.
Actual published-head CI results, reported separately, establish those gates.

## Remaining boundaries

Build-time restricted/VCN capability normalization, explicit enterprise ID
configuration, hardware-backed interface selection and all public module APIs
are separate work. The existing same-object concurrent double-registration
rejection remains; this pass repairs reuse after retirement and distinct callback
identities. Registration setup RPCs still use the existing registry lock. No
arbitrary APK compatibility, physical adapter behavior, Android wire-format
compatibility or cross-version Parcel compatibility is asserted.

## Primary references

- [NetworkCapabilities](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/framework/src/android/net/NetworkCapabilities.java): SHA-256 `c8618524256b165b833b773b51f539a6cb9b08d4ce6145b326f9657858ce8821`.
- [NetworkRequest](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/framework/src/android/net/NetworkRequest.java): SHA-256 `b6322f059cbb2c96b7a75fce385446d191915ff107e1e5cfa913e78561605051`.
- [ConnectivityManager](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/framework/src/android/net/ConnectivityManager.java): SHA-256 `846f9ab64c4c6f270038a82f23fc6beb24111d4c2b73ecbdb86cebdc868a0e24`.
- [Imported value source receipts](aosp-network-builder-values-2026-11-13.json).
