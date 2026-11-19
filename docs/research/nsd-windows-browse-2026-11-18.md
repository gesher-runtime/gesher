# N46: Windows NSD discovery transport

## Status and scope

This is an intermediate issue #56 / N46 implementation. It replaces empty successful
discovery with real Windows multicast DNS queries and tracks observed services,
network membership, cancellation and resource ownership. It does **not** complete
N46. Registration, resolution and service-info subscriptions still report
`FAILURE_INTERNAL_ERROR`; they do not advertise success or invented service data.

**Actual Windows execution is pending.** The native translation unit cross-compiles,
the Java contracts run on the host, and an Android 16 reference probe ran successfully.
None of those results qualifies the Windows production path. The required Windows
services-track gate must pass before this increment can be recommended for merge.
Canceled CI workflows were not restarted or manually dispatched during this work.

## Sources and selected API

Android behavior is pinned to the Connectivity module's
[`android-16.0.0_r4` NsdManager][aosp-manager] and [NsdService][aosp-service]. The
discovery cache details were checked in [MdnsServiceTypeClient][aosp-client],
[MdnsResponse][aosp-response] and [MdnsUtils][aosp-utils] at the same tag.

The Windows transport dynamically loads `DnsStartMulticastQuery` and
`DnsStopMulticastQuery` from the system `dnsapi.dll`. The [start contract][win-start]
describes an asynchronous query that continues until stopped and invokes its callback
for each network response. The request uses `DNS_TYPE_PTR`, version 1,
`DNS_QUERY_STANDARD`, and a concrete interface index. The actual SDK definitions of
[MDNS_QUERY_REQUEST][win-request], [MDNS_QUERY_HANDLE][win-handle] and
[MDNS_QUERY_CALLBACK][win-callback] are used directly, without custom ABI structs.

An earlier unqualified draft used `DnsServiceBrowse`. Its contract does not establish
the refresh/removal stream this cache needs; Microsoft staff also describe missing
removal notifications in [this support discussion][win-browse-removal]. That draft is
superseded. Its compilation is not evidence for the final API.

The selected platform must actually export and execute both multicast functions. An
absent API, rejected request, malformed result or resource limit produces an explicit
failure. Microsoft's documented minimum client is Windows 10 desktop. Execution on
the repository's Windows runners remains required, including Unicode result encoding
and the records supplied by the callback.

## Contract and ownership

| Area | Implemented behavior |
| --- | --- |
| Dispatch | A service handler serializes submissions to the app executor. The default direct executor receives asynchronous handler callbacks. A supplied executor still controls execution concurrency. |
| Listener identity | Identity keys, duplicate rejection, pending-stop reservation, terminal removal before app callback submission, stale-session suppression and listener reuse. A quota failure reserves its listener until its terminal message. |
| Networks | Default discovery observes operational multicast-capable adapters. Explicit Network discovery uses only that real interface. NetworkRequest discovery tracks actual ConnectivityManager callbacks, including loss followed by return of the same index between polls. |
| Names and types | Unicode instance text is retained. Cache identity uses ASCII DNS case folding. Found/lost types have the Android trailing dot. API36 type/subtype precedence is preserved; unsupported multiple-subtype requests fail. |
| Values | Results contain the actual Network(index + 100). The hidden local-only interface fallback stays zero. Port is zero, addresses are absent and TXT is empty because browsing is not resolution. Only the queried subtype is included when its PTR was observed. |
| Lifetime | PTRs establish membership. Observed SRV records, including separately delivered records, update known lifetime. SRV alone never creates a discovered service. Refresh does not duplicate found callbacks. |
| Termination | Stop succeeds only after native stop succeeds. Failure is delivered once while native-only retirement retains and retries unresolved native ownership. App callbacks and network scopes are detached before that wait. |
| Permissions | Context and native boundaries enforce the app's INTERNET entitlement. NetworkRequest binding enforces ACCESS_NETWORK_STATE and uses ordinary connectivity callbacks. Android 17 local-network authorization is outside this Android 16 profile. |

The index mapping follows Gesher's existing Windows network identity. This change
does not alter ConnectivityManager. Network snapshots include a generation so a
loss/return transition cannot be erased by a one-second membership poll. Executor
rejection stops native work and removes its listener. Failed connectivity callback
registration is never followed by unregistering an unknown callback, preserving the
original permission/quota exception.

