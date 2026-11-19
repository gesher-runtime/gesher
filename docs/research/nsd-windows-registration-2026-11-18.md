# Windows DNS-SD registration

This increment implements service registration and unregistration on the existing
browse/resolve foundation. Its selected SDK36 denominator is **seven declarations**:
two ordinary `registerService` overloads, `unregisterService`, and the four
`RegistrationListener` callbacks. The existing `AdvertisingRequest` manager
overload is additional pinned source-profile support; it is absent from that
selected SDK36 manager descriptor set. Service-info subscriptions remain separate.

## Ownership and callbacks

The real `Services` bootstrap publishes `gesher.nsd.registration`. It captures
actual Binder PID/UID, the installed `ApkPackage` and attached `ProcessRecord`,
checks INTERNET, and installs death ownership before admission. An additive AM
observer retires advertisements belonging to that exact process even if its
callback Binder is hosted elsewhere. Package removal revokes queued work before
withdrawal. Queued submissions and callbacks recheck the captured process and
package; PID reuse or reinstall cannot inherit the earlier ownership.

The persistent system-service process owns the driver, records and scheduler.
Logical unregister releases client ownership before independent goodbye snapshots
finish. `DeviceWork` observes actual deadlines and pending retirement. Existing
`createInProcess` mode cannot promise goodbyes after the entire process exits.
Only the isolated SystemServer bootstrap permits a server-local owner without an
app `ProcessRecord`. The read-only Binder dump uses existing control-plane
authorization and actual owner-process counters; it adds no registration privilege.

Listener identity is shared with discovery and resolution. Terminal events remove
the listener before executor submission. Initial Binder-equivalent copying owns
raw TXT and KEY; later genuine copies preserve the pinned copy constructor's KEY
omission. Accepted subtype updates retain original TXT/options and queued packet
snapshots. A failed update removes the manager listener but retains the earlier
server transaction for authenticated death/uninstall cleanup. The source's
200-transaction client limit includes retained entries and active browse/resolve
operations; the server counts its own registrations rather than trusting a client
claim about them.

Successful registration returns a fresh value containing only the selected name
and hostname: null type, zero port, and no echoed TXT or Network. Failure and
unregister callbacks use the manager's stored request. A success callback does not
certify a successful OS send or packet delivery.

### Bounded admission and reliable retirement

Ordinary request and backend-event admission is bounded before jobs accumulate on
the serial service dispatcher. Capacity failures use the existing typed failure
callback. For an already accepted ID, the terminal capacity event follows earlier
accepted updates and computes whether the previous advertisement remains owned.
Callback-token enrollment failures use one private `ServiceSpecificException`
code, which the real client bridge alone translates to the asynchronous NSD error.
Unknown private codes and `SecurityException` propagate. Actual transport
`RemoteException` remains distinct; a checked `RemoteException` is not used as an
application error reply that Android Parcel cannot serialize.

Every accepted backend ID reserves a coalesced withdrawal slot independently of
ordinary register/update queue capacity. Withdrawal preserves FIFO order with
accepted work, wakes a stopped owner, and cannot be displaced by a full data queue.
If a backend rejects forced cleanup, the service retains only the integer ID and
retries after one second. Its ID charge lasts until backend withdrawal admission;
the backend then owns goodbye and native-retirement completion. These are separate
milestones. Registration and stop-completion bridges hold weak request references,
so deferred backend work does not retain a retired callback, token, package or
process. A live request remains mapped until its one terminal callback.

Both normal and error retirement keep a failed-to-close driver in a closing state,
retry at one-second intervals, and avoid generation, reconciliation or poll calls
on that retiring handle. They retain ownership until release succeeds. The dump's
`pending_withdrawals` is required to reach zero alongside native and client counts;
an empty client map alone is insufficient evidence of cleanup.

## Pinned record core and schedules

[`nsd-registration-core-sources.json`](nsd-registration-core-sources.json) binds
five new imports and complete packet/writer declarations to Android 16 r4 Gitiles
blobs and a reproducible transformer. Apache notices remain. The private-package
adaptations remove an unused Looper dependency and inline one genuine helper.
Real feature-flag builder defaults remain unchanged. The primary sources include
[`MdnsRecordRepository`](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/service-t/src/com/android/server/connectivity/mdns/MdnsRecordRepository.java),
[`MdnsProber`](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/service-t/src/com/android/server/connectivity/mdns/MdnsProber.java),
[`MdnsAnnouncer`](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/service-t/src/com/android/server/connectivity/mdns/MdnsAnnouncer.java)
and [`MdnsPacketRepeater`](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/service-t/src/com/android/server/connectivity/mdns/MdnsPacketRepeater.java).

