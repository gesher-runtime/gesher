# N46: continuing DNS-SD service-info callbacks

This increment implements continuing named service observation through
`NsdManager.registerServiceInfoCallback` and `unregisterServiceInfoCallback`.
It replaces the former unconditional registration failure with a real service
owner and Windows multicast transport. **It does not close N46. Actual Windows
execution, installed-application ownership and network transition qualification
remain required.**

The selected SDK36 denominator is six declarations: the two manager methods and
the four `ServiceInfoCallback` methods (`onServiceUpdated`, `onServiceLost`,
`onServiceInfoCallbackRegistrationFailed`, `onServiceInfoCallbackUnregistered`).
The baseline is registration PR158, head
`eeb9ada576fa3694d6fb33e06f0f6311383d3e15`, on the existing browse/resolve stack.
Canonical main `826ba8a0e87335607350a7ced029b97cc232e324` is integrated; its
temporary pause of CI runs on main pushes is preserved. This change adds only
the subscription gate invocation to the shared Windows services track.

## Android contract and selected source profile

The implementation follows the Android 16 r4 Java mDNS profile. Primary sources
are [NsdManager](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/framework-t/src/android/net/nsd/NsdManager.java),
[NsdService](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/service-t/src/com/android/server/NsdService.java),
[MdnsServiceTypeClient](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/service-t/src/com/android/server/connectivity/mdns/MdnsServiceTypeClient.java)
and [MdnsResponse](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/service-t/src/com/android/server/connectivity/mdns/MdnsResponse.java).

A selector identifies one service instance. A complete response requires SRV,
TXT and an address for the actual SRV target; base PTR is optional. Updates carry
the bare base type, observed hostname/port, actual Network or unmapped interface,
IPv4-then-IPv6 address list, scoped link-local IPv6, binary TXT and observed
subtypes. A literal dot, backslash or Unicode character in an instance remains
within that one DNS label. A subtype suffix in the selector does not invent a
subscription subtype filter.

The pinned response decoder preserves null and empty TXT values separately and
does not decode arbitrary TXT bytes as Strings. The first case-insensitive
non-null value wins; a later duplicate may replace a null value. Returned values
own their arrays. An unchanged receipt refresh extends the observed lifetime
without producing another update. A meaningful complete response change produces
an update. In this source profile, SRV expiry, whole-service goodbye and actual
scope retirement remove the current complete observation; TXT/address/subtype
expiry alone does not purge it. The pinned whole-service goodbye rule includes
a base PTR with zero TTL. The stock reference retains that expectation explicitly
so a different installed module profile cannot silently redefine the oracle.

The genuine passive-query scheduler governs ordinary refreshes. Progress can ask
for a newly learned host, while each question retains its one-second minimum
interval. A schedule is charged only after the transport confirms an actual
complete send. An unavailable or explicitly absent Network stays pending without
fallback sockets, an invented success or an arbitrary terminal timeout.

## Public callback and service ownership

Manager entries are keyed by callback identity, including the existing
browse/resolve/registration identity maps. Input validation retains synchronous
errors. Initial backend failure uses the registration-failure callback. The
manager dispatches through its existing service executor and the caller's
executor, removes terminal entries before executor submission, and suppresses
queued nonterminal updates after cancellation or callback reuse. There is no
public successful-registration callback; private READY only establishes actual
owner progress.

The real `Services` bootstrap publishes `gesher.nsd.subscription`. Its production
authority captures Binder PID/UID, the installed package and exact attached
process, checks INTERNET, and retains authenticated death ownership. Process death
and package removal revoke pending work and release callback references before
backend withdrawal. An isolated service-process bootstrap is the existing
explicit system owner case; ordinary application calls use their real identity.
The read-only dump uses the existing system control-plane authority.

