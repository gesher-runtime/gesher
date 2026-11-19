# N46: binary-preserving one-shot DNS-SD resolution

This increment implements the two SDK36 `NsdManager.resolveService` overloads and
`stopServiceResolution`, with a request-owned Windows multicast transport. It
preserves arbitrary TXT bytes and only resolves from an actual complete SRV, TXT
and address observation. Registration and service-info subscriptions remain
unavailable. **This increment does not complete N46. Actual Windows execution is
still a required merge gate.**

## Dependency and ownership

The source base is published browse PR147, remote head
`0cdd21a6b4efa973323633c2fafcb6f970c871b6`, tree
`de0d50c5b904ee2971043385d6cae2b34a84d9a8`. The isolated author checkout uses the
exact tree-equivalent local commit `62198215f0a672f89869fa8d5d37a0d1c8fa5afc`.
Publication must use the remote PR147 head as parent after rechecking that tree.
The earlier browse and Wi-Fi value candidates are unchanged.

The refreshed open-PR ownership inventory has no separate owner for the new
resolver or codec paths. PR147 owns the existing manager/native build integration.
The Windows services track has several other owners; this change adds only its
new gate invocation. It does not absorb the unrelated WebView change in main
`1e14cbb8629e1defba0009f58996630e51bc0c19`, change ConnectivityManager, or touch
the held N09/N15 work.

## Codec, source license and defining providers

The 15-file codec/value closure is pinned to AOSP `android-16.0.0_r4`, under
`packages/modules/Connectivity/service-t/src/com/android/server/connectivity/mdns`.
Original Apache-2.0 notices are retained. The only source adaptations are package
relocation to `com.gesher.net.mdns` and removal of unavailable
`androidx.annotation.VisibleForTesting` / `ChecksSdkIntAtLeast` build annotations.
The complete upstream `MdnsFeatureFlags` value and defaults accompany the reader;
no fake runtime flag or replacement platform provider is introduced. The upstream
label-count limit remains enabled. `MdnsResponseDecoder` is not imported.

[`nsd-resolver-codec-sources.json`](nsd-resolver-codec-sources.json) records every
primary URL, upstream SHA-256 and adapted SHA-256. The reproducible provenance
tool verifies each adaptation byte-for-byte and checks the compiled signatures.

The selected distributable archives were reparsed as DEX definitions:

| Actual selected input | SHA-256 | Definition count |
| --- | --- | ---: |
| `framework.jar` | `fed525b7fd92cfe2b82ac547172d545dd2f4073a6bb67740906b15ec7c8c1bb7` | 34,764 |
| `gesher-app.jar` | `6ea29dee02b9ac9239a043bdf169aff73ead17dd10f5fd060792acc4a691441a` | 2,403 |

These are distribution run `44208723771`, artifact `14386510665`, source
`63d8db3e8e4a7c992d8daa0c027860bb759dcdaa`, checkout
`76ac2cd2be8469ea88acc9145c4c51ffeb313a96`. Existing NSD values and manager are
app-defined; neither selected archive supplies the new resolver/codec, JmDNS or
SLF4J. This is a two-archive observation, not a complete runtime census.

The fresh application closure contains 47 selected manager/resolver/codec class
definitions. Its 459 distinct Android/Gesher member references resolve as 418
fresh application declarations, 39 actual selected framework DEX declarations,
and two inherited `java.lang.Thread` declarations from genuine core compile
classfiles. Those two core declarations close `HandlerThread.start/setDaemon`
for compilation; they are **not** packaged-core-DEX provider evidence. Direct
`java.*`, `javax.*` and other core calls are outside this selected member audit.

The official SDK36 jar, SHA-256
`d9eb9da824d9e247a352f570f01e1169e725b2954bca9e283a71786c59b59f9a`, supplies the
**seven-declaration denominator**: two resolve overloads, one cancellation
method, and four ResolveListener methods. Exact descriptors and access/static
flags match. This denominator describes this increment, not the whole NSD API.
SDK stubs never serve as runtime providers. All eight private JNI method
descriptors and the native-created Packet constructor `(JIII[B)V` match the
fresh classes.

