# Proxy configuration and SOCKS5 contracts

Run with Python 3, Git and a JDK 11 or newer:

```sh
python3 tests/host-net-proxy/run.py --java-home "$JAVA_HOME"
```

The default framework source is
`third_party/aosp/frameworks/base/core/java/android/net/Proxy.java`. For a separate
checkout, add `--proxy-source /path/to/Proxy.java`. The source must be the pinned
[Android 16r4 Proxy.java](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/java/android/net/Proxy.java),
either original or with the repository's `0786-proxy-socks-bypass.patch` applied.
The runner checks the original source's SHA-256, applies the production patch to
a temporary copy and compiles that actual class. It neither downloads sources
nor changes the supplied checkout.

The host suite compiles the production `ProxySetting` and `WindowsProxy` classes.
It verifies SOCKS aliases and normalization; invalid hosts, credentials and
ports; bracketed IPv6; persistence and package/launch precedence; child process
environment serialization; and the real `installInProcess()` entry point in
fresh JVMs with each supported `GESHER_PROXY` spelling. Android service and
value-object dependencies are isolated stubs. Proxy parsing and the framework
property setter are not stubs.

`tests/dex-services/com/gesher/net/ProxySettingsRuntimeTest.java` is shared between
this host runner and the Windows/ART services test DEX. It exercises the actual
framework setter after configuration and again at app-bind time, tests HTTP,
HTTPS and socket selector bypasses and HTTP/PAC/direct transitions, and sends a
real `HttpURLConnection` request through a local SOCKS5 server to a separate
local HTTP origin. The server verifies the greeting, CONNECT command and remote
hostname. Additional requests prove that loopback bypass works with an
unavailable SOCKS server and that an explicit HTTP proxy takes priority. All
wire peers are isolated loopback fixtures; the `.invalid` destination is mapped
only inside the SOCKS fixture. No test routing is installed in production code.

The shared entry point is `com.gesher.net.ProxySettingsRuntimeTest`; it prints
`PROXY_RUNTIME_CONTRACTS_OK` only after all runtime and wire assertions pass.
The host suite additionally prints `PROXY_HOST_CONTRACTS_OK`.

Host Java success alone does not establish Windows/ART success. In particular,
Android's HTTPS bypass selection and ordinary `new Socket()` behavior differ
from OpenJDK. This feature covers clients that consult `ProxySelector` or use an
explicit SOCKS proxy; the fixture does not claim that Android transparently
tunnels ordinary sockets. The host PAC dependency is a selector marker that
throws if asked to evaluate a URL: actual WinHTTP PAC evaluation and Internet
Options change notifications require Windows integration checks.
