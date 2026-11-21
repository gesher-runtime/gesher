# Bluetooth (bluetooth track)

Design: [0085](decisions/0085-bluetooth-host-radio.md). Tests:
`tests/windows/tracks/61-bluetooth.ps1` with `gesher.services.BluetoothHostTest`
(services-tests.dex), and `.github/workflows/bluetooth.yml` with
`tests/bluetooth/bluetooth_probe.cc` (the backend alone).

## Pieces

| Piece | Where |
|---|---|
| Backend interface | `compat/devices/bluetooth.h` (adapter, paired devices, inquiry, SDP service classes, LE scan, GATT client, RFCOMM client and listener) |
| Windows backend | `compat/devices/bluetooth_win.cc`: Windows.Devices.Bluetooth, .Advertisement, .GenericAttributeProfile, Windows.Devices.Radios and Windows.Devices.Enumeration through their ABI (winrt.h); BluetoothApis.dll (inquiry, loaded at run time); Winsock AF_BTH (RFCOMM, WSASetService) |
| Test radio | `compat/devices/bluetooth_test.cc`, chosen by `GESHER_BLUETOOTH_TEST=1` |
| JNI | `compat/devices/bluetooth_jni.cc` (`com.gesher.server.WindowsBluetooth`, registered by `register_gesher_devices`) |
| Java bridge | `app/java/com/gesher/server/WindowsBluetooth.java` |
| System side | `app/java/com/gesher/server/BluetoothManagerService.java` (`gesher.bluetooth`, in `Services.create`) |
| Module API | `app/java/android/bluetooth/` (see its README) |

## What apps get

- **System features**: `FEATURE_BLUETOOTH` when Windows has an adapter (looked at when
  the system server starts), `FEATURE_BLUETOOTH_LE` when it also reports LE support; both
  absent without a radio (the CI runners). `hasSystemFeature`, `getSystemAvailableFeatures`
  and the features cache in shared memory agree.
- **Adapter**: present when Windows has a radio (WinRT `BluetoothAdapter.GetDefaultAsync`).
  `getState()`/`isEnabled()` follow Windows' radio (Settings, Bluetooth on/off).
  `getName()` is the computer name; `getAddress()` is `02:00:00:00:00:00` as on Android.
  `enable()`/`disable()` ask Windows (`Radio.SetStateAsync`, after `RequestAccessAsync`);
  apps targeting API 33+ get false, as on Android. Without a Radio object (Windows.Devices.
  Radios unavailable) the state comes from BluetoothApis (a local radio is listed only
  while it is on).
- **Permissions**: BLUETOOTH_CONNECT / BLUETOOTH_SCAN for apps targeting API 31+,
  BLUETOOTH / BLUETOOTH_ADMIN below, else SecurityException. Scan results need
  ACCESS_FINE_LOCATION (API 29+, either location permission below) unless BLUETOOTH_SCAN
  is declared `neverForLocation`; without it the scan runs silently, as on Android.
  `gesher.bluetooth` checks its Binder callers itself, by package and target SDK, as the
  module's service does: switching the radio needs BLUETOOTH_CONNECT (API 31+) or
  BLUETOOTH_ADMIN and is refused to apps targeting API 33+; since it is the PC's radio the
  user also confirms it (the runtime-permission consent dialog). Discovery needs
  BLUETOOTH_SCAN / BLUETOOTH_ADMIN, SDP refresh BLUETOOTH_CONNECT / BLUETOOTH.
- **Paired devices**: Windows' pairings, classic and LE (`DeviceInformation.FindAllAsync`
  with the paired selectors), with name, class of device, connection state and the SDP
  service classes of their records (`BluetoothDevice.SdpRecords`, parsed for
  ServiceClassIDList). Empty while the radio is off. `fetchUuidsWithSdp()` asks the device
  again (an uncached RFCOMM service query) and broadcasts ACTION_UUID.
