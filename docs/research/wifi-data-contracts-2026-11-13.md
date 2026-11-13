# Wi-Fi data contracts without a radio backend

The baseline is PR #12 head `45a278b4c506978f2a39c4070df00a19e303c908`,
which already includes PRs #10 and #11. Its Wi-Fi manager intentionally reports
no Android Wi-Fi controller. That does not remove the independent public data
contracts: callers can construct a `WifiSsid`, name `SupplicantState`, or read
the disconnected `WifiInfo` returned by the manager without using a radio.

This change supplies real data implementations for the two missing types and
adds 17 constants and 16 query/mapping methods to the existing WifiInfo snapshot.
The four directional/max-link-speed getters intentionally return zero, as the
pinned Android 16 default constructor does. Upstream `reset()` instead assigns
minus one; it is a distinct operation, not the constructor used by this snapshot.

## Source provenance

All references come from `platform/packages/modules/Wifi` at
`android-16.0.0_r4`, under `framework/java/android/net/wifi/`:

| File | Upstream SHA-256 | Import policy |
| --- | --- | --- |
| [WifiSsid.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/refs/tags/android-16.0.0_r4/framework/java/android/net/wifi/WifiSsid.java) | `a7a2bbcc8769f6bfe14d0ad7affccb8da16d9aed72132cda3d1b0f56184aa287` | Unchanged, including the Apache 2.0 header. |
| [SupplicantState.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/refs/tags/android-16.0.0_r4/framework/java/android/net/wifi/SupplicantState.java) | `9ed6f5c024cc878f6a42be069fb241cc0ee3f7ebb98093767f02c94df66519f4` | Only the unavailable AndroidX Keep import and annotation are removed. D8 is used without shrinking. |
| [util/HexEncoding.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/refs/tags/android-16.0.0_r4/framework/java/android/net/wifi/util/HexEncoding.java) | `99e9186f20fd88e3de58cf3e64b9718042854229772797515396a4f1c1bdac92` | Unchanged helper, including the Apache 2.0 header. |
| [WifiInfo.java](https://android.googlesource.com/platform/packages/modules/Wifi/+/refs/tags/android-16.0.0_r4/framework/java/android/net/wifi/WifiInfo.java) | `9b6a82a813cb612866e2d11448016622fcd62c8a290687bcf2db5a36b3bc0da2` | Reference for constants, state mapping and constructor defaults; the entire upstream WifiInfo is not imported. |

The imported SSID implementation retains its exact upstream byte ownership and
UTF-8 decoding behavior. Getter arrays are copies; the upstream constructor's
input byte-array ownership is not changed by this port.

## Validation and boundaries

`tools/test_wifi_data_contracts.py` compiles test calls against the independently
pinned SDK36 android.jar, then runs them against freshly compiled production
classes. It checks UTF-8 and non-UTF-8 SSIDs, null/empty values, byte equality and
hashing, getter copies, all thirteen enum mappings, runtime constant fields
(rather than only inlined constants) and disconnected query values.

The host run passes 133 assertions. Its declared native initialization adapter
does not implement Parcel. The same Java fixture on Windows loads the actual
android_runtime and additionally checks native Parcel round trips and exact
stream consumption for every supplicant state and binary/text SSIDs.

The identical SDK fixture compiled against the old production WifiInfo fails
with `NoSuchMethodError` at `getSupplicantState`. The negative control retains
the new value types to isolate the missing method; it does not claim to execute
old missing-class behavior. Full app javac/D8 compilation also passes against
the real framework/core compile inputs.

This does not implement a Windows Wi-Fi controller, scan results, connection
control, WifiInfo.Builder, MLO information, or WifiInfo's full mutable/redaction
model. Other absent module APIs remain separate inventory rows. Static API
presence and these focused data tests are not arbitrary-APK compatibility proof.
