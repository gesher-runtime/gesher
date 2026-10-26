# Networking

Roadmap Phase 5, networking part: Android apps use the network on Windows as
on a phone. Design: [ADR 0037](decisions/0037-net-networking.md). Branch:
`wip/net`.

```
 app ──► java.net / javax.net.ssl / HttpURLConnection / ConnectivityManager
          │            │                    │                    │
          │       Conscrypt (TLS,       OkHttp            connectivity service
          │       JCA, X.509)           (com.android.okhttp)  (com.gesher.server.
          │            │                                       ConnectivityService)
          │       BoringSSL (Windows)                           │
          ▼            │                                  Windows' networks:
  libcore's Linux natives ──► gesher-base sockets ──► Winsock   GetAdaptersAddresses,
                                                                 Network List Manager,
  system CAs: $ANDROID_ROOT/etc/security/cacerts ◄── Android's   NotifyIpInterfaceChange
              set + Windows' ROOT/CA stores (trust_store.cc)
```

## Sockets and DNS

Java's sockets (`Socket`, `ServerSocket`, `DatagramSocket`, NIO channels and
selectors) and name lookups (`InetAddress`) are libcore's, over its Linux
natives (`Linux.cpp`, `NetworkUtilities`, OpenJDK's `Net.c`, `EPoll.c`), which
call gesher-base's sockets ([ADR 0009](decisions/0009-socket-fds.md)):
Winsock behind Linux's socket API, `getaddrinfo` over Windows' resolver,
`epoll` and `poll` over sockets. Nothing networking-specific was needed in
libcore for them; `tests/dex-net` (`NetTest local`, `NetTest echo`) checks
them: DNS (names, literals, a failed lookup), TCP over IPv4 and IPv6 loopback
with a read timeout and a refused connect, UDP, a non-blocking NIO echo with
a `Selector`, and TCP and UDP echo by a .NET server.

## BoringSSL, NativeBN, Conscrypt

- **BoringSSL** (`external/boringssl`, `cmake/aosp/boringssl.cmake`) is built
  as a Windows library from the project's own `android-sources.cmake`, like
  ICU4C: Win32 threads and locks, `ProcessPrng`. `OPENSSL_NO_ASM` (the x86-64
  Windows assembly is NASM's), `OPENSSL_SMALL` as Soong builds it, and
  `OPENSSL_NO_SOCK`: BoringSSL's socket and connect BIOs and `SSL_set_fd` call
  Winsock's `send`/`recv`/`connect`, whose import-library members also define
  the names gesher-base defines for its fds.
- **NativeBN** (libcore's `java.math.BigInteger` natives) is built over it, no
  longer in `GESHER_LIBCORE_UNPORTED`.
- **Conscrypt's natives** (`libjavacrypto`: `common/src/jni/main`,
  `cmake/aosp/conscrypt.cmake`) are a built-in library of dalvikvm
  (`compat/net/javacrypto.cc`), loaded by `NativeCryptoJni` as on Android.
  `patches/external/conscrypt/0001` takes the POSIX side of Conscrypt's
  `_WIN32` branches (gesher-base gives it POSIX), the platform's
  `FileDescriptor.descriptor` and `AttachCurrentThread(JNIEnv**)`, an
  `asprintf`, registers every native through a System V thunk
  (`NATIVEHELPER_SYSV_JNI_ENTRY`, [ADR 0006](decisions/0006-managed-abi.md))
  and replaces `SSL_set_fd` with `gesher_SSL_set_fd`
  (`compat/net/fd_bio.cc`: a BIO over gesher-base's `recv`/`send`), for the
  file-descriptor socket (the engine socket, Android's default, needs none).
- **Conscrypt's, OkHttp's and BouncyCastle's Java** (`tools/net/net-java.sh`):
  Android.bp's `conscrypt` (repackaged `com.android.org.conscrypt`, with
  `NativeConstants` generated from BoringSSL's headers, `StaticBlocklist`,
  its aconfig flags at release BP4A's values, and a `ConscryptStatsLog`
  stand-in: no statsd), `okhttp` (`com.android.okhttp`, which `java.net.URL`
  uses for http and https) and `bouncycastle` (the BC provider). They are
  compiled with libcore and **dexed into core-libart.jar**: the boot class
  path keeps its jars (every launcher and test spells it out), and they are
  in the primary boot image. They stay out of the coremod the framework
  compiles against (it uses Conscrypt's platform API stubs, as on Android).
- **JCA providers**: libcore's `security.properties` as on Android
  (AndroidOpenSSL, CertPathProvider, BC, AndroidOpenSSL's JSSE);
  gesher-app's `Digests.install` steps aside when Conscrypt is there.

## Trust anchors from Windows

Conscrypt's `TrustedCertificateStore` and the framework's network security
config (`SystemCertificateSource`) read the system CAs from
`$ANDROID_ROOT/etc/security/cacerts`: PEM files named `<subject hash>.<n>`.
`compat/net/trust_store.cc` writes that directory when libjavacrypto loads
(every TLS client loads it before it looks a certificate up), from:

- Android's own CA set, Conscrypt's `apex/ca-certificates/files`, shipped as
  `cacerts\` next to dalvikvm.exe (`tools/net/collect.sh`, the installer);
- what Windows trusts for TLS servers: the ROOT and AuthRoot stores of the
  user and the machine (group policy and enterprise roots included) and the
  self-signed certificates of the CA stores, less those whose enhanced key
  usage property leaves out server authentication;
- less every certificate in Windows' Disallowed stores.

So a root an administrator, a company or a content filter (NetFree, TLS
inspection) installed in Windows is trusted by Android apps with no setup.
A manifest (`.gesher-cacerts`) makes the check cheap when nothing changed; a
named mutex serializes writers; `GESHER_WINDOWS_CERTS=0` leaves Windows'
stores out; `GESHER_TRACE_CACERTS=1` reports what was written. User CAs
(`$ANDROID_DATA/misc/keychain`) are Android's, unchanged.

Hostname verification (OkHttp's `OkHostnameVerifier`, Conscrypt's endpoint
identification), SNI and TLS 1.3 are Conscrypt's and BoringSSL's.

## Connectivity

`com.gesher.server.ConnectivityService` is `connectivity` in the system server
(and in gesher-app's single process). It describes Windows' networks
(`com.gesher.net.WindowsNetworks`, natives in `compat/net/windows_networks.cc`,
the built-in library `libgesher_net.so`): every adapter that is up and reaches
beyond its link (a gateway, or Windows' route to the internet), with its kind
(Wi-Fi, Ethernet, cellular, VPN by type or by name), addresses and prefixes,
gateways, DNS servers and suffix, MTU and link speeds, and from the Network
List Manager whether it reaches the internet, is metered (fixed or variable
cost), roaming or congested. The active network is the one Windows routes
8.8.8.8 (or 2001:4860:4860::8888) through, a VPN's when one is up.
`NotifyIpInterfaceChange`, `NotifyUnicastIpAddressChange` and
`NotifyRouteChange2` wake its watcher (and every 15 s it looks again, for the
Network List Manager's verdicts); each new snapshot goes to its listeners. Its
binder interface is `com.gesher.net.IConnectivity` (written as AIDL would
generate it).

`android.net.ConnectivityManager` (gesher-app.jar; framework.jar compiles
against the Connectivity module's API and lacks it) is its client, with the
module's public API: `getActiveNetwork`, `getAllNetworks`,
`getNetworkCapabilities`, `getLinkProperties`, `getActiveNetworkInfo` /
`getNetworkInfo`, `isActiveNetworkMetered`, `registerNetworkCallback`,
`registerDefaultNetworkCallback`, `registerBestMatchingNetworkCallback`,
`requestNetwork` (with `onUnavailable` after its timeout),
`unregisterNetworkCallback`, `bindProcessToNetwork`... A Network's netId is
100 + the Windows interface index. Callbacks are called on their Handler (or
a ConnectivityThread) as on Android: `onAvailable`, `onCapabilitiesChanged`,
`onLinkPropertiesChanged`, `onBlockedStatusChanged` for a new network, the
changed ones after, `onLost`. `Network`, `NetworkCapabilities`,
`LinkProperties`, `LinkAddress`, `RouteInfo`, `IpPrefix`, `ProxyInfo`,
`NetworkInfo`, `NetworkRequest`, `InetAddresses` and `TransportInfo` are
written with the module's public API and Parcelable.

Capabilities: the transport; NOT_RESTRICTED, TRUSTED, NOT_SUSPENDED,
FOREGROUND, NOT_VCN_MANAGED; NOT_VPN unless a VPN; INTERNET with a gateway
(or the internet route, or a VPN); VALIDATED when the Network List Manager
reaches the internet through it; NOT_METERED, NOT_ROAMING, NOT_CONGESTED from
its cost; bandwidths from the link speeds; a VPN's underlying networks.

**Permissions.** `ConnectivityService` refuses callers whose package does not
request ACCESS_NETWORK_STATE (SecurityException, as ConnectivityService).
INTERNET is the inet group on Android: without it gesher-base refuses
AF_INET/AF_INET6 sockets (EACCES) and name lookups (`EAI_SYSTEM` with
EACCES, which libcore reports as "Permission denied (missing INTERNET
permission?)"); numeric addresses, AF_UNIX and loopback socketpairs are not
affected (`<gesher/inet.h>`). The system server sets `GESHER_INET=0` for app
processes of packages without it; gesher-app applies it in its own process
(single process and `--system-server`).

## Proxy and broadcasts (web3 track, ADR 0058)

- **Windows' proxy settings** (the user's Internet Options as WinHTTP reads them:
  `WinHttpGetIEProxyConfigForCurrentUser`) are Android's default proxy
  (`com.gesher.net.WindowsProxy`, natives in `compat/net/windows_proxy.cc`,
  libgesher_proxy.so): a proxy server becomes `ProxyInfo.buildDirectProxy(host, port,
  exclusions)` (per-scheme WinINet strings: http's server; `<local>` has no Android
  equivalent), a PAC URL a PAC proxy. Automatic detection (WPAD, on by default in Windows)
  is not followed: every request would look for a script most networks do not have. Every process applies it with
  `Proxy.setHttpProxyConfiguration` (http(s).proxyHost/Port/nonProxyHosts, as
  ActivityThread at bind), so `Proxy.getDefaultHost()`, HttpURLConnection and OkHttp use
  it, and follows Windows' changes (RegNotifyChangeKeyValue on the settings' key). Under a
  PAC script, the framework's PacProxySelector asks `com.android.net.IProxyService`, which
  every process serves itself with `WinHttpGetProxyForUrl` (Windows fetches and runs the
  script, in process). `ConnectivityManager.getDefaultProxy()` and `getProxyForNetwork`
  return it.
- **CONNECTIVITY_ACTION**: the connectivity service keeps it sticky (made at the first
  sticky query, as BatteryService's battery broadcast) and sends it when the default
  network (its netId or kind) changes: EXTRA_NETWORK_INFO, EXTRA_NETWORK_TYPE,
  EXTRA_NO_CONNECTIVITY.
- **PendingIntent requests and listens** (`requestNetwork`, `registerNetworkCallback` with
  a PendingIntent): the app's ConnectivityManager sends the PendingIntent with
  EXTRA_NETWORK and EXTRA_NETWORK_REQUEST when a matching network is available;
  `releaseNetworkRequest`/`unregisterNetworkCallback(PendingIntent)` end them.
- Tests: tests/windows/tracks/54-web3.ps1 (web3app.apk's NetActivity on the Gesher device,
  with Windows' settings set to the test's proxy, then to a PAC script, and restored).

## Tests

`tests/windows/tracks/56-net.ps1`, with `tests/dex-net` (`net-tests.dex`,
plain Java on the core boot class path) and `tests/apps/netapp`,
`tests/apps/nonetapp` through gesher-app:

- sockets, DNS, NIO, HTTP through OkHttp to a local server, BigInteger
  (NativeBN), the JCA providers, digests, HMAC, AES-GCM, RSA and EC
  signatures, PBKDF2 (BouncyCastle), SecureRandom, the default SSLContext;
- the system CA directory (Android's and Windows' certificates);
- TCP and UDP echo by .NET servers;
- a test root made by the script: an HTTPS server (SChannel) on 127.0.0.1 with
  a certificate for localhost from it is rejected, trusted once the root is in
  the machine's ROOT store, rejected with `GESHER_WINDOWS_CERTS=0`, and
  rejected again once the root is removed;
- netapp.apk's HTTPS check (`NetApp https`) goes to an HTTPS server of the script with a
  certificate from the test root, kept in the machine's ROOT store while the apps run
  (`$GESHER_NETAPP_HTTPS`; web3 track: ci 40240210807 had failed when example.com did not
  answer);
- on the internet: DNS, HTTPS GETs (example.com, google.com), TLS 1.3 with SNI
  and hostname verification. The four rejection cases (expired, wrong host,
  self-signed and unknown root) now use local TLS endpoints in 56-net with
  separately generated certificates and only the intended test CA trusted.
  Manual NetTest internet runs retain the public badssl defaults when
  GESHER_TLS_REJECTION_URLS is unset;
- netapp.apk in one process: ConnectivityManager's state, a default and a
  request callback, HTTPS through HttpsURLConnection with the network
  security config, cleartext refused (target SDK 36);
- nonetapp.apk: lookups and sockets refused, ConnectivityManager's
  SecurityException;
- netapp.apk in an app process of a system server: ConnectivityManager over
  binder; an address added to and removed from the adapter of Windows'
  default route reaches its default network callback.

`.github/workflows/net.yml` compiles the native pieces and the boot jars on
pushes to `wip/net`, for quick feedback; ci.yml builds and tests everything.

## Known gaps

- (web3 track, ADR 0058: `CONNECTIVITY_ACTION` and `PendingIntent` requests, and Windows'
  proxy settings, are done; see "Proxy and broadcasts" below.) PendingIntent requests are
  kept by the app's process and end with it.
- Pending CI (web3 continuation): socket/process bindings select Windows'
  outgoing interface (both sides of dual-stack sockets); per-network DNS
  uses `DnsQueryEx.InterfaceIndex` without falling back to another adapter.
  `Network`'s socket factory and HTTP connection factory use these bindings.
- VPNs are recognized by adapter type and name, a heuristic.
- No Wi-Fi details (`TransportInfo`, SSID, signal strength).
- BoringSSL without assembly (slower AES-GCM and bignum than a phone's).
- Pending CI (web3 continuation): the Network List Manager's
  `NA_InternetConnectivityV4/V6` `WEBHIJACK` flags map to
  `NET_CAPABILITY_CAPTIVE_PORTAL`, suppressing `VALIDATED` for that adapter.
  A runner without a captive portal cannot verify a real portal transition.

## Status and next steps (2026-10-25)

- Running combined ci 40345505558 at
  `21300715416f076ac32219b23d7ec0db97d9ca15`, after the fixture
  lifetime/ABI-selection fixes and coordinator reliability patch. Watch output:
  `C:/work/gesher-ci-review/web3-40345505558-watch.log`. Previous
  40338521188 completed with exactly three web3 fixture failures per runner;
  its runtime successes are bounded by the observations below. No milestone
  is closed pending complete green Windows/package/e2e verification.

- ci 40338521188 at 010551b completed: package and both e2e jobs pass
  (all nine apps, including required TIC-80; zero unexpected failures).
  Both Windows test jobs report exactly three failures, all in web3 fixtures:
  Activity completion destroyed the WebView before the external UIA client's
  final web-button search; a later launcher environment could not change the
  existing device server's native-library path, so its ARM case loaded x86_64.
  All binary messaging assertions, device geolocation, native x86_64 device
  execution and genuine x86_64/arm64 single-process networking pass.
- The next run keeps the accessibility Activity alive until external UIA
  invokes the page button and DOM title becomes `pressed 2` (after the
  Android provider's first click). Device ARM selection uses a boolean
  Intent extra, and device x86_64 identity is asserted too. Local Java
  compilation and PowerShell parsing pass. No existing action/name/heading
  assertions or time limits were relaxed.
- Imported coordinator reliability patch 50f16a9 (source-only): bounded
  geocoder completion without reading an incomplete Result, and atomic
  SHA-pinned APK caching/official-mirror fallback with four own-byte
  transport fixtures. All four local checks pass; full CI remains pending.
  Additional shared files: ci.yml, tests/e2e/{apks.ps1,test-apks.ps1,
  apps.json,run-e2e.ps1}, and tests/windows/tracks/58-devices.ps1.

- Running final combined ci 40338521188 at
  `010551b16f54c6eb9040e77c18f2082e1abf1a2a`. Watch output is
  `C:/work/gesher-ci-review/web3-40338521188-watch.log`. Superseded
  40333695614 cancellation was requested only after this dispatch succeeded.
  No new runtime milestone is closed until both complete Windows test jobs,
  package and both required e2e jobs pass.

- Final coverage dispatch is ready after merging integration ac50d41
  (verified aot2 code plus central net4 ownership documentation). It includes
  device geolocation and both native ABIs in both process modes. Diagnostic
  ci 40333695614 passed all builds; media preparation spent about 21 minutes
  before completing successfully. With final coverage now ready, that run
  will be superseded after the replacement dispatch is confirmed. Its runtime
  result is incomplete, not a runtime failure or an assumed known flake.

- Pending next final CI: build `web3_network_arm64.so` with the NDK arm64
  compiler and run its same seven-API regression in both single-process and
  device modes, asserting the compiled ABI in the native success message.
  Current ci 40333695614 contains x86_64 execution only. Remaining resolver,
  socket semantics and hardware/network completeness belong to the explicit
  net4 follow-up (ADR 0070), coordinated separately from these seven exports.

- Running combined ci 40333695614 at 455e9fcb99807d502879024fdcf689fea8586f66
  after the absolute NDK path and verified aot2 integration. Read the NDK
  runtime PASS line in both process modes on each runner before closing the
  seven exported networking API items.

- ci 40325234847 at 2de1e15: package and both e2e jobs pass. Both test
  runners pass the new selected-interface DNS, kernel binding, server-observed
  source address, process binding/unbinding and bound HTTP assertions in both
  modes. Four tests fail per runner: accessibility fixture/runtime semantics
  and a relative `System.load` path prevented NDK execution. The next run
  uses an absolute path. Raw resolver/NDK runtime verification remains pending.
- ci 40324592794 at 5e90b96 compiled the NDK test ELF, but the native
  build failed because raw-DNS used try/catch with exceptions disabled.
  Corrected to a native CreateThread worker; local syntax now checks
  `-fno-exceptions` too. Runtime checks remain pending a new full CI.
- ci 40318144247 at 722f86b verifies the local negative-certificate fixture
  tests on both Windows runners: `PASS net: DNS and HTTPS on the internet,
  local expired/wrong-host/self-signed/untrusted certificates rejected`.
  Both test jobs fail only the two web3 accessibility checks; package and
  both e2e jobs pass. This run predates the network-binding implementation.
- Pending CI: the seven assigned NDK networking exports now use the same
  interface/socket/DNS implementation through existing System V thunks.
  `android_res_nquery/nsend` return a socketpair descriptor signalled by a
  bounded native worker; `nresult` returns the DNS wire packet and rcode,
  consumes the descriptor, and rejects an undersized buffer. Cancellation
  shuts down the active query and closes the result descriptor. Queries
  use the chosen adapter's configured DNS servers over bound UDP, retry
  up to three servers (one with NO_RETRY), and fall back to TCP for
  truncated responses. Raw queries bypass the cache. A new NDK-built ELF
  library exercises all seven functions, real source addresses, A-record
  results, raw packet IDs, NXDOMAIN, cancellation and invalid arguments in
  single-process and device modes. C++ `-Werror` syntax checks pass; full
  NDK runtime remains for CI (ELF compilation passed in 40324592794 and
  40325234847). Additional shared edits are one
  source entry in `compat/posix/posix.cmake` and one test-library compile
  line in `.github/workflows/native-libs.yml`.
- Continuation (2026-10-26), pending CI: interface-specific socket/process
  binding, DNS, `Network.getSocketFactory/openConnection`, and NLM captive
  state are implemented. Local checks compile the changed POSIX files with
  `-Werror`, compile `Network/WindowsNetworks/NetActivity` against the saved
  Android classes, and parse the PowerShell tests. A small native executable
  using the actual changed Winsock code passes IPv4/dual-stack kernel
  option readback, unbinding, process inheritance, invalid-network binding
  preservation, and A/AAAA DNS through interface 8. The native JNI file has
  a syntax check with JDK headers/a thunk-macro stand-in; CI must validate
  the real SysV JNI build and Android runtime. New 54-web3 checks require
  the HTTP server to observe the selected adapter's source address and
  reject DNS on a missing network. No adapter configuration was changed.
  The seven native networking exports assigned from native3's API inventory
  are implemented, including pollable raw DNS results and cancellation; their
  runtime checks are pending the absolute-path correction above.
- Additional file ownership approved by the coordinator: minimal edits in
  `compat/posix/{src/socket.cc,src/winsock.cc,src/netdb.cc,src/socket_internal.h,
  include/gesher/inet.h}` and related bionic wrappers; the original web3
  domain also needs the additive `oleaut32` link in `cmake/aosp/conscrypt.cmake`.
- Continuation (2026-10-26), pending CI: replaced the four badssl rejection
  endpoints in 56-net with local certificate fixtures. Real internet
  DNS/HTTPS/TLS 1.3/SNI assertions remain. Rejection must now have a
  CertificateException in its cause chain or SSLPeerUnverifiedException;
  an unrelated SSL handshake failure or timeout is not accepted. Local
  Java compilation, PowerShell parsing and fixture key/date/SAN/custom-root
  chain checks pass. No Windows ROOT store was changed locally. Runtime
  Conscrypt validation still requires a new full CI run.


### ADR 0009 socket gap audit (2026-10-26)

The binding work does not close these pre-existing gaps. Duplicated socket
descriptors still store independent `O_NONBLOCK` metadata rather than sharing
an open-file description. `recvmsg` drops ancillary data and output flags;
`sendmsg` has no ancillary transport. `SCM_RIGHTS`, `IP_PKTINFO`, full datagram
length reporting with `MSG_TRUNC`, and urgent-data readiness (`POLLPRI`) remain
unsupported. `AF_NETLINK`, `AF_PACKET` and `AF_VSOCK` return `EAFNOSUPPORT`.
Legacy host/service/protocol resolver functions have no Gesher implementation
(the separate thread-local `h_errno` does exist). The existing bionic exports
forward directly to the Windows imports. A small probe using Gesher headers
confirmed the ABI defect: Windows' 16-bit hostent fields are read as Linux's
32-bit fields (`h_addrtype=262146,h_length=0` for native `2,4`). A neutral
structure translation and correct resolver error state are required.
These socket/resolver items remain assigned to web3 or an explicit successor.

The coordinator assigns epoll edge/nesting/poll-on-epoll/multiwaiter behavior
and pipe polling to native3, and cross-process InputChannel/socket descriptor
transport to binder2. Those tracks must coordinate any descriptor-sharing
changes. Receive-timeout stream behavior and Windows WSAPoll platform limits
also remain documented in ADR 0009. Real Wi-Fi, VPN, captive-portal transitions
and disconnected-adapter selection are not verified by the hosted runner.

**Branch** `wip/net` (worktree C:/work/gesher-net). **CI-verified green**: ci
40141520320 (66d71de) and ci 40143168361 (47a4aeb, merged with
origin/phase4/integration incl. webview, stack, devsvc): build, every reusable
workflow, both test jobs (windows-2022, windows-2025), package and e2e. Every
56-net.ps1 check PASSes on both runners; the roadmap's Phase 5 networking box
cites these runs. Final: ci 40191501348 at 05aaeb5 (merged with origin/phase4/integration
6111f0d, devices) green, 39 `PASS  net:` lines and no FAIL on each test job;
its package job failed once on the .apk open command (the exception text is
cut off by the step's 5-line tail) and passed on rerun (attempt 2).

Done: BoringSSL, NativeBN, Conscrypt natives and Java, OkHttp, BouncyCastle
(in core-libart), the Windows trust store sync, ConnectivityService +
android.net classes + callbacks, the INTERNET / ACCESS_NETWORK_STATE gates,
the network security config in single-process mode, tests (tracks/56-net.ps1,
tests/dex-net, tests/apps/netapp and nonetapp), installer shipping, ADR 0037.

**CI history**
- net.yml (compile-only, push-triggered on wip/net): runs 40128266078 (b3cca77)
  and 40129516302 (56b94a7) green.
- ci 40114623277 (ecf8df9): Conscrypt compile errors (fixed b3cca77).
- ci 40129516691 (56b94a7): duplicate `inet_ntop` / `inet_pton` at the dalvikvm
  link (fixed 44a2e95).
- ci 40133972018 (44a2e95): both test jobs lost their runner after ~48 min: the
  connectivity-change step put a static IPv4 address on the runner's DHCP
  adapter. Fixed in f8bdfc8 (an IPv6 address, SkipAsSource, ActiveStore).
- ci 40138810234 (cfb2d2e): cancelled, superseded.
- ci 40139254526 (430d285): all green but the test jobs. FAILs: UDP
  (`setsockopt failed: ENOPROTOOPT`: PlainDatagramSocketImpl sets
  IP_MULTICAST_ALL=0), netapp single-process cleartext allowed (no
  NetworkSecurityConfigProvider outside handleBindApplication), nativeapp's
  tcp/getaddrinfo (EACCES: its manifest lacked INTERNET), 50-services' list
  of stand-in services (now has `connectivity`). All fixed in 66d71de.
- ci 40141520320 (66d71de) and 40143168361 (47a4aeb): all green.

**Next (optional)**
- Currency (org.billthefarmer.currency, in tests/e2e/apps.json) can now fetch
  its rates over HTTPS; docs/e2e.md could say so once its log shows it.
- Gaps (see "Known gaps" above): captive portals, VPN/metered detail, proxies.

**Pitfalls**
- Never change the runner's IPv4 configuration in a test (no static IPv4
  address on its adapter): the runner loses GitHub and the job's logs.
- Win32-side code must not reference ws2_32 functions named like gesher-base's
  (send, recv, connect, inet_ntop, inet_pton...): duplicate symbols at the
  dalvikvm link (ADR 0009). BoringSSL is built with OPENSSL_NO_SOCK for this.
- `<wincrypt.h>` defines X509_NAME etc.; trust_store.cc #undefs them.
- Conscrypt natives need System V thunks (patch 0001,
  NATIVEHELPER_SYSV_JNI_ENTRY) and are compiled with GESHER, not ANDROID.
- Local checks: app/java compiles against the framework-classes artifact
  used as a jar (its class dir has case-colliding names on Windows), JDK 21.
- Untried at run time: the cacerts sync (drive-less $ANDROID_ROOT as a Win32
  path), NLM COM in the system server, ConnectivityService.caller() (pid to
  package), IConnectivity over gesher's binder, New-NetIPAddress on the runner,
  badssl.com reachability.

**Edits outside the domain (all additive)**
- CMakeLists.txt: includes boringssl.cmake and conscrypt.cmake.
- cmake/aosp/libcore_native.cmake: NativeBN no longer unported; libcore_javacore
  links boringssl_crypto.
- aosp.lock: external/boringssl added; conscrypt, bouncycastle, okhttp sparse
  sets widened.
- tools/build-boot-jars.sh: runs tools/net/net-java.sh, compiles the net list,
  dexes it into core-libart.jar (not into the coremod).
- tests/build-test-apps.sh: builds tests/dex-net.
- .github/workflows/ci.yml: build job runs tools/net/collect.sh (cacerts,
  NET-NOTICES.txt).
- .github/workflows/framework.yml: builds and collects netapp.apk, nonetapp.apk.
- tools/package/gesher.iss: ships cacerts/ and licenses/NET-NOTICES.txt.
- app/java/com/gesher/app/Digests.java: install() steps aside with Conscrypt.
- app/java/com/gesher/app/Main.java: applies the INTERNET permission (2 calls).
- app/java/com/gesher/server/Services.java: one line registering ConnectivityService.
- app/java/com/gesher/server/SystemServer.java: GESHER_INET=0 for packages
  without INTERNET (3 lines).
- compat/posix/src/socket.cc, netdb.cc, include/gesher/inet.h: the INTERNET gate.
- Own new workflow .github/workflows/net.yml (push to wip/net only).
- compat/posix/src/socket.cc: IP_MULTICAST_ALL accepted (set and get).
- app/java/android/app/GesherAppDriver.java: NetworkSecurityConfigProvider.install
  before the Application (single-process mode, as handleBindApplication).
- tests/apps/nativeapp/AndroidManifest.xml: requests INTERNET.
- tests/windows/tracks/50-services.ps1: `connectivity` in the expected list.
