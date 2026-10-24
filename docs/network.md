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
- on the internet: DNS, HTTPS GETs (example.com, google.com), TLS 1.3 with SNI
  and hostname verification, badssl.com's expired, wrong-host, self-signed and
  untrusted-root certificates rejected;
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

- No `CONNECTIVITY_ACTION` broadcast; `PendingIntent` network requests are
  accepted and never fire.
- Windows' proxy settings are not carried over (`getDefaultProxy()` is null).
- Binding a socket or a process to a Network is accepted but Windows picks the
  route; no per-network DNS.
- VPNs are recognized by adapter type and name, a heuristic.
- No Wi-Fi details (`TransportInfo`, SSID, signal strength).
- BoringSSL without assembly (slower AES-GCM and bignum than a phone's).
- Captive portals are not detected.

## Status and next steps (handoff checkpoint, 2026-10-25)

**Branch** `wip/net` (worktree C:/work/gesher-net), tip cfb2d2e (f8bdfc8 + a merge
of origin/phase4/integration at 5561615). **No commit is CI-verified green yet.**

Done (all written, compiled in CI, run-time untested on Windows): BoringSSL,
NativeBN, Conscrypt natives and Java, OkHttp, BouncyCastle (in core-libart),
the Windows trust store sync, ConnectivityService + android.net classes +
callbacks, the INTERNET / ACCESS_NETWORK_STATE gates, tests (tracks/56-net.ps1,
tests/dex-net, tests/apps/netapp and nonetapp), installer shipping, ADR 0037.

**CI so far**
- net.yml (compile-only, push-triggered on wip/net): runs 40128266078 (b3cca77)
  and 40129516302 (56b94a7) green.
- ci 40114623277 (ecf8df9): Conscrypt compile errors (fixed b3cca77).
- ci 40129516691 (56b94a7): duplicate `inet_ntop` / `inet_pton` at the dalvikvm
  link (fixed 44a2e95).
- ci 40133972018 (44a2e95): build, framework, boot-image, package and e2e green.
  Both test jobs ended after ~48 min with "The hosted runner lost communication
  with the server"; no logs survived. Cause (most likely): the connectivity
  change step ran `New-NetIPAddress 192.0.2.77` on the runner's DHCP adapter,
  and a static IPv4 address turns the adapter's DHCP off, cutting the runner
  off. Fixed in f8bdfc8: the test adds 2001:db8::77 (IPv6, SkipAsSource,
  ActiveStore) and turns DHCPv4 back on should it find it off.
- ci **40138810234** (cfb2d2e): running.

**Next, exactly**
1. Read run 40138810234's test logs (`gh run view 40138810234 -R
   gesher-runtime/gesher --log-failed`): the `PASS|FAIL|INFO  net:` lines of
   tracks/56-net.ps1 and any other track's FAIL (core-libart now carries
   Conscrypt/OkHttp/BC; Digests.install defers to Conscrypt; `connectivity` is
   now registered, so ActivityThread's `cm.getDefaultProxy()` path runs;
   gesher-base refuses inet sockets when GESHER_INET=0).
2. Fix, push, rerun ci (`gh workflow run ci -R gesher-runtime/gesher --ref
   wip/net`; watch with `gh run watch <id> --exit-status --interval 120`, no
   polling loops: the API rate limit is shared by all agents).
3. When both test jobs and the package job are green: add a Phase 5
   networking checklist to docs/roadmap.md (sockets/DNS, BoringSSL+NativeBN,
   Conscrypt+OkHttp+BC, Windows trust store, ConnectivityManager + callbacks,
   permissions) quoting the run id and PASS lines; update this section.
4. Optional: Currency (org.billthefarmer.currency, already in
   tests/e2e/apps.json) now fetches its rates over HTTPS; docs/e2e.md could say
   so once its log shows it. No new F-Droid pin was added.

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
