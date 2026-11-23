# Windows Wi-Fi

Gesher exposes real Windows WLAN observations through Android's `WifiManager`
and the Wi-Fi transport information in `ConnectivityManager`. Every public SDK 36
`WifiManager` member and nested type exists (`tools/check_wifi_manager_api.py`
compares the built classes with the platform `android.jar`). It does not create
Android saved network configurations or change radio power.

## System feature and capabilities

`PackageManager.FEATURE_WIFI` is declared when a synchronous `WlanEnumInterfaces`
census, taken when the package manager starts, finds at least one WLAN interface.
A missing `wlanapi.dll`, a stopped WLAN AutoConfig service (`WlanOpenHandle`
failure) or zero interfaces leave it out. Wi-Fi Direct, Aware, RTT and Passpoint
features are never declared.

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
address does not establish a lease duration. Unknown DHCP fields remain zero.
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
