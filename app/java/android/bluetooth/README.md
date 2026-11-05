# Bluetooth callback and value types

The adapter remains unavailable (`BluetoothManager.getAdapter()` and
`BluetoothAdapter.getDefaultAdapter()` return null). Apps may create BLE
callbacks before checking for an adapter; OsmAnd 5.4.9 does so during startup.

`Attributable.java`, `BluetoothUuid.java`, and the six files under `le/` are
unmodified AOSP sources from `packages/modules/Bluetooth`, tag
`android-16.0.0_r4`, under `framework/java/android/bluetooth/`:

https://android.googlesource.com/platform/packages/modules/Bluetooth/+/refs/tags/android-16.0.0_r4/framework/java/android/bluetooth/

They implement Android's real callback, scan result, advertisement parsing,
transport data and parcel formats. They neither start scanning nor advertise a
Bluetooth feature. `BluetoothDevice` supplies the address value/parcel and
attribution contract those types need, while its hardware-dependent answers
remain unavailable.