### Native cancellation

[`DnsStopMulticastQuery`][win-stop] returns a real success/error result. Its docs do
not promise a final cancellation callback or say that previously entered callback
threads have returned. This implementation does not invent such a Windows callback.

Each request has a monotonically allocated numeric context ID. A registry owns its
request, query string and handle. Every entered callback obtains a shared owner and
retains it through freeing the returned record list. After successful stop, Java
consumes a private completion token and removes the registry entry. Entered callbacks
still retain their shared owners. A late callback with an absent ID frees its records
and never dereferences the supplied handle or a released context address. `nativeActive`
counts live request containers, including callback leases, rather than registry entries.

No JNI runs on Windows callback threads. They queue copied PTR/SRV observations and
free the Windows record list with the SDK's DnsRecordListFree/DnsFree contract. Java
subtracts native queue age from received TTLs. Real cancellation races and eventual
zero live containers remain part of the unexecuted Windows gate.

Limits are explicit: 200 logical requests per manager, 64 interfaces per request,
4,096 discoveries and 4,096 SRV metadata entries per interface, 1,024 queued native
events per request, and 512 live native requests process-wide. The last bound includes
persistent stop failures and requests retained by entered callbacks. Listener reuse
cannot evade it. Failed stops retry at most once per second per request. Capacity
failures report `FAILURE_MAX_LIMIT`, without silently dropping events or freeing
Windows-owned storage.

### Expiration and goodbye

The API36 source derives expiration metadata from SRV lifetime, not PTR TTL. A bounded
SRV cache joins that metadata to observed PTRs and retains the previous positive
snapshot for goodbye. Split SRV refresh/goodbye is handled without initiating a resolve.
Known metadata is passed to NsdServiceInfo with upstream second precision.

The pinned AOSP code removes a service immediately on a base/SRV goodbye. Subtype-only
PTR TTL zero does not mean the whole service disappeared; r4 explicitly documents its
incomplete subtype-expiration handling. The transport preserves that distinction.

There is an explicit bounded difference for incomplete records: if Windows provides
no matching SRV, expiration is unknown/null and membership expires using observed PTR
lifetime. Raw r4 AOSP constructs a far-future sentinel from Long.MAX_VALUE milliseconds
and uses only SRV for cache expiration. This increment does not claim exact sentinel
fidelity or label PTR lifetime as observed SRV metadata. The Windows gate records whether
SRV metadata was actually supplied, without substituting known fixture values for
missing native evidence.

## Validation

| Evidence | Result and limit |
| --- | --- |
| Production Java with explicit transport fixtures | NsdContractTest: 63 checks; NsdBrowseTransportTest: 182 checks. Host contracts, not Windows networking. |
| Owner retention | An app owner is garbage-collected while failed native stop remains outstanding. Native storage remains retained until a later successful stop. |
| Java build | 280 production inputs against real framework/core classes, with a clean output directory. |
| Services build | 150 service/test inputs compiled and converted to API36 DEX. |
| Native build | Production windows_nsd.cc cross-compiled using llvm-mingw 20261006, Clang 23.1.3, Windows x64 and repository native flags. No Windows execution. |
| Android reference | 19 checks passed on the isolated API36 Android 16 image, in 5.079 seconds; qualification below. |
| Ordinary Android APK permission reference | Nine measured cases across three SDK/target36 apps, with distinct ordinary app UIDs and verified manifest/self-permission states. Windows APK enforcement was not executed. |

Host cases cover missing APIs, native errors, invalid queries, subtype precedence,
interface isolation, split PTR/SRV delivery, differing PTR/SRV TTLs, queue age, goodbye
snapshots, subtype-only goodbye, refresh, silent expiry, network loss generations,
limits, cancellation retries, callback exceptions, executor rejection and 100 immediate
stop cycles. Run `tools/test_nsd_browse_contracts.py --help` for its explicit classpath
inputs. Its substitutions are supplied directly by fixtures; the production factory
always loads the native driver.

### Android reference qualification

