# Windows Wi-Fi observations and scans

Gesher exposes real Windows WLAN observations through Android's `WifiManager`
and the Wi-Fi transport information in `ConnectivityManager`. This is a read
and scan integration. It does not create Android saved network configurations,
join an access point, change radio power, or implement `WifiNetworkSpecifier`.

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

Radio mutation methods continue to return unsupported results. Wi-Fi locks are
compatibility tokens, and a `WifiScanner` hardware service is not supplied.
This subset does not add `FEATURE_WIFI` to the advertised system features.

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
