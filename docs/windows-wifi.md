# Windows Wi-Fi observations and scans

The [Wi-Fi service contract](wifi.md) describes Android permissions and Binder
authority. This note covers the native backend and its validation.

`com.gesher.net.WindowsWifi` is the internal data backend of the authenticated
Wi-Fi service. Android Wi-Fi and connectivity wrappers obtain its observations
through that service. Like other raw host backends, direct internal access is
outside the [Android API permission boundary](microg.md); it is not an OS sandbox.
`compat/net/windows_wifi.cc`, registered as the built-in
`libgesher_wifi.so`, dynamically loads only Windows' system `wlanapi.dll`. A server
without the optional Wi-Fi DLL can still start Gesher normally.

The backend reads `WlanEnumInterfaces`, `WlanQueryInterface` and
`WlanGetNetworkBssList`. A synchronous census (`nativeHardware`) reports the
interface count and, per adapter, `WlanGetInterfaceCapability` PHY types and the
`wlan_intf_opcode_supported_infrastructure_auth_cipher_pairs` list; IHV-defined
PHY types and malformed pair lists contribute nothing. One watcher thread
(`nativeWatch`) registers ACM and MSM notifications on its own handle. The Windows
callback only ORs an event class into an atomic and signals an event; the Java
thread that owns the handle drains it, so no JNI runs on Windows' threads. The
first delivery after registration requests a full re-read, so a change racing
startup is not lost. Unregistration and close drain callbacks before the state is
freed; a failed close retains it as the scan path does. When WLAN cannot be opened
(stopped service), the watcher retries with a 30 s to 5 min backoff. A registration
silently stops delivering when the WLAN service restarts, so a quiet poll interval
(30 s) checks that the handle still reaches the service; if not, the watch ends and is
reopened through the same backoff, which also repeats the census. Interface GUIDs are converted to IP interface indices;
a request for one `Network` never receives another adapter's SSID, address or
link state. SSIDs cross the native boundary as their original bytes, so they do
not depend on a shell's code page or translated command labels. The backend does
not run `netsh` or PowerShell.

User-approved connections (`WifiConnections`, [Wi-Fi contracts](wifi.md#connecting))
use four more calls. `nativeConnect` stores a per-user profile with
`WlanSetProfile(WLAN_PROFILE_USER)`, registers ACM notifications on its own handle
before `WlanConnect`, and returns only the outcome Windows reports for that profile
on that interface (`connection_complete` or `connection_attempt_fail`); another
profile's completion is ignored, and no notification within the deadline is
`NO_RESPONSE`. The requester's liveness is checked before any Windows change and
while waiting. Reason codes map to Android's local-only failure codes (security
reasons and the MSM security range to `AUTHENTICATION`, association
rejection/timeout to `ASSOCIATION`, not visible/not available to `NOT_FOUND`).
`nativeDisconnect` disconnects, deletes the temporary profile and reconnects the
previous profile (used only while the interface is still on Gesher's profile or on
none); `nativeStoreProfile` stores or deletes suggestion profiles and moves a stored
one to the end of the preference list with `WlanSetProfilePosition` (deleting it if
that fails);
`nativeProfiles` lists only per-user profiles with Gesher's prefix (all-user
profiles are never listed or removed); `nativeCurrentProfile` reads the connected
profile name. Profile XML (`WlanProfiles`) carries the SSID as hex bytes and
escapes every text value; the native copy of the XML, which may hold a passphrase,
is zeroed after use.

## Observable behavior

- Reads use a cache refreshed by one daemon worker. The first read or an expired
  snapshot can report `WIFI_STATE_UNKNOWN`; reading does not wait for Windows RPC.
  Known state is reused for at most ten seconds while refreshing. Failed refreshes
  use a two-second retry interval rather than creating a thread for every getter.
- `startScan` queues a real `WlanScan` only for an observed enabled radio. The
  worker registers completion notifications before initiating requests and waits
  at most five seconds for them. A requester's identity/permission predicate is
  rechecked immediately before each physical scan, including after native setup.
  At least one coalesced requester must remain eligible.
