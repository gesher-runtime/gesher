# Windows Wi-Fi

Gesher exposes real Windows WLAN observations through Android's `WifiManager`
and the Wi-Fi transport information in `ConnectivityManager`. Every public SDK 36
`WifiManager` member and nested type exists (`tools/check_wifi_manager_api.py`
compares the built classes with the platform `android.jar`). It does not create
Android saved network configurations or change radio power. Apps join networks
through Android 10's contracts, network specifiers and suggestions, and only after
the user approves (see [Connecting](#connecting)).

## System feature and capabilities

`PackageManager.FEATURE_WIFI` is declared when a `WlanEnumInterfaces` census, taken
when the package manager starts, finds at least one WLAN interface. A missing
`wlanapi.dll`, a stopped WLAN AutoConfig service (`WlanOpenHandle` failure) or zero
interfaces leave it out. The census runs on its own thread and boot waits for it at
most 5 s: a WLAN service that hangs or is still starting leaves `FEATURE_WIFI` out for
that boot (logged as `WindowsWifi: WLAN census did not finish ...`) instead of
stalling the system server. A capability query made while the census is still
running waits only for the rest of that first 5 s, never a new bound: a healthy census
answers it, a hung one means no hardware at once. Its late result is still used for
the capability answers below, but system features stay fixed until the next start, as
on Android. Wi-Fi Direct, Aware, RTT and Passpoint features are never declared.

Capability answers come from the same census, refreshed on WLAN interface
arrival/removal:

| API | Source |
| --- | --- |
| `isWpa3SaeSupported`, `isWpa3SuiteBSupported`, `isEnhancedOpenSupported`, `isWpaPersonalSupported`, `isWepSupported` | Adapters' supported infrastructure auth/cipher pairs (`DOT11_AUTH_ALGO_WPA3_SAE`, `WPA3_ENT_192`, `OWE`, `WPA_PSK`/`RSNA_PSK`, WEP ciphers) |
| `isWifiStandardSupported` | `WlanGetInterfaceCapability` PHY types (DSSS/HR-DSSS/OFDM/ERP: legacy; HT: 11n; VHT: 11ac; HE: 11ax; DMG: 11ad; EHT: 11be) |
| `is24GHzBandSupported`, `is5GHzBandSupported`, `is60GHzBandSupported` | Band-specific PHY types (DSSS/HR-DSSS/ERP; OFDM/VHT; DMG) or an actually observed BSS/link in the band |
| `is6GHzBandSupported` | Only an observed 6 GHz BSS or link: Windows reports no 6 GHz capability, so an idle 6E adapter answers false until it sees one |

An observed band is kept until the adapter census changes (interface arrival or
removal): reception is a hardware property, so the answer does not follow, and does
not disclose to apps without location access, which access points are nearby now.

Feature queries require `ACCESS_WIFI_STATE`, as `WifiServiceImpl.isFeatureSupported`
does; band and standard queries have no permission, as on Android. Facilities that
Windows does not expose to Gesher answer with Android's unsupported results:
H2E/public-key SAE, WAPI, DPP, TDLS (callbacks receive `false`, `-1` or `0`),
STA/AP concurrency, Passpoint (`addOrUpdatePasspointConfiguration` and
`removePasspointConfiguration` throw `IllegalArgumentException`), local-only
hotspot (`onFailed(ERROR_GENERIC)` after Android's permission checks), WPS
(`onFailed(0)`, synchronously), usable/allowed channels (permission and band
checks, then `UnsupportedOperationException`), and policy APIs reserved for
privileged callers (`SecurityException`; `UnsupportedOperationException` if a
caller holds the privilege). `getChannelData` counts the cached real scan's APs
per frequency (at least -80 dBm) for `NEARBY_WIFI_DEVICES` holders.

## Events

`WindowsWifi` keeps one daemon thread registered for WLAN ACM and MSM
notifications. Windows' callback threads only record event classes; the Java
thread drains them, re-reads WLAN state and publishes:

- `WIFI_STATE_CHANGED_ACTION` (sticky, system origin, `EXTRA_WIFI_STATE` and
  `EXTRA_PREVIOUS_WIFI_STATE`) and `WifiStateChangedListener` callbacks when a
  successful read shows a known radio state different from the last one. A failed
  read is not a transition, and `WIFI_STATE_UNKNOWN` is never broadcast.
  Listeners are also called once on registration, as on Android.
- `SCAN_RESULTS_AVAILABLE_ACTION` (`EXTRA_RESULTS_UPDATED=true`) and
  `ScanResultsCallback` for scans Windows completes on its own (background scans
  or other clients), as Android publishes every completed device scan. The
  completion of a scan Gesher requested is published once by its requester path.

Callback registrations are Binder objects held by the service, checked against the
caller's live admission and `ACCESS_WIFI_STATE` at each delivery, dropped when the
process retires or the binder dies, and bounded per UID.

## Connecting

`WifiConnections` (system server) implements the two Android 10+ ways an app joins a
network. Windows networks change only after the user approves in a Gesher host
dialog, and success is reported only after Windows' own connection-complete
notification for that attempt's profile on that interface (plus, for specifiers, an
IPv4 address).

### `WifiNetworkSpecifier` requests

`ConnectivityManager.requestNetwork` with a `WifiNetworkSpecifier` follows
`WifiNetworkFactory`:

- The caller needs `CHANGE_NETWORK_STATE` (`SecurityException`). A request that keeps
  `NET_CAPABILITY_INTERNET`, has an invalid specifier, sets a band, comes from an app
  that is neither a foreground app (a resumed activity) nor running a foreground
  service, arrives with Wi-Fi off, or uses security a Windows profile cannot express
  (enterprise) is unfulfillable: `onUnavailable`, and Windows is not touched.
- One request is served at a time, on one worker; a new accepted request ends the
  previous one (`onUnavailable`, or `onLost` if it was connected). As in
  `WifiNetworkFactory`, a foreground app's request overrides any other, a foreground
  service's request does not override a foreground app's, and a rejected request
  leaves the current one alone. An ended request never acts on Windows again; if it
  ends while Windows is connecting, that attempt is undone when Windows returns.
- Matching uses the real scan cache and real scans: SSID pattern, BSSID/mask and
  security type. As `WifiNetworkFactory`, the search scans every 10 s until a match
  appears or the request ends (released, its `requestNetwork` timeout, its app gone,
  or replaced); nothing is reported meanwhile. The difference from Android: Gesher's
  host list cannot update live, so it is shown once matches exist rather than while
  searching; the app's release or timeout ends the search. A request that would skip
  the dialog (approved before) gives up after three scans as unfulfillable
  (`onUnavailable`, no failure code), as Android's `USER_APPROVED_SCAN_RETRY_MAX`.
- The user picks one of the matching networks in the host list dialog (the same
  surface as the intent chooser), showing SSID, band and signal; cancel is
  `USER_REJECT`. As on Android, a network the user approved before for the app is
  joined without asking again only for a single-network request (literal SSID or
  exact BSSID), looked up with the request's own SSID and BSSID: open and OWE
  networks only when the request names the approved BSSID, other security types by
  SSID and type. Approvals are kept for the system server's lifetime (Android
  persists them), at most 50 per app.
- The join uses a per-user, manual-mode Windows profile named `Gesher-r-<random>`,
  pinned to the chosen access point's BSSID, so Windows never joins it on its own.
  Failures map to Android's codes: association rejection/timeout `ASSOCIATION`,
  security failures `AUTHENTICATION` (not retried), not visible `NOT_FOUND`, no
  completion within 30 s `NO_RESPONSE`, no provisioned IPv4 address within 20 s
  `IP_PROVISIONING` (an automatic 169.254/16 address does not count: Android's IpClient
  acquires IPv4 by DHCP and never uses one); other attempts are retried up to three
  times.
- Success: `onAvailable` with `Network(100 + ifIndex)`, the real interface's link
  properties, and capabilities with `TRANSPORT_WIFI` and no `INTERNET`. The network
  specifier is absent, as Android redacts the agent's `WifiNetworkAgentSpecifier` to
  null for apps; SSID and BSSID are only in the location-checked transport info. The
  service's availability event to the app's process carries only the interface
  index, never the joined SSID, BSSID or frequency.
- A specifier for a hidden network must name its SSID literally
  (`WifiConfigurationUtil.validateNetworkSpecifier`); a hidden network with an SSID
  pattern is invalid and gets `onUnavailable` at once. SSIDs shown in the dialogs
  have control and formatting (e.g. bidi override) characters replaced by spaces.
  Releasing the request, unregistering the callback or the app's death deletes the
  profile and, while the interface is still on it (or on none), disconnects and
  reconnects the interface's previous profile (Android's single-STA behaviour).
  Windows dropping the connection is `onLost`; a network the user or Windows chose
  meanwhile is kept, and only Gesher's profile is deleted.
