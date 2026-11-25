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
  user CAs:   $ANDROID_DATA/misc/user/0/cacerts-added ◄── roots installed on this PC
```

## Sockets and DNS

### Android HttpEngine

`android.net.http` is supplied by gesher-app.jar, including `HttpEngine.Builder`,
`UrlRequest`, response/header values, upload providers and the platform option
builders. Its asynchronous adapter uses the existing `HttpURLConnection` transport
and therefore the same proxy selector, network binding, TLS verification and trust
anchors as other Java HTTP clients. Bidirectional HTTP/2 streams use the AOSP
OkHttp framed transport already in core-libart.jar, with independent reads and
writes, flush, END_STREAM and gRPC trailers. HTTPS negotiates `h2` using ALPN,
verifies the peer hostname, and tunnels through the selected HTTP CONNECT proxy.
HTTP/2 response metadata retains `:status`, which Cronet's gRPC clients consume
directly. Bounded read-ahead delivers final gRPC trailers without requiring an
extra application read, and terminal callbacks wait for pending data callbacks.
Unresolved addresses from Android's proxy selector are resolved for the proxy;
origin DNS remains at that proxy. This is needed by recent Cronet clients such
as Play Store 54.1.11, which selects the platform HttpEngine provider.

Requests support HTTP(S), explicit redirect callbacks, direct-buffer reads,
fixed-length/chunked uploads, upload rewinds on preserved-method redirects,
cancellation and one terminal callback. Application callbacks are serialized on
the supplied executor; callback failures become `CallbackException`. Headers and
URL chains are immutable snapshots. Redirects across origins remove explicit
Authorization, Cookie and Host headers. HTTP error status codes remain responses.
Canceled asynchronous uploads retain their provider until its pending read or
rewind reports sink completion. Uploads reuse one direct buffer of at most 32 KiB.
Bidirectional reads may be queued from `onStreamReady`, and direct read-only
buffers are accepted for writes. Gzip headers are decoded after dispatching the
response-header callback so a duplex peer can wait for that callback's upload.

Bidirectional streams reuse eligible HTTP/2 sessions owned by their `HttpEngine`.
The route includes the exact origin, selected proxy, supplied Network object and
TLS socket-factory identity. A warm session can multiplex streams; one stream's
cancellation or peer reset does not close its healthy peers. GOAWAY stops new
admission while accepted streams drain; a stream that selected a session but had
not yet sent HEADERS when GOAWAY arrived is admitted once on a fresh session.
Completed streams, including those with a request body, end without RST_STREAM.
Actual peer stream limits, including zero, are respected. Concurrent cold
connections can still open separately.

Completed transports release their leases only after admitted I/O retires.
Terminal callbacks, including `onCanceled`, are delivered only after the
stream's I/O workers finish.
Engine shutdown rejects active requests, then closes all owned connections.
The pool retains at most five idle sessions, expires them after 60 seconds, and
uses a single process-wide parked expiry worker. Stream I/O and physical socket
writes have separate 30-second deadlines; a broken physical connection still
fails its dependent streams. Each stream keeps its existing bounded 32 KiB
copies, while AOSP OkHttp owns framing and flow-control storage. As in Cronet,
a stream's receive window (6 MiB) is smaller than the connection's (16 MiB), so
one stream whose application stopped reading cannot stall its siblings. The qualified
loopback corpus covers 16 concurrent warm streams and measured connection/TLS
handshake reduction, not general high-concurrency throughput. See the
[session ownership and measurement report](research/http2-sessions-2026-11-22.md).

The transport does not implement QUIC, Brotli or engine-private disk caches.
Protocol/DNS/cache tuning is advisory and does not install process-global settings;
additional public-key pins fail explicitly rather than pretending to provide those
guarantees. Received-byte counts currently measure response body bytes, excluding
wire headers. Traffic
statistics setters are retained as request metadata, like Gesher's TrafficStats
adapter; they do not account per-request Windows traffic. The native Android
Cronet module is not shipped.

`tools/test_http_engine.py --java-home <JDK>` tests the production adapter over
real local HTTP sockets in CI. Adding `--okhttp-source <AOSP external/okhttp>`
compiles the pinned Android 16 framing and Okio sources for host HTTP/2 wire
tests, including gzip-header ordering, direct-buffer callback contracts, shared
session cancellation, GOAWAY, TLS/proxy/Network route isolation and real idle
descriptor retirement. The helper applies this revision's receive-credit and
window patches to the pinned provider; bounded in-memory contracts verify discard,
reset and padding accounting and the advertised window geometry, and are also
mandatory on Windows. The complete framework CI requires that pinned checkout.
`tests/net/HttpEngineDeviceSmoke.java` and
`Http2DeviceSmoke.java` also run on Gesher ART in Windows track 94, with an
independent local HTTP/2 wire peer. `GESHER_HTTP_ENGINE_TRACE=1` logs numeric
HTTP/gRPC status codes and failure types/code locations, never request URLs,
headers, credentials or bodies. Any installed update must replace both
gesher-app.jar and its matching boot image extension; source-only changes are not
enough for an already-installed runtime. The measured Play 54.1.11 startup and
catalog verification is recorded in
[the validation note](research/httpengine-play-store-2026-11-16.md).

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
A manifest (`etc/security/.gesher-cacerts`, next to the directory rather than
in it, so that cacerts holds only certificates, as Conscrypt expects) makes
the check cheap when nothing changed; a
named mutex serializes writers; `GESHER_WINDOWS_CERTS=0` leaves Windows'
stores out; `GESHER_TRACE_CACERTS=1` reports what was written.

### User-added CAs: roots installed on this PC

On a phone, a CA its user or a device admin installs is a *user-added* CA:
KeyChain writes it to `/data/misc/user/<user>/cacerts-added`, and
`TrustedCertificateStore` (the `AndroidCAStore` KeyStore) lists it with a
`user:` alias. Some apps trust only their own roots plus these: Chrome (its
Chrome Root Store plus `X509Util.getUserAddedRoots`, the `user:` aliases),
apps whose network security config has `<certificates src="user"/>`, and,
by the default config, apps targeting API 23 or lower
(`UserCertificateSource`, the same directory). Behind a content filter that
re-signs TLS (NetFree) such apps fail with an unknown authority unless the
filter's root is a user-added CA, as it is on a phone set up for it.

So the same sync also writes `$ANDROID_DATA/misc/user/0/cacerts-added` (user 0:
Gesher's device has one user) with the roots that were **installed on this
PC**, by its user, an administrator, group policy, an enterprise or a program
such as a content filter:

- the roots of Windows' ROOT stores (the user's and the machine's, with their
  group policy and enterprise parts) whose enhanced key usage allows server
  authentication,
- less those in Microsoft's root program: the SHA-1 thumbprints of Windows'
  AuthRoot certificate trust lists, the one automatic root update keeps in
  `HKLM\SOFTWARE\Microsoft\SystemCertificates\AuthRoot\AutoUpdate`
  (`EncodedCtl`) and the one built into crypt32.dll (its `AUTHROOTSTL`
  resource), taken together, and the roots automatic root update marked as
  taken from such a list (their `CERT_AUTH_ROOT_SHA256_HASH_PROP_ID`
  property);
- less Android's own CA set (`cacerts\`) and Windows' Disallowed stores.

Microsoft's list is what tells a distributed root from an installed one,
wherever Windows keeps it: the roots automatic root update adds on demand go
to the machine's AuthRoot store (part of the ROOT store) and are all listed,
and so are the ones Windows ships in the machine's ROOT store (Microsoft Root
Certificate Authority 2010/2011, the timestamping roots). The store a root is
in does not tell (an administrator's import and Microsoft's own roots are both
in `LocalMachine\ROOT\.Default`), and Windows' shipped roots carry no mark of
their own. Two 1997
Microsoft roots that are in neither list (`Copyright (c) 1997 Microsoft
Corp.`, `Microsoft Authenticode(tm) Root Authority`) are limited to other
purposes by their enhanced key usage and are left out of both stores. The CA
stores' self-signed certificates (such as `Root Agency` and the TPM roots
Windows keeps there) are not taken: Windows does not trust a root there either.
If neither list can be read, no root can be told apart and none is user-added:
a sync then removes Gesher's user CAs (the safe direction; they come back at
the next sync that can read a list).
The lists are not signature-checked: only an administrator can change the
registry copy, and crypt32.dll is a system file.

Such a root stays a system CA too (the system directory is as above), so apps
that trust only system CAs keep working, and Android's network security
config and `AndroidCAStore` now also see it as a user-added CA: both a
`system:` and a `user:` alias. (Android's own KeyChain installs no user copy of
a system CA; here the system copy is Gesher's import from Windows, and the
`user:` alias is what tells apps it was installed locally.) Apps see what they
would on a phone with that CA installed by the user:
`TrustedCertificateStore.isUserAddedCertificate` and
`X509TrustManagerExtensions.isUserAddedCertificate` return true for chains
that end at it, Chrome's `X509Util.getUserAddedRoots` lists it, and apps or
browsers that warn that the network may be monitored when a user CA is
installed will warn here.

The directory is laid out as `TrustedCertificateStore` keeps it: DER files
`<subject hash>.<n>`, a new one after the last index of its hash, a removed
one truncated to an empty tombstone while a later index exists and deleted,
with the tombstones before it, otherwise. A root removed from Windows is
removed at the next sync. Two records name the entries Gesher wrote and their
SHA-256, and either one is enough: the manifest `misc/user/0/.gesher-user-cacerts`
(next to the directory), written before the entries it adds, and an NTFS named
stream `:gesher-owner` on each entry (the entry's name and SHA-256), written
on the temporary file so the rename moves data and mark together. A lost or unwritable manifest, or a crash
between writes, so leaves no entry behind that outlives its root (nothing in
Gesher could remove it: there is no KeyChain or Settings). If the manifest
cannot be written, no entry is written (a volume without named streams would
have no other record). An entry Gesher did not write, or whose contents
changed since, is never touched. That includes a file someone put there with
the same certificate, and a copy of Gesher's entry at another name (copies
carry the stream, but it names the original). It does not include a file
with Gesher's certificate put at the very name Gesher's records give: that
one cannot be told from Gesher's own and goes with its root. A new entry
never replaces a file: if one appears at its name after the scan, the entry
takes the next index. Removing an entry opens it shared, so a reader that
shares writing and deletion (as Gesher's own file layer does) does not keep
it from going; one that still cannot be removed (a reader that shares only
reading, such as .NET's `File.OpenRead`) stays recorded, goes at a later
sync, and does not hold up new entries; while it cannot be read, it still
counts as present. The manifest is read leniently (CRLF, case, malformed lines skip only themselves). As in Conscrypt's
`findCert`, the indexes of a hash end at the first one that is not a file: a
directory there leaves the certificate out (the sync reports a failure)
rather than writing it past the directory, where it would be unreachable.
`cacerts-removed` (system CAs the user disabled) is Android's, unchanged.
`GESHER_TRACE_CACERTS=1` lists the user-added roots.

App processes look there as on Android: `ActivityThread.main` calls
`TrustedCertificateStore.setDefaultUserDirectory(Environment.getUserConfigDirectory(userId))`;
`GesherAppProcess`, which runs `main`'s steps itself, and gesher-app's
in-process driver do the same (`GesherAppProcess.useUserCaCertificates`).
Without it Conscrypt looked in `$ANDROID_DATA/misc/keychain` while the network
security config's `UserCertificateSource` looked in `misc/user/0`. In the
experimental AppContainer sandbox (docs/sandbox.md) the system server opens
`misc/user/0` to the runtime files capability for reading. KeyChain, its
Settings screens and `DevicePolicyManager.installCaCert` are not implemented:
apps cannot add or remove user CAs.

### Native TLS clients

Native code that does its own TLS reads the system CAs by their Android
names: Flutter's dart:io opendir()s `/apex/com.android.conscrypt/cacerts`
(newer SDKs) or fstatat()s `/system/etc/security/cacerts`, and BoringSSL's
by-dir lookup (`SSL_CTX_load_verify_locations`) fopen()s
`<dir>/<subject hash>.<n>`. A drive-less `/system/...` would be the current
drive's root, so the bionic shim maps exactly those two names, and the names
under them, for apps' native code (`compat/bionic/system_cacerts.h`, wrapping
stat/lstat/fstatat/statfs/statvfs/access/faccessat/open/openat/fopen/freopen/
opendir/realpath and their 64/_2 aliases in the shim's table) to
`$ANDROID_ROOT/etc/security/cacerts` when that directory exists, else to
`cacerts\` next to the executable (Android's own set, without Windows' roots
until a Java TLS client has synced the directory). Only a whole path component
matches (`.../cacertsX` is not mapped); a name under it with `..`, a
backslash, a colon, a trailing dot or space or a Win32 device name fails with
ENOENT. tests/native/libc_test.c checks it in the x86-64 native track when
`cacerts\` is next to elf_load_test.exe (`GESHER_TEST_SYSTEM_CACERTS=1`).

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
`NotifyRouteChange2` wake its watcher, and so does the Network List Manager's
`INetworkConnectionCostEvents` (`compat/net/cost_events_win.cc`: a connection's
cost or data plan changed, which moves no interface, address or route, e.g. the
"Metered connection" setting of a network that stays connected). Every 15 s it
also looks again (the fallback where the Network List Manager has no cost event
connection point). Each new snapshot goes to its listeners; a callback then hears
`onCapabilitiesChanged` for NET_CAPABILITY_NOT_METERED, and a polling
`isActiveNetworkMetered` sees the new cost (the watcher owns the shared snapshot while
there are listeners).

Unknown cost does not establish that a network is free of usage limits. A missing
Network List Manager, unmatched adapter, failed cost query, or `UNKNOWN` cost keeps
the snapshot's metering bit set. Only a successful, explicitly `UNRESTRICTED` result
without `FIXED` or `VARIABLE` grants `NET_CAPABILITY_NOT_METERED`. This conservative
classification keeps unmetered-only jobs and downloads waiting when Windows cannot
confirm the cost; it does not invent a tariff or change the network's transport,
reachability, or the user's settings. A later confirmed unrestricted result restores
the capability through the ordinary snapshot update.

`getRestrictBackgroundStatus` is RESTRICT_BACKGROUND_STATUS_DISABLED, as on a device
without Data Saver: Android's ENABLED means the app is blocked from background data on
metered networks, which Gesher does not do, so Windows' "Background data" limit
(`ConnectionCost.BackgroundDataUsageRestricted`) is not reported as it. It should be
reported only together with that enforcement. Its
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
written with the module's public API and Parcelable. `DnsResolver` (Waze's
network stack died without it) resolves A and AAAA records through a network's
interface as `Network.getAllByName` does, on a thread of its own, and calls back
on the executor unless cancelled; a name that does not resolve is NXDOMAIN with
no addresses. `rawQuery` answers A/AAAA questions with a DNS response built from
those addresses (TTL 0) and other types with NOTIMP: Windows' resolver gives no
other records (FrameworkFixesTest, 94-framework-fixes).

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
- **Gesher's own proxy**, over Windows' settings (`com.gesher.net.ProxySetting`): for every
  app or for one package, saved in `<data>/system/proxy.properties` by
  `gesher-app proxy [PACKAGE] host:port|direct|windows` or in the control panel ("Proxy for
  every app" above the app list, and each app's "Proxy" button, where "Same as every app"
  removes the package's own setting) (no argument shows them; `direct`
  is no proxy; `windows` for a package is Windows' settings over every app's setting), or for
  one launch (`gesher-app launch PACKAGE --proxy host:port`). The system server gives an app
  process its setting (the launch's, else the package's, else every app's) as
  `$GESHER_PROXY` when it starts it, with `HTTP_PROXY`/`HTTPS_PROXY`/`NO_PROXY` for HTTP
  clients that read those (Dart's HttpClient: Flutter apps); `WindowsProxy` then takes it
  instead of Windows' settings (an HTTP proxy server, with localhost, 127.0.0.1 and [::1] left out) and does not
  follow Windows' changes. A running app keeps its proxy until its process ends: reopening
  its window or an Activity can reuse that process, so a changed setting needs the app
  closed completely (or the device stopped) and started again, as the control panel says.
  It covers HTTP clients that use the system proxy or those variables. WebView pages are
  not given it: without the app's own `ProxyController` override (androidx.webkit, ADR
  0058) the process's WebView2 environment gets no proxy arguments and uses Windows'
  settings.
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
- the user-added CAs (`tests/native/trust_store_user_test.cc`): synthetic roots in
  temporary directories (a root in neither list is local, one in the root program list,
  Android's set or Disallowed is not; DER `<hash>.<n>` files, tombstones and appends as
  `TrustedCertificateStore` keeps them, entries Gesher did not write left alone, the
  manifest), then the production sync against the machine's real stores in temporary
  roots: every user-added root is in Windows' ROOT store, outside Microsoft's root
  program and still a system CA, and `GESHER_WINDOWS_CERTS=0` removes them;
- TCP and UDP echo by .NET servers;
- a test root made by the script: an HTTPS server (SChannel) on 127.0.0.1 with
  a certificate for localhost from it is rejected, trusted once the root is in
  the machine's ROOT store, rejected with `GESHER_WINDOWS_CERTS=0`, and
  rejected again once the root is removed; while it is there `AndroidCAStore`
  (`NetTest cas`) lists it as `user:` and `system:`, and a root of the
  machine's AuthRoot store (Microsoft's) as `system:` only; once removed, not at all.
  netapp.apk (`NetApp user CA`), in its own process and in an app process of a
  system server, finds it among its `user:` aliases;
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
- Wi-Fi observations and asynchronous scans use [Native Wi-Fi](windows-wifi.md); radio configuration and hotspot control remain unsupported.
- BoringSSL without assembly (slower AES-GCM and bignum than a phone's).
- Pending CI (web3 continuation): the Network List Manager's
  `NA_InternetConnectivityV4/V6` `WEBHIJACK` flags map to
  `NET_CAPABILITY_CAPTIVE_PORTAL`, suppressing `VALIDATED` for that adapter.
  A runner without a captive portal cannot verify a real portal transition.

## Status and next steps (web3)

Completed full ci **40376044459** at
`939ec313d0fe5a2d8410ee700a927fad0538269e`, with AOT3 foundation f5ce6f6
and bounded TLS fixture cleanup. All 14 web3 checks and the new TLS fixture
pass on each runner; root-test, all four negative and app-server cleanup
markers complete on both, with accepted clients joined and certificates
removed/disposed. Real internet/TLS and all certificate assertions still pass.
Windows 2025 has zero failed tests; Windows 2022's sole failure is the AOT2
JIT startup threshold (profile/speed medians 452/404 ms; interpreter 571/612).
Package actual cleanup and both nine-app e2e jobs pass with required
TIC-80/Puzzles and zero unexpected failures. Every other job is success/skipped.
This is not mergeable; the coordinator owns startup diagnosis and a complete
new combined CI remains required. Watch/logs are under
`C:/work/gesher-ci-review/web3-40376044459-*`.

Completed diagnostic ci **40364697280**, exact source
`eee306b12a958286b72da8b30071438a9f1acdf3`; watch output:
`C:/work/gesher-ci-review/web3-40364697280-successor-watch.log`. Both runners
pass all 14 web3 checks; 2022 has zero failed tests and actual package cleanup
and both nine-app e2e jobs pass with required TIC-80/Puzzles. Windows 2025
times out at 60 minutes, with its last PASS at 17:26:36 UTC after the entire
internet/TLS/local-negative-certificate group and no later output before
cancellation at 18:08:55. Its zero-failure summary is absent; no merge gate
is satisfied by that partial run.

The next combined source includes verified AOT3 foundation f5ce6f6. The local
TLS server fixture now owns its accepted sockets, cancels authentication/reads
and joins within fixed limits. Own Windows idle-accept, incomplete-handshake
and idle-read cleanup cases pass. Cleanup markers bracket server stop, store
removal and certificate disposal; the previous log alone does not identify
which call blocked. This follows the documented requirement to
[close accepted connections separately](https://learn.microsoft.com/en-us/dotnet/api/system.net.sockets.tcplistener.stop).
Real DNS/HTTPS/TLS and all negative certificate assertions remain enabled.

Next combined CI is ready with verified native3 foundation f677944f merged,
native geocoder exit 1f1cc96, and exec3's source-only Launcher framing fixes
8eac61c/d2a5f7b. The native3 merge retained the newer native probe exit when
resolving its older geocoder helper. The forwarding regression passes locally
for partial UTF-8 titles, concurrent complete lines, log truncation and final
fragments; exec.yml now checks out its helper and runs that JDK regression.
Window/frame/action assertions remain unchanged. Full CI still must pass.

Full-stage gate awaits a next combined CI with native geocoder probe exit
1f1cc96 and exec3's Launcher line-framing fix. Completed ci 40353139413 at
`6270c9e84e0a8f3486a77824ecb113c563d2871d` again passes all 14 web3 checks
per runner, package and both e2e jobs. Windows 2022 fails the probe's CLR
teardown; Windows 2025 also fails windows3 because a real first-frame line
was split by a concurrent launcher status line. Neither failure is waived.
The native probe exit preserves real-answer/error/30-second/45-second gates
and passes focused own-byte exit-zero/one fixtures. Launcher is owned by
exec3; no concurrent edit is made here. No milestone is closed before both
Windows jobs, package and e2e pass completely.

In completed ci **40345505558 at 2130071**, all 14 web3 checks pass on each
runner, including these real network observations in both process modes:

- Configured selected-interface DNS and refusal to fall back from a missing
  network; actual `IP_UNICAST_IF`/`IPV6_UNICAST_IF` kernel binding and source
  addresses, explicit TCP/UDP binding, process bind/unbind/invalid preservation,
  and a bound HTTP connection whose server observes the selected source.
- Real x86_64 and translated arm64 NDK ELF callers pass all seven assigned
  libandroid networking APIs: selected TCP/process source, getaddrinfo,
  pollable raw DNS query/send/result, packet ID, NXDOMAIN, cancellation,
  closed descriptors, undersized buffers and invalid flags/networks. Each
  ABI runs in single-process and device modes and identifies its compiled ABI.
- Windows system proxy/PAC, direct bypass, sticky CONNECTIVITY_ACTION and
  PendingIntent requests. The four local TLS negative-certificate cases
  pass while live internet DNS/HTTPS/TLS assertions remain enabled.

Package cleanup and both e2e jobs pass, including all nine apps, required
TIC-80, four pinned-byte APK transport fixtures and zero unexpected failures.
The full run still fails: Windows 2025 has zero failed tests; Windows 2022's
only failure is a completed geocoder answer followed by a stuck PowerShell
probe teardown. That 45-second timeout is not a passing probe. Minimal fix
58d7dc6 flushes the awaited result and exits zero, while errors/inner timeouts
exit one and both existing guards remain. Environment.Exit still hung in 40353139413; the native exit fix above now
awaits combined CI.

The raw DNS worker uses the chosen adapter's configured servers and bound
UDP, up to three servers (one with NO_RETRY), cache bypass and TCP fallback
for truncated answers. Like Android's DnsResolver it makes two rounds over the
servers, waiting 5 seconds on the first and its scaled timeout (at least one
second) on the others, so one lost or slow answer from a runner's single
server is retried instead of failing with ETIMEDOUT. Only a completion byte uses its pollable socketpair;
the full answer remains available until nresult consumes/closes the fd.
Cancellation shuts down the active query. Forced truncation/TCP fallback,
cache transitions (including mixed Java/NDK binding) and IPv6-only default
selection remain for net4; current assertions do not prove those cases.

**net4 / ADR 0070** owns the remaining 84 libc networking inventory entries,
legacy resolver ABI/error state, shared socket flags/ancillary/truncation/
urgent data/families, Wi-Fi/VPN/captive transitions, connectivity hidden APIs
and BoringSSL acceleration. See [its handoff](handoff/tracks/net4.md).
**web4 / ADR 0072** owns remaining WebView contracts separately.

Approved expanded/shared edits: POSIX socket.cc, winsock.cc, netdb.cc,
socket_internal.h and public gesher/inet.h; bionic networking wrappers;
network_dns.cc plus its posix.cmake entry; WindowsNetworks/Network/native
routing glue; conscrypt.cmake's oleaut32 link; native-libs.yml's two NDK
fixture builds. Coordinator reliability helpers add ci.yml APK caching,
tests/e2e transport fixtures and the bounded geocoder child process in
58-devices.ps1. No new routing check reconfigures runner adapters, and no
Windows ROOT store was changed locally.

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
These socket/resolver items are assigned to net4 / ADR 0070.

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