- **Pairing**: `createBond()` asks the system server, which pairs through Windows' basic
  pairing (`DeviceInformation.Pairing.PairAsync`): Windows shows its own pairing UI and the
  user confirms there. ACTION_BOND_STATE_CHANGED goes NONE>BONDING, then BONDING>BONDED
  when Windows lists the device, or back to NONE. False while the radio is off, for a
  bonded device or with its pairing running. ACTION_PAIRING_REQUEST is not sent: Windows'
  UI takes its place. The hidden `removeBond()` removes Windows' pairing
  (`UnpairAsync`) after the user confirms (Gesher's consent dialog); the watch broadcasts
  BONDED>NONE.
- **Discovery**: `startDiscovery()` runs a classic inquiry (BluetoothFindFirstDevice,
  about 10 s) in the system server: ACTION_DISCOVERY_STARTED, ACTION_FOUND (device, name,
  class) and ACTION_DISCOVERY_FINISHED. LE devices are found by LE scans.
- **LE scan**: one active BluetoothLEAdvertisementWatcher per process for all scans.
  Reports are rebuilt as AD structures from the advertisement's data sections and parsed
  by AOSP's ScanRecord; a scan response is merged with the advertisement it follows.
  Software filters, immediate ALL_MATCHES, PendingIntent scans and legacy `startLeScan`
  use this watcher. Controller-offloaded filtering is unavailable, so delayed scans,
  FIRST_MATCH, MATCH_LOST and auto-batch settings return
  `SCAN_FAILED_FEATURE_UNSUPPORTED`, as required by Android 16's scanner policy.
  Callback requests receive the error on the main thread; PendingIntent requests
  return the error code without registering a scan or sending the intent.
- **GATT client**: `connectGatt` opens the device (`FromBluetoothAddressAsync`, with the
  address type) and a GattSession with MaintainConnection; Windows connects. Discovery
  reads services, characteristics and descriptors uncached, and included services
  (`GetIncludedServicesAsync`): those that are not primary services are listed as
  SERVICE_TYPE_SECONDARY; reads and writes (with and
  without response) map to Windows' calls with their ATT error as the status (an
  unreachable device is 133, as on Android). One request at a time, as the module.
  Writing a CCCD subscribes through `WriteClientCharacteristicConfigurationDescriptor`;
  notifications and indications arrive as `onCharacteristicChanged`. `requestMtu` reports
  Windows' negotiated MaxPduSize (never above the request). `autoConnect` retries until
  the device is there. `readRemoteRssi` answers the last advertisement's RSSI. The hidden
  `readUsingCharacteristicUuid` reads the first discovered characteristic of the UUID in
  the handle range (false when there is none). `readPhy` reports the PHY Windows 11
  gives (`GetConnectionPhy`), else 1M with GATT_REQUEST_NOT_SUPPORTED; `setPreferredPhy`
  is declined (GATT_REQUEST_NOT_SUPPORTED, the PHY unchanged): Windows chooses it. Several
  clients of the process may connect to one device; it stays connected until the last
  one closes.
- **RFCOMM**: `createRfcommSocketToServiceRecord` / `createInsecure...` connect AF_BTH
  sockets by service UUID (Windows looks it up in SDP); the hidden `createRfcommSocket(int)`
  that SPP apps call by reflection connects to a channel. Streams, `available()`, and an
  IOException at the end of the stream, as on Android. `connect()` is a non-blocking
  connect and a wait, so `close()` from another thread cancels it. Secure sockets require
  authentication and encryption (SO_BTH_AUTHENTICATE, SO_BTH_ENCRYPT); the hidden
  `isEncrypted()` is true only while such a socket of the process is connected to the
  device, since Windows reports no encryption of other links (it answers false then). `listenUsingRfcommWithServiceRecord`
  binds a channel and registers an SDP record (WSASetService).
- **Profiles**: `getProfileProxy` gives BluetoothA2dp and BluetoothHeadset proxies listing
  the paired devices Windows has connected that offer an A2DP sink / hands-free service.
- **Broadcasts** (BluetoothManagerService, from the first app call on): the watch looks at
  Windows when Windows announces a change (Radio.StateChanged, the paired devices'
  ConnectionStatusChanged, a pairing made here) and at least every 10 s (pairings made in
  Settings):
  ACTION_STATE_CHANGED (TURNING_ON/ON, TURNING_OFF/OFF), ACL_CONNECTED/DISCONNECTED,
  BOND_STATE_CHANGED (pairings made or removed while the radio is on), the A2DP and
  headset profiles' CONNECTION_STATE_CHANGED and the adapter's.

## Adapter request intents

