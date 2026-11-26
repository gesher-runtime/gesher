# Windows network qualification for Gesher Free

[The edition plan](../store-editions-plan.md) requires outbound public Internet
access for Free APKs that have Android INTERNET permission. The host, loopback
and the LAN are outside that permission's Free profile.

## Current source and proposed boundary

- The installed launcher removes `GESHER_SANDBOX`. The opt-in development
  layout uses the per-package `AppSandbox` path. The current mapping grants
  both `internetClient` and `privateNetworkClientServer` to packages with
  Android INTERNET. See [sandbox milestones](../sandbox.md).
- The first Windows candidate for Free is an AppContainer token with
  `internetClient` for INTERNET, without the private or server network
  capabilities. The launch should require production isolation, with
  kernel-verified Binder identity and the same network policy for child
  processes and any broker that connects on behalf of an APK.
- `compat/webview/webview_host.cc` creates a WebView2 environment through
  Microsoft's loader. Gesher applies Android permission checks and provides
  request handling and proxy settings. Qualifying the Free profile requires
  examining the browser and network process tokens and actual connections.
  Java or URL filtering in isolation cannot establish that the network path
  uses the same boundary as the APK.

Microsoft describes [AppContainer isolation](https://learn.microsoft.com/en-us/windows/win32/secauthz/appcontainer-isolation),
[the outbound Internet capability](https://learn.microsoft.com/en-us/windows/win32/secauthz/createprocessinsandbox#capabilities-string-default-empty),
and [loopback diagnostics](https://learn.microsoft.com/en-us/windows/security/operating-system-security/network-security/windows-firewall/troubleshooting-uwp-firewall).

## Manual probe

Build `appcontainer_network_probe` explicitly from a configured repository
build. This target is excluded from the default build and has no package or CI
network dependency.

```powershell
cmake --build <build-dir> --target appcontainer_network_probe
& <build-dir>/appcontainer_network_probe.exe
```

An independent Windows build with LLVM MinGW can use the same source inputs:

```powershell
& <llvm-mingw-clang++> -std=c++20 -D_WIN32_WINNT=0x0a00 -Wall -Wextra -Werror `
  -O2 -static -municode -mno-ms-bitfields -fno-exceptions -Icompat/framework `
  tests/sandbox/appcontainer-network-probe.cc compat/framework/services_zygote.cc `
  compat/framework/app_container.cc -ladvapi32 -lole32 -luserenv -lws2_32 `
  -o appcontainer_network_probe.exe
```

The probe opens temporary TCP listeners on IPv4 and IPv6 loopback. It uses
`SpawnInAppContainer` to start real children under its own disposable
AppContainer profile. The child checks its token before opening a socket.
Each row compares ordinary Winsock connections before and after the
AppContainer attempts. Code 0 means connected, 10013 means access denied, and
10060 means the four-second connection window elapsed. A restricted row
requires the ordinary controls to connect and both contained attempts to be
denied or time out. A public row additionally requires the internet-only
child to connect. This establishes observed reachability for each supplied
endpoint on that Windows configuration.

Add reachable lab endpoints as `public <numeric-IP> <port>` or
`restricted <numeric-IP> <port>` triples. Supply an independent public endpoint
and a listener on a separate LAN host for a useful broader comparison. For the
same-PC case, use a listener bound to an interface's own address. Check that
host firewall or routing permits the ordinary controls before interpreting
results. Run without an external harness that prevents child initialization.

## Observations on Windows build 26100

The standalone LLVM MinGW build linked the production AppContainer and
zygote sources from the planned branch. An ordinary Windows run on
2026-11-26 returned exit 0:

| TCP endpoint | Ordinary controls | No network capability | Internet capability only |
| --- | --- | --- | --- |
| IPv4 loopback listener | Connected | Timed out | Timed out |
| IPv6 loopback listener | Connected | Timed out | Timed out |
| Same PC's IPv4 LAN-interface listener | Connected | Timed out | Timed out |
| Public IPv4 on TCP 443 | Connected | Access denied | Connected |

The restricted tool sandbox allowed the parent controls and caused its
contained children to stop during DLL initialization (`0xc0000142`). The
ordinary Windows run above reached the child socket checks.

The Gesher development launcher can use this token configuration by setting
`GESHER_SANDBOX=appcontainer` and `GESHER_SANDBOX_NETWORK=internet-only` in the
system server's environment. It grants `internetClient` to APK processes
requesting Android INTERNET and omits `privateNetworkClientServer`. The normal
experimental profile remains available by omitting the network variable.

## WebView2 environment and browser token

A standalone Win32 probe linked Gesher's `webview2_loader.cc` and the actual
`SpawnInAppContainer` implementation. On build 26100, Evergreen 158.0.4370.45
created a WebView2 environment and controller successfully in both the ordinary
and AppContainer runs. The ordinary browser had an ordinary Windows token.
In the contained run, querying the PID returned by `BrowserProcessId` showed
the same AppContainer SID as the probe process and only the `internetClient`
capability SID (`S-1-15-3-1`). This is evidence for environment and browser
startup. During the public navigation, `GetProcessInfos` reported a browser,
two utility processes, two renderer processes and a GPU process. All six
tokens matched the probe's AppContainer SID and carried only
`internetClient`. This snapshot identifies utility processes by WebView2 kind.

The same standalone WebView2 probe then navigated to `https://example.com/`
and temporary HTTP listeners. Normal and contained WebView2 both completed
the public HTTPS navigation. The ordinary browser completed loopback and
same-PC IPv4 interface navigation, with a request reaching each listener.
The contained browser reported a connection failure for loopback and no
completion within 15 seconds for the same-PC interface. Neither contained
navigation reached its listener. The timeout establishes non-delivery for
that run and leaves the browser's ultimate completion status unresolved.

## APK launch in the current experimental mode

On the same Windows host, we extracted the packaged runtime from
[CI run 45668397258](https://github.com/gesher-runtime/gesher/actions/runs/45668397258)
at `156abd94b` and verified the artifact SHA-256 against the GitHub metadata.
The source for `AppSandbox`, native spawn, the launcher wrapper and the
existing `AppContainerPoc.ps1` script is unchanged between the CI revision
and the PR base `82bb674a0`. Each run used an isolated, fresh Gesher profile.

| Mode | `lifecycle.apk` result |
| --- | --- |
| Ordinary runtime, headless | Install and launch returned 0. The Activity reached `onResume`. Windows reported that the app process had no AppContainer token |
| Experimental AppContainer, headless | Install returned 0. The app process started, then ART reported access denied reading the boot class path JARs. Launch returned 1 before `onResume` |
| Experimental AppContainer repeated from a fresh Windows temporary directory | The same startup failure occurred |

This run used Gesher's packaged binaries and a repository APK fixture through
the development test launcher. It establishes a blocker before testing APK
network traffic or WebView in this baseline. Keep this separate from the
native probe, whose children reached the Winsock tests. For an earlier
unmerged packaged-isolation candidate, see [PR #427](https://github.com/gesher-runtime/gesher/pull/427).

### ART boot and Looper fixes

An AppContainer could open an ACL-authorized boot JAR through `CreateFileW`.
The storage path check then returned access denied because a normalized
DOS-volume lookup on that handle failed. The normalized NT-volume lookup
succeeded. The launcher now resolves the media root in the same NT namespace
before spawn. The contained process freezes that root with its storage
identity and applies the existing scoped-storage rules to normalized handle
paths under that root. [Windows final path contract](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfinalpathnamebyhandlew)

A native child with a real AppContainer token exercised the production
handle check. It could read its granted runtime fixture next to the media
root. Scoped storage continued to deny a control file inside the root,
including an open through an outside alias. The patched `lifecycle.apk`
and network APK both passed boot class loading. They next hit a Looper wake
descriptor failure because the contained runtime could not open `NUL`.

When that error occurs in an AppContainer, the descriptor allocator can
duplicate the broker's inherited startup `NUL` handle after verifying its
kernel object name. CRT reservation keeps the same type of file handle.
This also covers the reservation used for descriptor duplication. After
clearing the wake descriptor stage, another startup failure surfaced in
Android's `Build` initialization when it received an empty system property.

The property broker now grants a contained app read access to the device's
real property area. Two published snapshots let a reader check consistency
without sharing a Windows named mutex with the writer. A native AppContainer
control read the broker's property and a live update, while both writable
file and mapping access and reads of the persistent store were denied.

The corrected descriptor reservation allowed Binder to duplicate the app's
shared-memory descriptor and Looper to use the logical wake descriptor.
With the rebuilt runtime and Java jar, `lifecycle.apk` reached `onResume`
and exited normally with an observed low-integrity AppContainer token.
Conscrypt then exposed a separate pipe startup problem in the network APK.
Guest-local Linux pipes now use the documented AppContainer-local Windows
namespace and keep the normal endpoint direction and descriptor flags.
The ordinary Windows pipe contract passed 274 checks with the new native
objects. [Windows named-pipe contract](https://learn.microsoft.com/en-us/windows/win32/api/namedpipeapi/nf-namedpipeapi-createnamedpipew)

### Android network fixture

Build the repository's [network APK](../../tests/apps/sandboxnetwork) against
an Android SDK. Keep the HTTP(S) test endpoints active for the entire run.
Use distinguishable response content, a reachable public endpoint, and
reachable local listeners authorized for the test.

```powershell
tests/windows/sandbox/Build-SandboxNetworkApk.ps1 -AndroidSdk $sdk -OutputDir $apkOut `
  -PublicUrl $public -PublicMarker $publicMarker -HostUrl $hostUrl -HostMarker $hostMarker `
  -LoopbackUrl $loopback -LanUrl $lanPeer -LanMarker $lanMarker
tests/windows/sandbox/Test-SandboxNetworkApk.ps1 -RuntimeDir $runtime -Apk $apk `
  -ScratchDir $scratch -WithLoopback -WithLanPeer
```

The harness runs `HttpURLConnection` and Android WebView against those
endpoints in three fresh profiles. The ordinary runs must receive the
specified content before and after the internet-only AppContainer run.
The contained run must receive the public content, reject the configured
host and LAN requests, and log both its OS token and server-selected capability
list. Leave `-LanUrl` and `-WithLanPeer` out when there is no distinct LAN
test peer. A same-PC interface is a separate test from a LAN peer.

On the original packaged CI runtime, after copying the packaged fonts into
the fixture's system root, the APK ordinary baseline received all three
response markers in both `HttpURLConnection` and WebView2 for public HTTPS,
loopback and the same-PC IPv4 interface. This established reachable controls
for those endpoints.

The final run used the same fixture in three scratch profiles on Windows
build 26100, with ordinary runs before and after the contained run. Both
ordinary runs received all six expected content markers. In the contained run
Windows reported a low-integrity AppContainer, and Gesher selected
`gesherRuntimeFiles`, `gesherBinderClient` and `internetClient`. The
fixture completed and the three-run gate returned 0.

| Endpoint | Android HTTP in AppContainer | WebView2 in AppContainer |
| --- | --- | --- |
| Public HTTPS | Verified content | Verified content |
| Same PC's IPv4 interface | Connection timed out | Navigation timed out |
| IPv4 loopback | Connection timed out | Main-frame connection failure |

The controlled HTTP listener received requests during both ordinary runs and
none during the contained run. The runtime used the exact rebuilt native
objects and Java jar in the digest-verified CI layout. SHA-256 prefixes are
`78650666` for `dalvikvm.exe`, `754a34f1` for the app jar and `3da0f00c`
for the signed fixture. Run the separate LAN-peer check with a reachable,
authorized listener when one is available.

## Remaining release evidence

1. Exercise a distinct LAN peer, IPv6 public endpoints, DNS and UDP,
   network profiles, VPN and proxy configurations, and inbound listening.
   Verify how Windows classifies local public address space and local routes
   before relying on `internetClient` alone.
2. Repeat qualification from the intended packaged entry points. Verify the
   token and effective capabilities for native children, app-private files,
   cross-package isolation and trusted Binder identity in the same build.
3. Extend the WebView2 checks beyond top-level navigation to subresources,
   redirects, WebSockets and an app-supplied proxy. Verify Windows services
   that make requests on an APK's behalf.
4. Keep the current shipping behavior during qualification. Wire the Free
   mapping through the trusted edition decision when both channels are ready
   for production containment. A failed isolation setup must stop that APK
   launch.

The Android DownloadManager contract reaches `DownloadProvider` and
`DownloadTransfer` inside the system server. PackageTransport currently opens
each transfer using the system server's own network and default Windows proxy,
including follow-up requests from redirects. `AssetLinks` also performs HTTP
verification in the system server on behalf of installed packages. For
production Free confinement, those paths need a constrained transport tied to
the authenticated package while preserving existing operation and certificate
handling. A passing direct TCP/WebView test cannot establish that broker
property.