- A driver rejection, missing completion, Windows location denial, malformed data
  or an unavailable BSS list cannot report fresh results. An explicit Windows
  access denial also clears prior private scan data. The Java request deadline
  is fifteen seconds; a stuck native worker continues to own the only worker slot,
  so callers cannot accumulate blocked threads or queued native operations.
- Scan timestamps preserve the observed beacon's age. Windows' `ullHostTimestamp`
  is converted from FILETIME into boot microseconds using `QueryInterruptTimePrecise`,
  the same clock as Android's elapsed time. The timestamp survives later adapter
  RPC and handle-close delays unchanged. WLAN observations remain available even
  when an interface's IP index cannot be resolved. RSN/WPA
  capabilities come from actual information elements; malformed/unsupported
  protected suites remain unknown, and WPA3 Enterprise is not reported as SAE.
- Returned `WifiInfo`, `DhcpInfo` and scan objects are independent snapshots. IPv4,
  netmask, gateway and DNS values use the matching `WindowsNetworks` interface.
  No DHCP server or lease duration is inferred from a gateway. No Android-owned
  profile is created: configured networks are empty and `WifiInfo.networkId` is -1.
- Radio/configuration mutation, SoftAP and the privileged `WifiScanner` service
  remain unsupported. `FEATURE_WIFI` follows the adapter census below.
  Android permissions, location redaction and protected broadcasts are handled
  by the authenticated service/caller layer, independently of Windows consent.

The native adapter bounds its complete serialized result to four MiB across all
interfaces. It drains notification callbacks before releasing their event/context.
If native handle teardown fails, it retains that one callback target and rejects
further WLAN operations in the process instead of using freed memory or growing
an unbounded retained-handle list.

Windows can independently deny location-sensitive WLAN calls. Gesher preserves
that denial; it does not change Windows privacy settings or report a fictitious
network. The relevant Windows contracts are:

- [WlanScan and completion notifications](https://learn.microsoft.com/en-us/windows/win32/api/wlanapi/nf-wlanapi-wlanscan)
- [WLAN_BSS_ENTRY and host timestamp units](https://learn.microsoft.com/en-us/windows/win32/api/wlanapi/ns-wlanapi-wlan_bss_entry)
- [WlanCloseHandle callback lifetime](https://learn.microsoft.com/en-us/windows/win32/api/wlanapi/nf-wlanapi-wlanclosehandle)
- [Windows Wi-Fi and location access](https://learn.microsoft.com/en-us/windows/win32/nativewifi/wi-fi-access-location-changes)

## Validation boundaries

`tools/test_windows_wifi.py` compiles and runs the production Java backend with a
separately loaded test-native response boundary. It covers raw SSIDs, security,
interface matching, mutable results, failed queries, in-flight invalidation,
nonblocking reads, coalescing, callbacks, deadlines and queued grant revocation.

`tools/test_windows_wifi_native.py` compiles the unchanged production native source
against explicit test Win32/WLAN calls under ASan/UBSan. It covers callback ordering,
bounded waits, denied operations, exact byte fields, size checks and teardown.
Its two negative controls demonstrate that premature notifications and omission of
the live eligibility check are detected. This host boundary does not verify the
Windows ABI; the source is also compiled against the real llvm-mingw headers.

Windows track 50 runs `WindowsWifiTest` on ART and checks that live reads remain
nonblocking without assuming an adapter exists. Standard hosted CI has no physical
radio; scan reception, actual Wi-Fi hardware and the Windows consent UI require
separate hardware validation. Host results are not a claim of that validation.

The census, capability mapping and notification watcher are covered the same
way: the native fixture checks PHY/auth/cipher unions, failure cases and watcher
teardown; `WindowsWifiFixture` checks that only successful reads report state
transitions and that a requested scan's echo is not published twice; the service
harness checks permissions, sticky state broadcasts and callback delivery.
Whether a given adapter driver reports its PHY types and auth/cipher pairs
completely, and the timing of real WLAN notifications, need hardware validation.