Contracts: the [BluetoothAdapter request actions](https://developer.android.com/reference/android/bluetooth/BluetoothAdapter#ACTION_REQUEST_ENABLE)
and Settings' [RequestPermissionActivity](https://android.googlesource.com/platform/packages/apps/Settings/+/refs/heads/main/src/com/android/settings/bluetooth/RequestPermissionActivity.java).

`BluetoothAdapter.ACTION_REQUEST_ENABLE` and `ACTION_REQUEST_DISABLE` use the
system activity path, including for applications targeting API 33 or newer. They
require the caller's `BLUETOOTH_CONNECT` grant on API 31+, or its declared
`BLUETOOTH` permission below that target. A desktop consent prompt runs outside
the activity/Binder thread. Success is returned only after an existing adapter is
observed in the requested state; a rejected prompt, missing adapter, failed
operation or transition timeout returns `Activity.RESULT_CANCELED`. A radio that
was already in the requested state needs no mutation or confirmation.

`ACTION_REQUEST_DISCOVERABLE` requires `BLUETOOTH_ADVERTISE` for modern callers
(`BLUETOOTH` for legacy callers). The backend currently cannot make the local
radio discoverable. It completes with `RESULT_CANCELED` without changing radio
power or reporting an invented discoverable duration.

`BluetoothRequests` admits at most sixteen pending requests and one per package.
Queued work rechecks installed package identity, enabled/removal state, caller
lifetime and grants before changing the adapter. Explicit foreign components,
foreign packages, data/MIME types and unsupported categories are not intercepted.
An intent selector selects the handler; the delivered outer action determines
the operation. A native consent request covers its source activity and keeps the
runtime alive until completion. Results retain their original recipient and
request code, including forwarded results and `NEW_TASK` cancellation.

`BluetoothRequestIntentTest` runs the production helper and activity route with
isolated server-local radio, executor and clock boundaries in the Windows ART
track. `tools/test_bluetooth_requests.py` runs the same fixture on a host JVM,
including separate fixture/service class loaders. Both use constructed package
and permission metadata; they do not test real Windows consent/radio hardware or
persistent permission metadata parsing.

## Scan settings without controller offload

The contract follows `isSettingsConfigAllowedForScan` and
`postCallbackErrorOrReturn` in the pinned
[Android 16 r4 BluetoothLeScanner](https://android.googlesource.com/platform/packages/modules/Bluetooth/+/refs/tags/android-16.0.0_r4/framework/java/android/bluetooth/le/BluetoothLeScanner.java).
When offloaded filtering is unavailable, only `CALLBACK_TYPE_ALL_MATCHES` with
zero report delay is accepted. A callback receives one posted
`SCAN_FAILED_FEATURE_UNSUPPORTED`; the PendingIntent overload returns that code
synchronously without sending the intent. Neither request creates a registration.
An already registered callback still receives `SCAN_FAILED_ALREADY_STARTED`
before its new settings are considered, and a rejected new callback can be
retried with supported settings. As in Android, radio state, duplicate callbacks
and unsupported settings are checked before registration enforces scan permission.
An admitted scan still throws `SecurityException` if the caller lacks that permission.

`tools/test_bluetooth_scan_contracts.py` compiles the complete production scanner
with explicit scheduling/value-type/native boundaries. It checks these failure
paths, permission/error precedence, retry, immediate results, PendingIntent payload
and cancellation, and stop/cleanup suppressing queued delivery. `--source-ref` is a
regression negative control: the original scanner compiles, then fails the rejection assertions.
The public API assertions in `BluetoothHostTest.scan` also run in the existing
Windows ART track with its isolated test radio. These tests do not demonstrate
physical controller offload, GATT on real devices, or completion of issue #56 B03.

## Not there

- LE advertising (`getBluetoothLeAdvertiser()` returns null and extended advertising
  capability and data length are false/zero; Windows'
  BluetoothLEAdvertisementPublisher could back it), GATT server (`openGattServer` is null;
  GattServiceProvider could back it), L2CAP channels, LE Audio, HID, PAN, PBAP/MAP.
- Custom pairing (PIN entry by the app, ACTION_PAIRING_REQUEST, setPin,
  setPairingConfirmation): Windows' own UI does it. ACTION_REQUEST_ENABLE /
  REQUEST_DISCOVERABLE have no activity.
- Reliable writes, PHY selection (declined), connection priority (accepted only).
- In device mode each app process has its own scan watcher and GATT sessions; Windows
  shares the radio between them.

## The Soundcore app (com.oceanwing.soundcore)

Anker's earbuds talk to the app over RFCOMM (SPP or a vendor UUID
`0cf12d31-fac3-4553-bd80-d6832e7xxxxx`, OpenSCQ30's notes), on the classic link Windows
already holds for A2DP; some devices use BLE GATT. With this track the app finds the
paired earbuds (getBondedDevices, the A2DP proxy, getUuids), and can open the RFCOMM
channel and exchange packets. Still needed for the app itself: it now gets past its
packer's native code (ADR 0084) but stops before its UI, when ART verifies its
application class.

## Status

- Verified on CI (both Windows runners): the backend probe (all 27 checks of the test
  radio; Windows' real calls answer "no adapter", HRESULT 0x8007048F, on runners without
  a radio); see the bluetooth track's runs.
- Built and run by ci.yml's tests: BluetoothHostTest (61-bluetooth.ps1).
- Verified once on a PC with a real radio and paired Soundcore Liberty 4 Pro earbuds:
  `bluetooth_probe.exe --hw "<name>"` finds the adapter and the paired device and its
  services (read-only), and `--query-state` opens RFCOMM to the vendor service, sends
  one request-state query and decodes the state update (battery and firmware levels).
- Not verified: GATT on real devices, Windows' GATT cache and protected services, and
  an app driving the real radio end to end. Without options the probe prints what a PC
  with a radio answers (adapter, paired devices, a 4 s scan; `--inquiry`: an inquiry)
  and never connects to a device.
