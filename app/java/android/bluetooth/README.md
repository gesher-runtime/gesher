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

The additional scan settings/filter, advertisement data/settings/parameters,
periodic parameters/report, transport filter and advertising callback use the
same pinned AOSP tag. BluetoothAssignedNumbers, BluetoothStatusCodes and the
scan permission annotation supply their real constant/annotation dependencies.
Exact URLs and upstream/local SHA-256 receipts are in
`docs/research/aosp-bluetooth-values-2026-11-05.json`.

The local adaptations are limited to generated flag annotation constants being
spelled as their string literals, ScanSettings' active scan default (both
branches of the upstream flag expression have value 2), and inlining the
BluetoothUtils.writeStringToParcel wrapper as Parcel.writeString. These avoid
importing the Bluetooth service stack and preserve the value/parcel behavior.
BluetoothDevice supplies its SOURCE-retained AddressType annotation;
BluetoothAdapter supplies upstream random-static address classification and
reports transport-discovery offload as unsupported.

BluetoothValueContractTest passed 31 assertions in a separate Windows runtime
with isolated ART data and no Binder connection. It covers settings defaults
and validation, real advertisement parsing, compound and masked filter matches
and mismatches, builder conflicts, and complete parcel consumption for seven
value types. Adapter availability remains null before and after all tests.
It does not test scanning, advertising, connections or any Bluetooth hardware.
