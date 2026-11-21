# NSD reference and Windows peer fixtures

These fixtures use unique owned service types. They do not enumerate unrelated service
types or log other devices' names/addresses.

## Android discovery reference

`NsdAndroidReference.java` uses the official API36 NSD SDK at its callsites. Its bootstrap
obtains a system context through app_process, so its 19-check Android run is discovery,
value and executor evidence under shell identity, not ordinary APK permission evidence.
The source and payload hash are described in
[`nsd-windows-browse-2026-11-18.md`](../../docs/research/nsd-windows-browse-2026-11-18.md).

## Ordinary Android APK permission reference

`permissions/src/gesher/qa/nsdpermissions/ControlReceiver.java` and the three
`permissions/*/AndroidManifest.xml` files define the permission matrix measured by independent
QA. The source records outcomes; it does not hardcode which permission should succeed.
The manifests declare no permissions, INTERNET only, or INTERNET plus ACCESS_NETWORK_STATE.
All use min/target SDK36. Their repository license is Apache-2.0. The three manifests now
disable debugging: installation and ordered broadcasts need neither a debugger nor
`run-as`. Earlier Android execution receipts describe the older manifest/APK hashes;
they are not evidence that newly rebuilt APKs have executed. Separate reference fixtures
that retrieve private result files with `run-as` retain their own required configuration.

Build the three owned permission APKs using explicit SDK/JDK paths:

```sh
python3 tools/build_nsd_permission_reference.py \
  --java-home /path/to/jdk \
  --android-jar /path/to/android-36/android.jar \
  --build-tools /path/to/build-tools/36.1 \
  --output /path/to/isolated/nsd-reference-build
```

The builder uses only the SDK jar, generates a local fixture signing key, verifies APK
signatures/package/target/permissions and the built APK's non-debuggable flag, and records
input/tool/APK/badging hashes in manifest.json.
It writes setup, execution and cleanup command arrays and **does not execute ADB**.
Setup pushes read-only APKs and checks device hashes. Execution covers acquisition and
both discovery overloads for each APK. Cleanup names only the three owned packages and
their uploaded files. Run arrays serially through the chosen isolated-device coordinator,
verifying setup hashes before installation.

Broadcasts return JSON frames prefixed `NSD_APK_PERMISSION_REFERENCE:`. Verify actual
SDK/target, manifest/self-permission state, ordinary app UID and matching worker Binder
UID before interpreting observations. Distinguish synchronous exceptions from callbacks
and timeouts. This is an Android reference: its explicit `candidateWindowsExecution:false`
output must never be counted as Windows permission qualification.

The pinned SDK/JDK/D8 can reproduce the frozen DEX. APK hashes may differ because this
builder generates a fresh test key. Record the exact APKs executed; rebuilt artifacts
must not replace an earlier execution receipt. Do not commit APKs, keys or build outputs.

## Required Windows production peer

`windows_peer.py` constructs mDNS packets independently using Python's standard library
and opens a real listening TCP socket. The Windows gate supplies an owned random type;
two further types derived from that nonce are reserved for goodbye cases. IPv4 and IPv6
use the selected real interface. TXT contains embedded NUL, and Unicode/dot/backslash
instance names occupy one DNS label.