The client bridge keeps operation and remote-service generations separate.
Ambiguous transactions against a live Binder retry the same callback token and
request ID, and enrollment is idempotent at that boundary. Actual service death
retires previously observed scopes and reconnects established subscriptions to a
new owner. It does not fabricate another initial registration failure or report
unregistration before the actual replacement owner has accepted cancellation.
Definite initial permission/identity denial propagates its synchronous exception
and releases its operation, wire entry and idle death recipient without retry.
Unknown potentially accepted transactions retain cleanup ownership.

Finite producer backpressure preserves accepted event order. Stop and revocation
release blocked producers without consuming an ordinary admission slot. Failed
forced withdrawal retains only the bounded backend ID for retry. A subscriber
worker retains a failed-to-close native handle, makes no further query/snapshot
calls on it, and retries closure once per second. Normal stop completion follows
confirmed native retirement. `DeviceWork` observes pending work and retirement.

## Windows observations and native ownership

The shared native library has a distinct subscriber mode and an eight-method
`kSubscriptionMethods` table. The original resolver eight-method and advertiser
nine-method table bodies remain byte-identical to the PR158 baseline. Native
progress and ownership counters are separated by mode; the real socket budget is
shared.

Each real adapter snapshot carries canonical, IPv4 and IPv6 indices, LUID,
packet generation, available families and a request-owned observed lifetime.
Packet generation rejects stale packet attribution. Lifetime continuity permits
an unchanged observation across an unrelated notification or a surviving-family
handover, while observed departure and return invalidate its earlier data. A
concurrent or unresolved snapshot returns retry, never a fabricated empty set.
The existing WindowsNetworks snapshot supplies Network identity.

IP Helper callback rows supply identifiers only. Complete operational/address
state comes from the actual Windows query APIs. Callback leases enter a snapshot
fence before waiting on the continuity mutex; snapshot publication also checks the
generation after capture. Native callbacks do no JNI or socket I/O. The worker
closes sockets and cancels both notification registrations outside locks needed
by callbacks. Failed cancellation retains its exact handle. Details and primary
Windows references are in [the native subscription note](../../tests/net/README-nsd-subscription.md).

The transport reuses actual request-owned Winsock multicast sockets and the
existing packet metadata qualification. A complete subscriber send requires a
full datagram on every configured family; an absent/stale/retiring channel or a
partial family send cannot report success. The six diagnostic fields are live
requests, sockets, watches, entered callbacks, polled requests and actual sent
datagrams owned by live requests. A stopped request leaves all six at zero.

| Resource | Bound |
| --- | --- |
| Client bridge | 1024 operations and 4096 pending deliveries per connection |
| Service | 1024 clients, 1024 live/retiring IDs, 4096 pending admissions, 4096 backend events and 128 events per request |
| Worker | 200 concurrent owners and 64 total observed scopes per owner, checked before selector filtering |
| Cached named response | 64 addresses and 65 pointers per scope; real record excess is an availability error |
| Native | Existing shared request/socket/adapter/address limits; finite request-owned continuity ledger |

## Source reconstruction and defining-provider evidence

[`nsd-subscription-core-sources.json`](nsd-subscription-core-sources.json) pins
seven upstream inputs by Gitiles URL, Git blob and SHA-256. Six complete source
files retain their bodies after private-package relocation and explicit utility
reference changes. Three complete utility declarations form the private adapter.
The transformer and its block helper are hashed, and original Apache notices
remain. Feature-flag defaults are the genuine existing source defaults.

The actual fresh application DEX and retained framework DEX were parsed, rather
than treating the official SDK jar as a runtime implementation. The six public
SDK descriptors and flags match both the application classfiles and DEX. The
selected closure has 121 application definitions, no framework overlap and 1187
distinct Android/Gesher member references: 1087 resolve to the fresh application
DEX, 96 to the retained framework DEX and four inherited declarations to genuine
core compile inputs. There are zero unresolved selected references. These four
core declarations are compile evidence, not a packaged-core runtime census.
Direct core-library calls and the remainder of the SDK are outside this audit.

