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

## Remaining release evidence

1. Exercise a distinct LAN peer, IPv6 public endpoints, DNS and UDP,
   network profiles, VPN and proxy configurations, and inbound listening.
   Verify how Windows classifies local public address space and local routes
   before relying on `internetClient` alone.
2. Exercise a representative APK from the packaged entry points. Verify the
   token and resolved capability grants for the process and its native
   children. Exercise app-private files, cross-package isolation and trusted
   Binder identity in the same build. The native probe measures networking
   from a Windows executable and establishes only that primitive.
3. Verify WebView2 public navigation and blocked local destinations through
   real browser and network processes, including subresources, redirects,
   WebSockets and an app-supplied proxy. Verify any Windows service that makes
   requests on an APK's behalf. Use controlled endpoints and real OS results.
4. Keep the current shipping behavior during qualification. Wire the Free
   mapping through the trusted edition decision when both channels are ready
   for production containment. A failed isolation setup must stop that APK
   launch.