The multicast socket keeps its wildcard bind so it can receive genuine multicast.
`peer_scope.py` enables the Windows `IP_IFLIST`/`IPV6_IFLIST` receive restriction and
adds only the selected family's interface index before binding. After bind, it verifies
that the restriction is enabled and that the returned interface list contains exactly
that index. This post-bind check covers Windows' deferred socket-option validation.
Unsupported options, failed binds, disabled/mismatched readbacks and other setup errors
fail the peer before readiness and close both owned sockets; there is no broad fallback.
Outgoing multicast selection and membership remain scoped to that interface too. The
PowerShell gate selects IPv4/IPv6 indexes by the same adapter alias; the Android `Network`
identity continues to use the adapter's IPv4 index. See Microsoft's
[IPv4 options](https://learn.microsoft.com/en-us/windows/win32/winsock/ipproto-ip-socket-options),
[IPv6 options](https://learn.microsoft.com/en-us/windows/win32/winsock/ipproto-ipv6-socket-options)
and [deferred validation rule](https://learn.microsoft.com/en-us/windows/win32/api/winsock/nf-winsock-setsockopt).

`python3 tools/test_nsd_peer_scope.py` exercises the real guard and peer setup at an
explicit socket boundary, including native DWORD encoding, setup/readback ordering,
rejected configuration, cleanup and permission-manifest/build-flag validation. Both
the framework workflow and the Windows browse gate run it. These host checks do not
claim that Windows filtering executed. The actual peer gate requires the real provider's
successful readback. Live multicast peers require Windows. The restriction is evaluated
only when multicast starts, so importing packet encoders remains safe on other hosts.

The services-track gate drives the peer and the actual manager/native transport. Short
PTR2/SRV5 lifetimes test refresh and silent expiry. Fresh PTR120/SRV180 observations,
verified native positive lifetime and a three-second goodbye deadline distinguish
goodbye loss from natural expiry. A base-only goodbye during subtype discovery must
reach the native callback and cause Java loss before that deadline.

The Windows gate has not run for this candidate. Host fixtures, packet parsing, Android
references and cross-compilation cannot replace it.

## Binary one-shot resolution

`resolver_peer.py` independently encodes valid DNS-SD packets and owns real UDP/TCP
sockets. Its TXT fixture distinguishes key-only/null from empty arrays, includes 205
opaque bytes, and carries enough padding for a response above 512 bytes while the
retained attributes remain within Android's 1300-byte policy. Random owned types
isolate live peer traffic. No unrelated service enumeration is performed.

The multicast resolver uses the same verified receive-interface guard as the browse
peer. `--fixtures` packet generation and explicit `--loopback` transport bypass only
the multicast setup, and remain available on Linux without Windows socket options.

Run the production Java contracts and actual IPv4/IPv6 loopback peers with genuine
framework/core/app class inputs and a fresh output directory:

```sh
python3 tools/test_nsd_resolve_contracts.py \
  --java-home /path/to/jdk \
  --framework-classes /path/to/framework/classes \
  --core-classes /path/to/core/classes \
  --app-classes /path/to/fresh/app/classes \
  --output /path/to/new/resolver-validation
```

The manifest binds production/fixture sources, host-native transitive include files,
the host library, packet fixtures and executed commands. Host fixtures explicitly
inject the transport boundary. Their real loopback sockets do not execute Windows
Winsock, interface watchers or native retirement. `NsdResolveOldSourceControl.java`
is a deliberate negative control for the extracted PR147-tree manager; it must run
with those isolated old classfiles first on the classpath and checks its defining
origin. It is not a success gate for current production code.

Audit imported bytes and the selected SDK/native/provider descriptors using:

```sh
python3 tools/check_nsd_resolve_provenance.py \
  --java-home /path/to/jdk \
  --android-jar /path/to/android-36/android.jar \
  --framework-dex /path/to/distribution/framework.jar \
  --application-classes /path/to/fresh/app/classes \
  --core-classes /path/to/genuine/core/classes \
  --upstream-sources /path/to/pinned/raw/mdns/sources \
  --output /path/to/provenance.json
```

This audits seven selected public declarations, eight JNI descriptors, fifteen
imports, and selected Android/Gesher member references. It explicitly distinguishes
actual framework DEX definitions from fresh application classes and inherited core
compile declarations. It is not a full boot-provider census or Windows execution.

`NsdResolveAndroidReference.java` uses stock SDK36 calls, actual selected Network and
interface membership, a raw valid multicast publisher and real TCP endpoint. It logs
UID/system-context bootstrap and platform provider ownership, emits exceptions to
stderr, and closes its peer sockets/workers in finally. Build immutable inputs with:

```sh
python3 tools/build_nsd_resolve_reference.py \
  --java-home /path/to/jdk \
  --android-jar /path/to/android-36/android.jar \
  --d8-jar /path/to/d8.jar \
  --output /path/to/new/android-resolver-reference
```

The builder writes frozen source, class/DEX/JAR hashes and setup/run arrays. It never
runs ADB. A single designated device coordinator verifies uploaded hashes and executes
the arrays serially. Preserve the three javac classfiles/seven DEX definition distinction
and keep every executed capsule immutable. The measured 27-check stock IPv4 reference
is shell/system-context evidence, not ordinary APK permissions or Windows transport.

The services track invokes `run-nsd-resolve-contract.ps1`, which loads genuine native
runtime initialization before the Java fixtures and then drives the actual Windows
resolver against the independent multicast peer. Cancellation waits for real sockets,
both interface watchers, a completed poll and a sent query. An absent-Network check
waits for a completed real reconciliation/poll before asserting no fallback. The peer
gets a bounded graceful quit window to close sockets and write its receipt. Real
interface loss/return and ordinary APK identity remain separately required Windows
qualification; the script does not change a user's adapters.

The exact measured scope, source provenance and pending gates are documented in
[`nsd-windows-resolve-2026-11-18.md`](../../docs/research/nsd-windows-resolve-2026-11-18.md).

## Native Windows import/link regression

The normal native build also builds `nsd_windows_link_test`. It links the complete
NSD transport with the real guest POSIX `bind`, `sendto`, `setsockopt` and `select`
entries. The fixture retains those genuine function references without calling a
socket, JNI or network operation. It supplies no replacement implementation.

```sh
cmake --build /path/to/configured/windows-build --target nsd_windows_link_test
```

Winsock import-library members define a callable thunk as well as an `__imp_*`
slot. Directly importing the same names can collide with guest Linux signatures
or select the wrong implementation. The transport therefore resolves exact
WinSDK-typed pointers from the system `ws2_32.dll`, keeps the module alive for the
process lifetime, and reports actual lookup failures through its existing
initialization-error path before Winsock startup or interface enrollment.

The former direct-call source is a meaningful negative control: linking it with
this fixture and the real POSIX archive fails on those duplicate symbols. The
repaired resolver and registration profiles passed complete x64/ARM64 PE links
using separately hashed cached POSIX/import-library inputs; their PE import tables
contain none of those four direct imports. This is a focused link profile, not a
fresh complete current distribution build, Windows native loading, packet delivery
or lifecycle qualification. The earlier translation-unit checks did not detect
the actual published full-link failure, whose evidence remains retained.