`tests/nsd/NsdAndroidReference.java` uses the official API36 SDK. The read-only,
device-hashed DEX SHA-256 was
`ce92f3b8cabb0416af49617a44c96d0f81a7fa5dc5b9e14e7318756e81ea0294`.
The image fingerprint was
`Android/sdk_phone64_x86_64/emu64x:16/BE2A.250530.026.D1/13818094:userdebug/test-keys`.

The public SDK calls ran through app_process with a system context under shell identity.
This is **not ordinary-APK target/manifest/permission qualification**. The unique valid
type, owned listening socket and Unicode/dot/backslash name were actually registered,
found, unregistered and lost. Found/lost had the trailing-dot type, Network 102, zero
port, no host and empty TXT. Every callback used the named executor. Found and goodbye
lost retained the same non-null expiration. The SRV provenance is established by the
separate AOSP source trace, not by decoding records in this reference probe.

A public String TXT input containing embedded NUL remained intact in the unregistration
callback's byte-array attributes. This run did not separately decode Android wire TXT
or exercise resolution; those are not claimed results. Fixture labels obey
[RFC6335 §5.1][rfc6335] and [RFC6763 §7.2][rfc6763]. Production validation was not
tightened beyond AOSP's grammar.

An independent QA fixture then exercised three ordinary APKs on the same API36 image
(coordinated receipt 040). Its source SHA-256 is
`479833635b482727d1cbb27d947730c63b45101ec034b5bbb318474eca6c03ae` and shared DEX
SHA-256 is `981a0c5e0527392a7b866424570b8a02acb1682f1e3b7fa9b02718e120715c55`.
All nine acquisition/ordinary-discovery/NetworkRequest cases completed without timeout.

| APK declarations | Measured API36 outcome |
| --- | --- |
| No INTERNET | Manager acquisition synchronously throws SecurityException. Discovery is not attempted without a manager. |
| INTERNET only | Acquisition and ordinary discovery/start/stop succeed. NetworkRequest discovery synchronously throws SecurityException for ACCESS_NETWORK_STATE and emits no callbacks. |
| INTERNET and ACCESS_NETWORK_STATE | Acquisition and both discovery overloads succeed; start/stop use the named executor. |

These ordinary-app measurements support the constructor and request-boundary permission
checks. They are distinct from the shell/system-context discovery probe, and they do
not establish ordinary-APK permission enforcement on Gesher/Windows.
The unchanged receiver source is included under `tests/nsd/permissions`. The three
manifests now disable debugging without changing the measured permission matrix.
`tools/build_nsd_permission_reference.py` parameterizes SDK/JDK/output paths and rebuilt
the identical frozen DEX. Its revised locally signed APKs were verified but have not
been executed on an Android device; their hashes are not substituted for the original
measured APKs, and the measurements above remain tied to those original artifacts.

### Required Windows execution

`tests/windows/run-nsd-browse-contract.ps1` is part of the required services track. It
runs the public manager/native driver against a separate Python standard-library wire
peer. The peer advertises only a random owned type, opens a real TCP listening port,
and constructs UTF-8 DNS labels and binary TXT independently of Gesher/Windows DNS APIs.
PTR TTL is two seconds and SRV TTL five seconds; refresh testing exceeds both.
Goodbye cases use two other fresh owned types, with PTR TTL 120 seconds and SRV TTL
180 seconds. The gate verifies a native-observed positive PTR TTL of at least 60 seconds
(and long SRV expiration when available), then requires loss within three seconds.
Silent expiry therefore cannot stand in for a processed goodbye.

The gate requires real found/lost, silent expiry, goodbye, subtype discovery, Unicode
identity, explicit-network isolation, permissions, 50 immediate-stop races, no callbacks
after successful stop, and zero retained native containers. IPv4 is required. IPv6 runs
when the selected adapter has a preferred IPv6 address; otherwise it is explicitly
reported as unexercised. Missing APIs or callbacks fail. Earlier Windows Server
CI reached the subtype PTR on Server 2022 and timed out waiting for the base-only
goodbye. Server 2025 timed out at the subtype response.