| Measured artifact | SHA-256 |
| --- | --- |
| Official SDK36 declarations | `d9eb9da824d9e247a352f570f01e1169e725b2954bca9e283a71786c59b59f9a` |
| Retained actual framework DEX archive | `fed525b7fd92cfe2b82ac547172d545dd2f4073a6bb67740906b15ec7c8c1bb7` |
| Fresh application DEX archive | `a0b1b83138b8f659eeea897639b2bff6805eae6227d3a4e0cdfff08b85c7e068` |
| Fresh services test DEX | `4bf25617d805b65cec84228d4803a2e2348d81e8c243f96d2949e62c33348d55` |

## Measurements and qualification limits

Production/test source at `5202b795790ec0d1f189b3a8d66d8517c63e45b4`
passed the following author checks. Later documentation-only changes do not
change those artifact inputs.

| Check | Observed result | Boundary |
| --- | --- | --- |
| Complete unmodified app build | Passed in 25.808 seconds | Genuine selected framework/core/AOSP inputs; no Windows execution |
| Complete services test build | Passed in 9.649 seconds, including the new Windows gate class | Genuine compilation and DEX generation |
| Response state | 41 checks | Deterministic valid records and explicit clock |
| Manager and client | 252 checks | Real classes with explicit executor, service and Binder-lifetime seams |
| Service ownership | 46 checks | Real service logic with explicit host authority/backend seams |
| Worker lifecycle | 55 checks | Actual worker and explicit driver/clock boundaries |
| Independent loopback peers | 15 IPv4 and 15 IPv6 checks | Real owned UDP/TCP sockets; injected loopback attribution, no Winsock claim |
| Native lifetime/fence/retirement | 97 checks under ASan/UBSan | Production helpers and finite ordinary observations; leak checking unavailable under this host and explicitly disabled |
| Native Windows compile/link | Complete transport plus genuine 49-source POSIX library and guest-symbol fixture linked for x64 and ARM64 | Compiler/linker input hashes retained; neither PE was executed |
| Windows gate preparation | Real PowerShell parser passed; Java gate is in the actual services DEX | Actual execution still pending |

Independent review identified two concrete lifecycle errors in earlier source:
a definite initial permission denial retained an unaccepted request and scheduled
repeated enrollment, and an unknown scope snapshot deferred already-known SRV
expiry. The corrected client releases definite pre-admission denial, while the
worker expires records before attempting scope reconciliation. Maintained finite
regressions cover both boundaries. In the expiry fixture, the genuine parser's
SystemClock receipt time bounds the explicit worker clock advance; no arbitrary
timestamp epoch is used. These changes preserve ambiguous-transaction ownership
and unknown-scope semantics separately.

The immutable SDK36 ordinary-APK reference uses public APIs, actual application
UID/Binder UID and an owned independent multicast/TCP peer. Its builder creates
reviewable setup/execution/cleanup arrays and never invokes a device. The frozen
v1 APK SHA-256 is
`92e77717ea4df32ac8514e5d7d6961a804056adc1a95a89f402929bb56f337f2`.
Building or reviewing this capsule is not an Android runtime pass. Actual stock
observations must retain their image/module profile and original result, including
any difference at the base-PTR goodbye assertion.

The maintained Windows gate requires real IPv4 and IPv6 on the selected adapter,
independently encoded valid announcements, a real resolved TCP endpoint, binary
updates, loss/reappearance, actual sent-datagram progress, an absent Network and
zero native ownership after stop. It reports `ordinaryApkIdentity=false` and
`remoteBinder=false`, because it calls the actual native backend directly from a
services fixture. It cannot qualify installed-app permissions or process death.

Still required are actual Windows loading and multicast coexistence, real
adapter/address transitions, independently observed notification ordering,
installed-app callback execution and permissions, remote Binder service
replacement, process death/uninstall, and repeated retirement measurements on
the target architectures. Host checks, a stock Android result and cross-links
cannot substitute for those gates. No CI run is dispatched or restarted by the
new scripts.

See [the reproducible subscription test instructions](../../tests/nsd/README-subscription.md)
for input requirements and each fixture's explicit boundary.
