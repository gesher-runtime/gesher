# 0093: Wi-Fi connections through user-approved Windows profiles

Status: proposed, 2026-11-23. Implementation and contract details are in
[docs/wifi.md](../wifi.md) ("Connecting") and [docs/windows-wifi.md](../windows-wifi.md).

## Context

Android 10 removed apps' ability to change the device's Wi-Fi configuration
directly (`addNetwork`, `enableNetwork` and the rest fail for apps targeting API
29+). Apps join networks in two ways instead:

- `ConnectivityManager.requestNetwork` with a `WifiNetworkSpecifier`, for a
  local-only connection to a device (a camera, a printer, an IoT setup network).
  The system shows the user the matching networks, joins the one chosen, gives the
  app a `Network` without `INTERNET`, and undoes the connection when the request
  ends.
- `WifiManager.addNetworkSuggestions`: the app proposes networks, the user allows
  the app once, and the platform may then join them by its own network selection.

On Windows the WLAN AutoConfig service owns network selection and credentials.
Gesher has no Android supplicant; it can only ask Windows to connect.

## Decision

- Gesher implements both contracts with Windows WLAN profiles, written with
  `WlanSetProfile` as **per-user** profiles (`WLAN_PROFILE_USER`, no elevation)
  named `Gesher-...`, and connects with `WlanConnect`.
- A specifier request uses a temporary **manual** profile pinned to the chosen
  BSSID, so Windows never joins it on its own; it is deleted, and the interface's
  previous profile reconnected, when the request ends. This is Android's
  single-STA behaviour; Gesher does not create a second station interface.
- An approved app's suggestions become profiles whose connection mode follows
  `isInitialAutojoinEnabled`; Windows' own selection then plays the part of
  Android's auto-join. They are deleted on removal, on "Don't allow", when the app
  is uninstalled, and (as leftovers without an Android owner) after a restart.
- User approval uses Gesher's existing host surfaces: the list dialog of the intent
  chooser for choosing a network, the consent dialog for allowing suggestions.
  Without an interactive desktop nothing is approved and nothing changes.
- Success is reported only from Windows' connection-complete notification for that
  attempt's profile on that interface (and, for specifiers, an IPv4 address);
  failures map to Android's documented status codes.
- Security a Windows profile cannot express faithfully (enterprise EAP, WEP, WAPI,
  Passpoint) is refused with Android's failure results rather than joined
  differently.
- The legacy saved-network API keeps Android's failure results for all target SDKs.

## Consequences

- Windows networks change only after the user's approval in a Gesher dialog, as
  Android requires, and the profiles Gesher creates are identifiable and removed.
- Approved access points and the suggestion store live for the system server's
  lifetime; Android persists both. After a restart the user is asked again and
  apps must re-add suggestions (`getNetworkSuggestions` shows the loss).
- Windows' auto-connect attempts do not identify their failures to Gesher, so
  `SuggestionConnectionStatusListener` receives no failure reports.
- Only the first adapter with an enabled radio is used for specifier and
  suggestion connections.
- Real adapter behaviour (profile acceptance by drivers, WPA3-SAE and OWE profile
  support on older Windows builds, notification timing) needs hardware validation.