The genuine repository owns PTR, SRV, binary TXT, address, KEY and NSEC records,
conflict comparison, known-answer selection, TTL and cache-flush rules. Scheduling
preserves three probes 250 ms apart plus the completion interval, an initial
random delay, five-second conflict retry, eight exponential announcements and
three goodbyes starting at 100 ms with two-second intervals. Source goodbyes contain
only marked service/subtype PTRs with TTL zero and cache-flush clear. Shared-host
removal rebuilds survivors; a same-name replacement cancels the earlier exit work.

No selected scope means pending without an invented timeout. Skip-probing completes
logically on a real scope before the first transmission. Ordinary probing can
finish despite individual send errors, as pinned. Successful-send counters remain
separate from source timestamps that can mark attempted sends. An established
conflict first restarts the existing name; a further conflict while probing
renames it. No additional simultaneous-probe tie-breaking algorithm is claimed.

## Explicit Windows wire adaptation

A legal 1300-byte TXT with maximal labels produced a 1571-byte pinned announcement
and a 1550-byte no-additional fallback, exceeding the source's 1300-byte writer
buffer. A separate Windows layout adapter preserves record/writer bodies, gives
each packet fresh compression and section counts, and never clips a record.
Actual family MTUs come from `GetIpInterfaceEntry` with real indices, LUID and
generation. An oversized single RR is isolated and bounded to 9000 bytes including
IP/UDP headers. Actual fragmentation remains a Windows gate.

