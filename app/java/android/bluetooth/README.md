# The Bluetooth module's API

Since the bluetooth track ([docs/bluetooth.md](../../../../docs/bluetooth.md), ADR 0085)
these classes run over the host's radio (`com.gesher.server.WindowsBluetooth`): there is
an adapter when Windows has a Bluetooth radio (or with `GESHER_BLUETOOTH_TEST=1`), and
none otherwise (`BluetoothManager.getAdapter()` and `BluetoothAdapter.getDefaultAdapter()`
return null, as before; apps may still create BLE callbacks before checking, as OsmAnd
5.4.9 does during startup).

Gesher's own: BluetoothManager, BluetoothAdapter, BluetoothDevice, BluetoothGatt,
BluetoothSocket, BluetoothServerSocket, BluetoothA2dp, BluetoothHeadset (with
BluetoothAudioProfile), BluetoothProfile, BluetoothGattServer and its callback (types
only), BluetoothHost (permissions, adapter, what the process knows of devices),
le/BluetoothLeScanner and le/BluetoothLeAdvertiser. Unmodified AOSP sources from the
same tag (below): BluetoothClass, BluetoothGattService, BluetoothGattCharacteristic,
BluetoothGattDescriptor, BluetoothGattIncludedService; BluetoothGattCallback with its
flag annotation spelled as a string and the subrate callback's annotations removed.

## Callback and value types

`Attributable.java`, `BluetoothUuid.java`, and the six files under `le/` are
unmodified AOSP sources from `packages/modules/Bluetooth`, tag
`android-16.0.0_r4`, under `framework/java/android/bluetooth/`:

https://android.googlesource.com/platform/packages/modules/Bluetooth/+/refs/tags/android-16.0.0_r4/framework/java/android/bluetooth/

They implement Android's real callback, scan result, advertisement parsing,
transport data and parcel formats. They neither start scanning nor advertise a
Bluetooth feature. `BluetoothDevice` supplies the address value/parcel and
attribution contract those types need, while its hardware-dependent answers
remain unavailable. That includes the hidden `prepareToEnterProcess(AttributionSource)`
framework.jar's `Intent.prepareToEnterProcess` calls on a Bluetooth broadcast's
`EXTRA_DEVICE` (issue #303). `app/gen/BootLinkCheck.java` (run by `app/build.sh`) fails
the build when framework.jar references a member these classes lack.

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
value types. Adapter availability remains null before and after all tests (the
runners have no radio). Scanning, GATT, sockets and broadcasts are tested by
BluetoothHostTest over the test radio (tests/windows/tracks/61-bluetooth.ps1).