- `requestNetwork` timeouts deliver `onUnavailable` and release the request.

### Network suggestions

- `addNetworkSuggestions` checks `CHANGE_WIFI_STATE` (exception) and its app op
  (`ERROR_APP_DISALLOWED`), validates every entry (`ERROR_ADD_INVALID`), and only then
  checks what the caller may add: carrier, subscription and carrier-merged entries are
  `ERROR_ADD_NOT_ALLOWED` (they need carrier privileges), as is the per-app limit
  (1024, 256 on low-RAM devices: `ERROR_ADD_EXCEEDS_MAX_PER_APP`). An equal suggestion
  replaces the stored one, as on Android 11+, and its Windows profile is rewritten with
  the new credentials and auto-join setting (`updateWifiConfigInWcmIfPresent`).
  Suggestions Windows profiles cannot express faithfully (enterprise, WEP, WAPI,
  Passpoint) return `ERROR_ADD_NOT_ALLOWED` rather than being stored and never joined.
- Untrusted (`setUntrusted`), restricted (`setRestricted`), OEM-paid and OEM-private
  suggestions are also `ERROR_ADD_NOT_ALLOWED`. Android selects them only for a
  `NetworkRequest` that asks for an untrusted, restricted or OEM network, never as the
  default network; a Windows auto-connect profile would make them every app's default
  network, with capabilities they must not have. (Android itself accepts them; this is
  a documented refusal rather than a silent change of their meaning.)