Multicast response TC stays clear. Complete unique RRSets form one immediate burst
with cache-flush on every member. Same-owner probe authority sets stay together;
TC does not indicate authority continuation. A multi-record same-owner set that
cannot fit its MTU fails that send attempt while the genuine schedule continues.
Source probe authority does not include TXT. These distinct message classes follow
the constraints in [RFC 6762 sections 10.2, 17 and 18.5](https://www.rfc-editor.org/rfc/rfc6762).

Current authoritative reply sets come from the genuine pure announcement-value
method. The actual `DnsUtils` provider's null/empty default selectors and complete
address sets are measured. Known-answer groups include actual scope/LUID
generation, IP family, source address and source port; legacy grouping also includes
the transaction ID. The source's strict **greater than half** suppression threshold
is retained and tested at exact half. It differs from RFC 6762 section 7.1's
**at least half** boundary. Required unique sets are completed from current
authoritative values, never from query RDATA.

QU uses mDNS framing; an off-prefix or unknown-prefix source receives multicast on
the same scope. Legacy replies use separate 512-byte framing, original ID/questions,
no cache-flush, TTL at most ten seconds, and complete required RRSets or TC.
No EDNS or TCP responder is claimed. Native unicast requires an observed query
endpoint and actual on-link prefix; off-prefix legacy delivery can fail honestly.

Windows transient/cluster flags and random suffix origin are not reinterpreted as
Android privacy flags. The original `updateAddresses(List<LinkAddress>)` body is
unchanged. A separately derived `updateWindowsAddresses` accessor uses actual
`InetAddress` values and genuine address flags because the pinned record builder
never reads a prefix. It preserves real multicast advertisement when Windows
reports an unknown prefix, without constructing a fictitious /32 or /128. Such
addresses remain ineligible for on-link unicast. Known-prefix output is byte-equal
to the original method, including genuine temporary-address filtering in the
comparison fixture. Windows does not claim that its available metadata identifies
Android temporary privacy addresses.

## Resource bounds

| Owner | Bound and release point |
| --- | --- |
| Service | 1024 clients, 1024 live/retiring IDs, 4096 pending ordinary admissions and 4096 pending backend events; a retiring ID stays charged through withdrawal admission |
| Backend | 1024 active registrations, 4096 scoped entries and 4096 ordinary queued commands; each of at most 5120 admitted IDs reserves one coalesced withdrawal slot |
| Pending replies | 256 groups, 8 MiB accumulated input and 16384 records, including retiring groups until released |
| Prepared burst | At most 1024 datagrams/8 MiB, with at most another 8 MiB for native validation; prepare/send/discard one complete burst at a time |
| Observed endpoints | A 1024-entry native LRU cleared on reconfigure/close; eviction produces a transport error, never fabricated delivery |

Native registry, socket, interface and address bounds are shared with the resolver.
Hidden paths are explicit: unavailable offload-only registration returns
BAD_PARAMETERS; host-only subtype updates remain unavailable because the pinned
copy path has no SRV, retaining prior cleanup ownership; large KEY/custom-host probe
sets can hit capacity failure. These do not enlarge the seven-declaration SDK
denominator.

## Measured evidence and remaining qualification

| Profile | Observed result | Limit |
| --- | --- | --- |
| Fresh Java compilation | 307 production sources, 856 classfiles; full services build includes 155 compiler inputs; app and services DEX generation passed | Genuine selected compile inputs, not a complete Windows bundle link or ART execution |
| Author host contracts | 373 core/layout/protocol/retirement checks, 24 manager checks, 243 service checks; earlier manager fails the same positive registration probe with error 0 | Declared clock, I/O, native identity, metadata and Binder-lifetime seams |
| Independent QA | Fresh complete compile and maintained gates passed; separately reproduced rejection/retry, live-stop completion, dead-stop collection and bounded close retries; 32 wire checks over 14 packets | Host execution and independently decoded output, not Windows transport |
| Import/provider audit | Five imports reconstructed exactly; seven SDK declarations/constants, nine advertiser JNI methods, 115 selected application classes and 1320 member references, zero unresolved selected references | Actual selected framework DEX plus genuine compile declarations, not a whole shipped boot/core census |
| Native build | Real-header x64/ARM64 checks and complete focused PE links passed; original direct-call resolver/registration profiles fail against real POSIX symbols | Hashed cached POSIX/import-library inputs; no Windows loading, packet delivery or fresh full-bundle link |
| Stock Android reference | 65 checks; 24 owned multicast packets, four QM query windows and four DNS-decoded TCP endpoint connections | July API36.0, UID 2000/package android/system-context; public String TXT only; periodic announcements may supply observations |
| Windows gate preparation | Official SDK36 APK and services DEX built; script AST and counter readiness/retirement checks passed | Actual Windows execution remains pending |

The stock reference keeps the source-pinned r4 profile separate from the older
July Android image. Its initial v1 run lacked a Network; a later v1 run registered
and emitted multicast but failed its required legacy-unicast observation. Those
failures remain retained. Version 2 was never installed or executed. Version 3
uses ordinary QM multicast observation with explicit freshness windows; it proves
record availability and the decoded TCP endpoints, not query causality or legacy
unicast. It does not qualify arbitrary non-UTF-8 TXT, candidate runtime behavior,
ordinary APK permissions or Windows.

The maintained Windows gate uses installed application clients, a persistent
service, an independent owned peer, full binary TXT, death/uninstall/replacement
and actual native/OS counters. Port 5353 coexistence, IPv4/IPv6 delivery, remote
Binder, application lifecycle, interface loss/return and low-MTU fragmentation
require actual Windows evidence. The runner does not alter host interfaces and
reports `fullWindowsMergeQualification=false` until the separate interface gates
are supplied. User-canceled CI remains canceled. Build commands and fixture
boundaries are in the [NSD test README](../../tests/nsd/README.md) and the
[installed-app gate README](../../tests/apps/nsdregistration/README.md).

### Native link failure and repair

A published PR149 x64 build failed at the final executable link with duplicate
`bind`, `sendto` and `setsockopt` symbols from guest POSIX and Winsock import members.
The transport also directly imported `select`, whose guest archive member was not
selected in that particular failed link. The narrow repair follows existing
Gesher Winsock boundaries: exact WinSDK-typed function pointers resolved from the
system DLL, process lifetime, and real lookup errors before `WSAStartup`.
Resolver and advertiser use the same repaired boundary. The maintained link target
forces all four actual guest exports into the link, with no alternate providers
or unresolved-symbol bypass. Its original-source controls fail and both repaired
profiles link on x64 and ARM64. Java source, APK, DEX and stock-reference payloads
remain byte-identical to their already executed checkpoints. This focused result
does not turn the failed published build into a success or establish the still
pending Windows execution/full current-bundle gate.