## Why raw multicast sockets

Windows' `DNS_SERVICE_INSTANCE` convenience API exposes null-terminated WCHAR
properties. It cannot transport Android's opaque `byte[]` TXT values through a
String conversion. JmDNS v3.6.5 was examined but not imported: its current
Apache-2.0 relicensing notice is acceptable, while its label encoding,
registration completion, recovery, shutdown and socket integration require a
larger adaptation than this resolver slice.

The new native transport uses actual nonblocking Winsock UDP sockets, real
`SO_REUSEADDR`, port 5353, per-interface memberships/output selection, and
IPv4 TTL / IPv6 hop limit 255. Questions request multicast responses, without
the QU bit. Microsoft documents multicast delivery to both sockets sharing an
interface, port and group; this is a design basis, not a substitute for testing
coexistence with the Windows DNS service. An ephemeral-port legacy-unicast path
was not selected because its response-size/truncation behavior does not establish
this large-TXT contract without additional transport work. See the
[multicast socket rule](https://learn.microsoft.com/en-us/windows/win32/winsock/using-so-reuseaddr-and-so-exclusiveaddruse)
and [RFC6762](https://www.rfc-editor.org/rfc/rfc6762).

`WSARecvMsg` supplies packet-interface, destination and hop metadata. Only an
untruncated ordinary response from source port 5353, to the multicast group,
with hop limit 255 and the selected actual family/interface/generation can enter
the state machine. Queries, other opcodes, error responses and wrong-scope
packets cannot establish success. See Microsoft's
[WSARecvMsg contract](https://learn.microsoft.com/en-us/windows/win32/api/mswsock/nc-mswsock-lpfn_wsarecvmsg).
Network fixtures use valid owned protocol traffic; resource and failure paths
use explicit controlled seams.

## Android-visible result and lifecycle

The contract follows the pinned r4 Java mDNS service, not its historical native
daemon branch. Primary sources are
[NsdManager](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/framework-t/src/android/net/nsd/NsdManager.java),
[NsdService](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/service-t/src/com/android/server/NsdService.java),
[MdnsResponse](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/service-t/src/com/android/server/connectivity/mdns/MdnsResponse.java),
and [MdnsUtils](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/service-t/src/com/android/server/connectivity/mdns/util/MdnsUtils.java).

Resolution requires unexpired SRV, an observed TXT record (which may be empty),
and at least one usable address for the actual SRV target. PTR is optional.
Success creates a separate NsdServiceInfo with the requested named-resolution
label/type seed, leading-dot resolved type, observed port/hostname, actual
addresses, remaining SRV-derived expiration and actual network scope. A name
containing Unicode, a literal dot or a backslash remains one DNS label.

TXT keys with no equals sign retain null; `key=` retains an empty array; bytes
after the first equals sign remain opaque, including NUL, invalid UTF-8 and
further equals signs. The case-insensitive upstream map preserves the first
non-null value, while a later duplicate can replace an earlier null. Returned
arrays are owned copies. Invalid/oversize value entries are skipped according to
NsdServiceInfo policy without turning an observed empty TXT into an absent TXT.
The lossy legacy raw-TXT helper methods are not used.

Optional subtype metadata comes only from observed, type-qualified PTRs pointing
at the selected instance. It is never inferred from subtype syntax in a request.
The r4 Java profile retains this known metadata, including subtype-only zero TTL,
until whole-service goodbye, SRV expiry or scope retirement; it does not have an
independent subtype-expiry purge. The result retains the latest observed DNS-case
spelling. Whole-service zero-TTL SRV/TXT/address/base-PTR observations retire the
cached instance. The pinned decoder's same-type cache-flush behavior replaces
the old address RRset once and retains all new records in the packet; its
deferred one-second flush behavior remains an upstream TODO and is not claimed.

Address order is IPv4 then IPv6. Only link-local IPv6 receives the actual IPv6
interface index; site-local and global addresses remain unscoped, matching
`InetAddressUtils.withScopeId`. A missing usable IPv6 index cannot create a
link-local result. The native snapshot carries canonical, IPv4 and IPv6 indices,
LUID and generation. Network identity is looked up in the existing
WindowsNetworks snapshot. An unmapped link-only adapter yields null Network and
its actual interface index. Explicit Network selection never falls back to
another adapter or invents an `index + 100` Network for an unmapped adapter.

Listener ownership is by identity across implemented browse/resolve operations.
The entry is removed before terminal executor submission, and stale completion
cannot target a reused listener. Failure and cancellation return the original
request object. Default callbacks run through the existing internal handler;
an explicit executor selects the application callback thread. Discovery's
existing 200-request admission check includes active resolutions. The r4 resolve
path itself has no Java discovery-quota check; actual resource exhaustion is a
backend error.

No response is not a terminal timeout. Queries back off up to an hour while the
operation remains cancelable. Progress, expiry and a new SRV target schedule
missing questions promptly; each question still obeys the one-second minimum
interval. A temporarily unsent nonblocking query retries after one second,
rather than inheriting the previous hour-long backoff.

## Native ownership and resource limits

One worker owns every operation on its driver. Each native numeric request owns
its Winsock reference, selected sockets and two change-notification registrations.
Watchers are registered before the first adapter snapshot. Callbacks hold a
registry lease and increment an atomic generation only: no JNI, Java reference,
socket lock or application callback runs on a Windows notification thread.
Loss and return of the same numeric adapter index still invalidates observations.

Cancellation closes sockets and deregisters both watchers from the worker,
outside callback/registry locks. Only confirmed closure/cancellation releases the
request. A failure emits a terminal failure, drops all app/request/listener
references, and leaves only the bounded driver/native owner for cleanup retry.
The retry cannot emit a later success. This follows the locking restrictions in
[CancelMibChangeNotify2](https://learn.microsoft.com/en-us/windows/win32/api/netioapi/nf-netioapi-cancelmibchangenotify2).

| Resource | Bound / behavior |
| --- | --- |
| Java started workers / native requests | 200 each; retiring native owners still count |
| Native sockets | 512 process-wide |
| Selected interfaces per request | 64, at most 128 family sockets |
| Enumerated adapters / adapter buffer | 1,024 / 4 MiB |
| UDP packet / record count | 65,535 bytes / 8,192 records |
| Current target addresses / observed subtypes | 64 / 64 per selected scope |
| Retained question timestamps | SRV/TXT plus A/AAAA for the current target |
| Native poll | At most 50 ms; no unbounded native packet queue |

These are explicit backend resource limits, not Android API quotas or evidence
that the host supports any particular number of real services.

The private native diagnostic reports live requests, sockets, watchers, entered
callbacks, and request-owned completed-poll/successful-send indicators. The
Windows gate waits for actual sockets, both watchers, a sent query and completed
poll before normal cancellation. Its absent-Network check waits for a real
reconciliation/poll cycle before asserting no fallback socket or send. Registry
allocation alone cannot pass either check.

## Measured evidence

| Evidence | Result | Interpretation |
| --- | --- | --- |
| Maintained manager/resolver contracts | 149 checks passed | Production Java state/manager, explicit native-boundary seams |
| Independent IPv4 UDP/TCP loopback peer | 17 checks passed; 1 query/1 response | Real host sockets, full response 1,395 bytes; not Windows transport |
| Independent IPv6 UDP/TCP loopback peer | 17 checks passed; 1 query/1 response | Real host sockets, full response 1,407 bytes; not Windows transport |
| Existing browse regression | 63 manager + 182 transport checks passed | Existing browse contracts retained; explicit host seams |
| Fresh application compilation | 296 sources / 780 classes, 3.992 s | Genuine framework/core compile inputs; app/build source selection |
| Fresh full services DEX | Passed | Includes current Windows native-initialization and progress gate |
| x64 / ARM64 native cross-compilation | Passed / passed | ARM64 reports the existing nativehelper unsupported sysv_abi attribute warning; no Windows execution |
| Official SDK / JNI / import audit | 7 / 8 / 15 verified | Selected static scope described above |
| Root-owned stock Android reference | 27 checks passed in 5.010 s | Actual API36 IPv4 system service, shell/system-context identity |
| Actual Windows production execution | Pending | Required before merging a Windows-support claim |

The host fixture uses genuine framework/app classes and a narrow explicit host
native initialization boundary. Its framework/core execution on a host JDK is
not native ART or Windows qualification. All host-native included sources and
the produced library hash are retained by the fixture tool.

The Android reference uses a separately encoded valid raw multicast peer and
real TCP listener on Network 102, `wlan0`, index 16, owned address `10.0.2.16`.
Its DNS response is 1,419 bytes; raw TXT is 1,201 bytes; the retained attribute
policy size is 1,183 bytes, within Android's 1,300-byte policy. These three sizes
must not be conflated. Opaque/null/empty/duplicate values, Unicode label,
leading-dot type, actual Network/address/port, real TCP connection, cancellation,
listener reuse and peer cleanup passed. The observed image is the July-2025
API36 profile `BE2A.250530.026.D1/13818094`, not an assertion that every later r4
detail was executed on that image.

The immutable Android capsule contains three javac classfiles and seven DEX
class definitions (four D8 lambda classes), all under `gesher.nsd`. It replaces
no platform providers. Source SHA-256 is
`c46292fa66a38156e65b944f756870a957f314a33aac9d483fc600c417e522f8`;
manifest SHA-256 is
`c83b6accc5bfb74d5bd3ae4f707a0190200a3c96a5bc76c9ad3eef33251e545a`;
executed JAR SHA-256 is
`f474913e7dfe64298ca20d29ae856f6ef6f6a215c1f06a71b0aa6a5af02e9f40`.
Root verified the device hash before execution. The process logs UID 2000 and
system-context bootstrap: **this is not ordinary-APK permission evidence**.

The old-source control compiles the exact PR147-tree NsdManager independently
and records its defining class origin. It observes the former unconditional
INTERNAL_ERROR outcome, copied failure input and silent cancellation no-op.
Independent QA also preserves the earlier resolver classfiles as negative
controls for missing expiration, delayed new-target questions and discovery
quota accounting. Earlier attempts/receipts remain separate from final inputs.

## Remaining qualification and follow-on work

`tests/windows/run-nsd-resolve-contract.ps1` is wired into the services track and
has passed syntax validation. It loads the genuine `android_runtime` library,
starts an independent valid multicast/TCP peer, exercises actual manager/native
resolution, checks binary results and real endpoint reachability, then checks
cancellation, listener reuse, absent Network and complete native retirement.
IPv6 runs only on a real usable IPv6 adapter and is explicitly unqualified when
none is available. The fixture's native INTERNET guard check is process-internal
evidence, not ordinary APK identity evidence.

Actual port-5353 coexistence and packet-interface attribution, ordinary APK
permission behavior, and real interface loss/return on an owned isolated adapter
remain Windows merge gates. The normal gate does not disrupt a user's network to
simulate that transition. No canceled CI was restarted and no Windows workflow
was manually dispatched. Cross-compilation, host sockets and Android reference
results do not close these gates.

Registration requires its own genuine binary-preserving advertiser, probing,
conflict/rename, announcement/goodbye and lifetime work. Service-info subscriptions
also remain separate. No hardware/network capability, admission or success is
fabricated to close those items.

Reproduction commands and fixture roles are in
[`tests/nsd/README.md`](../../tests/nsd/README.md). The author validation bundle
records source/command/output hashes, the exact local/remote base mapping, final
commit/tree and independent QA handoff; compiled artifacts are not committed.