The gate also sends a base-only goodbye while browsing a subtype. A direct
`DnsStartMulticastQuery` check on Windows desktop delivered the zero-TTL base PTR
to the exact base query. The subtype query received the earlier positive subtype
PTR and did not receive that goodbye. Each subtype browse now owns a separate base
query on the same interface. Its callback enqueues only base PTR deletions into
the same native FIFO, and both queries must retire before successful stop. This
direct API check does not establish end-to-end ART or Windows Server behavior.

## Remaining N46 work

Registration and resolution need a responder/resolver that preserves arbitrary binary
TXT, including embedded NUL. Microsoft's [DNS_SERVICE_INSTANCE][win-instance] and
[DnsServiceConstructInstance][win-construct] property representation uses null-terminated
WCHAR key/value strings. Routing Android byte attributes through it would be lossy.
This increment adds no responder dependency and claims no registration, conflict,
resolution or binary-TXT completion.

A concrete follow-on candidate is a per-interface responder integration using the
Apache-2.0 [JmDNS v3.6.5 release][jmdns], after reviewing NOTICE/relicensing history,
threading, sockets and dependencies. Its source was researched, not imported. Acceptance
must include independent wire peers and Android references for arbitrary TXT, Unicode
labels, conflicts/renaming, ports and IPv4/IPv6 addresses, binding, executor ordering,
duplicates, cancellation and owner lifetime. Windows ordinary-APK permission qualification
remains required; Android reference coverage must extend to the future responder APIs.

[aosp-manager]: https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/framework-t/src/android/net/nsd/NsdManager.java
[aosp-service]: https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/service-t/src/com/android/server/NsdService.java
[aosp-client]: https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/service-t/src/com/android/server/connectivity/mdns/MdnsServiceTypeClient.java
[aosp-response]: https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/service-t/src/com/android/server/connectivity/mdns/MdnsResponse.java
[aosp-utils]: https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/service-t/src/com/android/server/connectivity/mdns/util/MdnsUtils.java
[win-start]: https://learn.microsoft.com/en-us/windows/win32/api/windns/nf-windns-dnsstartmulticastquery
[win-stop]: https://learn.microsoft.com/en-us/windows/win32/api/windns/nf-windns-dnsstopmulticastquery
[win-request]: https://learn.microsoft.com/en-us/windows/win32/api/windns/ns-windns-mdns_query_request
[win-handle]: https://learn.microsoft.com/en-us/windows/win32/api/windns/ns-windns-mdns_query_handle
[win-callback]: https://learn.microsoft.com/en-us/windows/win32/api/windns/nc-windns-mdns_query_callback
[win-browse-removal]: https://learn.microsoft.com/en-us/answers/questions/111335/dnsservicebrowse-does-not-generate-events-for-remo
[win-instance]: https://learn.microsoft.com/en-us/windows/win32/api/windns/ns-windns-dns_service_instance
[win-construct]: https://learn.microsoft.com/en-us/windows/win32/api/windns/nf-windns-dnsserviceconstructinstance
[rfc6335]: https://www.rfc-editor.org/rfc/rfc6335.html#section-5.1
[rfc6763]: https://www.rfc-editor.org/rfc/rfc6763.html#section-7.2
[jmdns]: https://github.com/jmdns/jmdns/releases


## Companion follow-up validation

The shared native FIFO and grouped cancellation in the current repair are
preserved. A companion base query now also accepts an actual requested-subtype
PTR when Windows includes that record in its callback. A positive base PTR still
cannot establish or refresh subtype membership, and unrelated subtype records
are ignored. A base goodbye in the same batch suppresses subtype membership
from that batch; base withdrawal remains owned by the companion and shares the
same ordered queue as the primary query.

The production PTR classifier has host regressions for base positives, actual
subtype positives, base withdrawals, subtype-only withdrawals, unrelated owners,
case and trailing-dot spelling. The Java fixture additionally tests failed
companion start combined with failed cancellation, callback detachment, backoff
and eventual release. The real Windows gate now first publishes base-only
positive records and requires zero discovery, then retains the existing subtype
positive and three-second base-goodbye checks. Timeout diagnostics include native
callback/record/membership/goodbye/error/status counters and listener counts.

These source and fixture improvements do not establish that Windows 2022 delivers
the requested subtype. The actual IPv4/IPv6 gate still needs to run on both Windows
versions; no successful Windows outcome is claimed for this candidate.