- SSIDs may be UTF-8 text or, as `WifiSsid` allows, arbitrary bytes (Android's
  unquoted hex form); both reach Windows as hex bytes.
- The first add of an app asks the user (host consent dialog, off the Binder
  thread). Until answered the status is `PENDING`. Allow: `APPROVED_BY_USER`, and
  each suggestion becomes a per-user Windows profile `Gesher-s-<hash>` whose
  connection mode follows `isInitialAutojoinEnabled`; Windows then joins it by its
  own network selection when it sees it. Windows adds a new profile at the top of the
  preference list; the suggestion is moved to the bottom, below every network the
  user saved, as Android scores user-saved networks above suggestions. If Windows
  cannot place it there, the profile is removed rather than outranking the user's. Don't allow: the app's
  `CHANGE_WIFI_STATE` app op becomes `MODE_IGNORED`, its suggestions are removed and
  the status is `REJECTED_BY_USER`, as Android does. Dismiss: still pending, asked
  again on the next add. Listeners get the current status on registration and on
  each change.
- `removeNetworkSuggestions` returns `ERROR_REMOVE_INVALID` for null or unknown
  entries, unknown actions, or an app that never added suggestions (even with an
  empty list), and removes nothing then; otherwise an empty list removes all. The
  call waits for the profile pass that deletes the profile
  (`ACTION_REMOVE_SUGGESTION_DISCONNECT`; with `ACTION_REMOVE_SUGGESTION_LINGER` the
  current connection's profile goes after 30 s) at most 4 s, as `WifiServiceImpl`
  waits for the Wi-Fi thread. If Windows has not answered by then the call returns
  `ERROR_INTERNAL`, as Android does on that timeout; the suggestions are already
  dropped and the queued pass still deletes their profiles.
- Windows profiles follow the suggestion store through one profile thread. Each pass
  converges from Windows' own state: it reads the per-user profile list of every WLAN
  interface Windows lists at that moment (radio on or off), and deletes each profile
  with exactly a Gesher-generated name (`Gesher-s-` plus 24 hex digits for suggestions,
  `Gesher-r-` plus 16 for requests) that is not wanted there. A suggestion profile is
  wanted only on the primary interface (while no radio is on, the last primary one),
  only while its suggestion is approved and not blocked, or during its linger period;
  a request profile only while its request is active. Missing or changed suggestion
  profiles are then stored on the primary interface; stores wait for a radio. A
  removal, uninstall or block made while a pass is talking to Windows is handled by
  the pass after it, so no profile outlives its suggestion.
- Interface indices are not stable (an adapter disabled and re-enabled can return
  under another one) and Windows keeps profiles per adapter, so nothing is remembered
  per index: an adapter that is disabled, unplugged or behind a stopped WLAN service
  is simply not listed, and every WLAN event (connection, radio, interface arrival or
  removal) runs a pass, which finds and deletes its leftovers when it is back,
  whatever its index. Leftovers of an earlier run, including a request profile a
  crashed system server left connected, go the same way on every interface. A list
  Windows could not read, or a store or deletion it refused, is retried with backoff
  (5 s doubling to 5 min) and on the next WLAN event; a pass with nothing unknown or
  failed ends the retry. A WLAN service that cannot enumerate interfaces at all also
  counts as unknown.
- When Windows joins an app-interaction suggestion, the app receives the explicit
  `ACTION_WIFI_NETWORK_SUGGESTION_POST_CONNECTION` broadcast with
  `EXTRA_NETWORK_SUGGESTION`, if location is on, it holds fine location and its
  fine-location app op is allowed (`enforceCanAccessScanResults`).
- `disallowCurrentSuggestedNetwork` takes the app's suggestions when called and
  returns at once (Android posts it to the Wi-Fi thread). On the profile thread, in
  order with the passes of the app's later adds and removals as on Android, the current
  connection is read; if it is one of those suggestions, Windows leaves it and its
  profile is removed for the blocking period; every profile pass honours the block
  until it expires, then the profile returns. Windows has no BSSID block list,
  so the whole network is blocked even when `BlockingOption` asks for the BSSID only.
- An uninstalled app's suggestions and approvals are dropped as soon as the
  package-removal event arrives, and the next profile pass deletes its profiles,
  without holding the event's sender. WLAN events and profile installations are
  coalesced: at most one pass of each is queued at a time. The store lives for the
  system server's lifetime (Android persists it). Profiles without exactly Gesher's
  generated names, including the user's own, are never deleted.
- `SuggestionConnectionStatusListener` registrations are accepted but no failures are
  reported: Windows' own auto-connect attempts do not identify their failure to
  Gesher.

The legacy saved-network API (`addNetwork`, `enableNetwork` and so on) keeps
returning Android's failure values for every target SDK: implementing it faithfully
for apps targeting API 28 or lower would need Android-owned saved networks with
their identifiers and visibility rules, which Gesher does not keep.

## Data and execution

`WindowsWifi` uses the Native Wi-Fi API in the system `wlanapi.dll`. Interface
identity comes from the Windows interface GUID/index, SSIDs retain their original
bytes, and scan security comes from the received RSN/WPA information elements.
The implementation does not parse localized shell output or run `netsh`.

The Java boundary maintains a bounded cache and runs Windows RPC on a daemon
worker. Queries return cached observations or Android's unknown/unavailable
values while a refresh is pending. They do not wait for Windows on the activity
or Binder thread. An expired operation cannot publish a late successful result;
an outstanding native operation also prevents an unbounded accumulation of
workers. Returned Android objects are copies, so app mutation does not alter
the cache.

A scan request invokes `WlanScan` and observes the corresponding interface's
scan-completion notification. A cached BSS-list read alone is not a completed
scan. Failed admission, permission denial, unavailable WLAN, timeouts and
driver errors do not manufacture success. Scan timestamps preserve the age of
the Windows observation instead of marking an old list as newly received.

IPv4 address, mask, gateway and DNS information comes from `WindowsNetworks`
for the same interface. A gateway is not evidence of a DHCP server, and an IP
address does not establish a lease duration. Unknown DHCP fields remain zero, and
`DhcpInfo.netmask` stays 0 as `WifiServiceImpl.getDhcpInfo` leaves it (the prefix is in
the network's `LinkProperties`).
The current connection does not create a saved Android profile: network ID
remains `-1`, and `getConfiguredNetworks()` is empty.

## Permission and result boundary

`IWindowsWifi` is an internal Binder interface registered as `gesher.wifi`.
`WifiService` authenticates the actual calling process and package, including
standalone applications sharing the system services' process. A package name
alone does not grant system identity. The service rechecks installation,
enabled state, lifetime, grants and AppOps when disclosing observations and
before a pending native scan starts.

- State, connection, DHCP and result queries require `ACCESS_WIFI_STATE`.
- Scan admission requires `CHANGE_WIFI_STATE` and its allowed AppOp.
- Ordinary scan/connection identifiers require enabled location and the
  appropriate location grant/AppOp, plus the Wi-Fi scan AppOp. Legacy targets
  below API 29 may use coarse location. Real privileged permissions retain the
  Android service's exemptions.
- Missing ordinary location access leaves connection identifiers redacted,
  scan results empty and scan admission unsuccessful. It does not disclose
  cached data from an earlier permission grant.
- Scan completion is a protected, system-origin `SCAN_RESULTS_AVAILABLE_ACTION`
  broadcast containing `EXTRA_RESULTS_UPDATED`. It carries no network data.
  Coalesced requests share one completion broadcast; each receiver's subsequent
  data read is checked independently.

`ConnectivityManager.getNetworkCapabilities()` always redacts location-sensitive
transport fields. Callbacks retain their registration context and disclose
location only with `FLAG_INCLUDE_LOCATION_INFO` and current permission/location
policy. Context supplies attribution, not authority: transport reads go through
the same authenticated service, which checks `ACCESS_NETWORK_STATE` and the
Connectivity location policy independently of client-supplied permission answers.
A shared sticky connectivity broadcast contains no SSID. Wi-Fi details
for a specific `Network` are looked up by that network's interface index; another
adapter's SSID is never substituted.

Radio power and the legacy saved-network API (`addNetwork`, `enableNetwork`,
`disconnect`, `setWifiEnabled` and the rest) keep Android's failure results after
the `CHANGE_WIFI_STATE` check: no Android-owned profile exists to act on. Wi-Fi and
multicast locks enforce `WAKE_LOCK`/`CHANGE_WIFI_MULTICAST_STATE`, the lock mode,
reference counting and Android's 50-lock limit; Windows keeps an associated adapter
up and does not filter multicast, so the lock has no further effect. A
`WifiScanner` hardware service is not supplied.

`WindowsWifi` is a runtime-internal backend, not an Android application API.
These service checks preserve the Android API boundary; they do not add a
Windows process sandbox to Gesher's existing [host trust model](microg.md).

## Validation boundaries

Host fixtures exercise production Java permissions, caches, callbacks and
system-service behavior with explicit native/context boundaries. Native
fixtures exercise WLAN completion, failure and teardown paths. Windows ART
fixtures cover the installed service/API and Android parcel representations.
Synthetic observations remain confined to these fixtures. Host fixtures and
cross-compilation do not establish operation with a physical Wi-Fi adapter.

## Contract references

- [Android 16 WifiServiceImpl](https://android.googlesource.com/platform/packages/modules/Wifi/+/refs/heads/android16-release/service/java/com/android/server/wifi/WifiServiceImpl.java)
- [Android Wi-Fi scan permissions and broadcasts](https://developer.android.com/develop/connectivity/wifi/wifi-scan)
- [ConnectivityManager transport redaction](https://developer.android.com/reference/android/net/ConnectivityManager#getNetworkCapabilities(android.net.Network))
- [NetworkCallback location opt-in](https://developer.android.com/reference/android/net/ConnectivityManager.NetworkCallback#FLAG_INCLUDE_LOCATION_INFO)
- [Microsoft WlanScan](https://learn.microsoft.com/windows/win32/api/wlanapi/nf-wlanapi-wlanscan)
- [Microsoft WlanGetNetworkBssList](https://learn.microsoft.com/windows/win32/api/wlanapi/nf-wlanapi-wlangetnetworkbsslist)
