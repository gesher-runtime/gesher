# Telephony over a Windows cellular modem

Gesher backs Android telephony with the host's cellular (WWAN) modem when Windows
reports one, and otherwise behaves as an Android device without telephony. The
design decision is [ADR 0091](decisions/0091-telephony-cellular-modem.md).

## Android contract

Applications reach telephony through `TelephonyManager`, `SubscriptionManager`,
`TelephonyCallback`/`PhoneStateListener` and the `phone`, `iphonesubinfo`, `isub` and
`telephony.registry` Binder services. Some answers come from system properties written by
the phone process (`gsm.operator.*`, `gsm.sim.*`, `gsm.network.type`,
`gsm.version.baseband`), not from Binder calls.

Gesher reports `ro.product.first_api_level=34` and no `ro.vendor.api_level`, so the
device's vendor API level is below V. AOSP therefore does not enforce the telephony
feature mapping (`ENABLE_FEATURE_MAPPING`): no method throws
`UnsupportedOperationException` for a missing feature, `getSystemService` still returns
`SmsManager`/`CarrierConfigManager`, and `TelephonyManager` falls back to the
`config_voice_capable`, `config_sms_capable` and `config_mobile_data_capable` resources when
the matching feature is absent. Gesher's framework-res device overlay
(`config/framework-res-overlay`) sets all three to `false`, as a Wi-Fi tablet's device
overlay does. Consequently:

- without a modem: `getActiveModemCount()` is 0, `getPhoneType()` is `PHONE_TYPE_NONE`,
  `isVoiceCapable()`, `isSmsCapable()` and `isDataCapable()` are false;
- with a modem: `isDataCapable()` follows `android.hardware.telephony.data`, there is one
  phone and `getPhoneType()` is `PHONE_TYPE_GSM`; voice and SMS stay unsupported.

The overlay takes effect when framework-res is rebuilt (`.github/workflows/framework.yml`).

## Windows mechanism

`compat/net/windows_cellular.cc` (built-in `libgesher_cellular.so`) reads Windows'
Mobile Broadband API (`IMbnInterfaceManager` on the WwanSvc COM server):
interface capabilities (cellular class, data classes, device ID, model, firmware), ready
state, subscriber information (IMSI, ICCID, telephone numbers), home provider,
registration (state, provider ID/name, current data class), signal (RSSI as a 27.007 CSQ
value, bit error rate) and the packet connection's activation state. It never connects,
disconnects, scans, unlocks a PIN, changes registration or touches SMS. `REGDB_E_CLASSNOTREG`,
`ERROR_NOT_FOUND` and a disabled/missing WWAN service mean "no modem"; any other failure is
an error, not an absent modem. The interface declarations are written from the documented
`mbnapi.idl`, which llvm-mingw does not ship; IIDs are looked up through COM, so a mismatch
fails closed.

`com.gesher.server.WindowsCellular` parses the snapshot and translates it:

| Windows | Android |
| --- | --- |
| ready state INITIALIZED / SIM_NOT_INSERTED, NO_ESIM_PROFILE / BAD_SIM / DEVICE_LOCKED / DEVICE_BLOCKED / OFF, NOT_ACTIVATED / FAILURE | `SIM_STATE_LOADED` (reported as READY) / `ABSENT` / `CARD_IO_ERROR` / `PIN_REQUIRED` / `PUK_REQUIRED` / `NOT_READY` / `UNKNOWN` |
| register state HOME / ROAMING, PARTNER / SEARCHING / DENIED | data registration HOME / ROAMING / searching / denied |
| current data class (highest bit) | `NETWORK_TYPE_NR` (5G SA), `LTE` (LTE, 5G NSA), `HSPA`/`HSDPA`/`HSUPA`/`UMTS`/`EDGE`/`GPRS`, EV-DO, 1xRTT |
| connection ACTIVATED / ACTIVATING / DEACTIVATING | `DATA_CONNECTED` / `DATA_CONNECTING` / `DATA_DISCONNECTING` |
| CSQ 0..31 | RSSI −113..−51 dBm in the current RAT's `CellSignalStrength` (LTE, WCDMA or GSM); NR SA and CDMA have no RSSI field and stay empty |
| provider ID | MCC/MNC; country ISO from AOSP's MCC table |

`TelephonyService` keeps one model of the modem (Android phone 0). The modem present at
boot fixes the phone count and the system features, as on Android; a daemon worker polls
every five seconds only when a modem exists. A modem that disappears leaves phone 0 with
radio-off service state and `SIM_STATE_NOT_READY`; a read failure keeps the last reading
for at most two further polls. Subscription IDs are assigned per ICCID from 1 for the boot
session (not persisted).

## What applications observe

System features (only with a modem at boot): `android.hardware.telephony`,
`.telephony.subscription`, `.telephony.radio_access`, `.telephony.data`, and
`.telephony.gsm` for a GSM-class (3GPP) modem. Never `.calling`, `.messaging`, `.ims`,
`.euicc` or `.cdma` (Android 16 no longer supports legacy CDMA).

Permissions follow AOSP's `TelephonyPermissions` and `LegacyPermissionManagerService`
without carrier privileges (there are no carrier apps):

- phone state (`getServiceState`, `getDataNetworkType`, subscription info, call-state
  listeners for target ≥ S): `READ_PHONE_STATE` with its app op, or
  `READ_PRIVILEGED_PHONE_STATE`; `READ_BASIC_PHONE_STATE` suffices for the data network
  type. A missing permission throws `SecurityException`; a revoked app op returns
  placeholder data.
- device and subscriber identifiers (IMEI, IMSI, ICCID): `READ_PRIVILEGED_PHONE_STATE`, the
  `READ_DEVICE_IDENTIFIERS` or `USE_ICC_AUTH_WITH_DEVICE_IDENTIFIER` app op; apps targeting
  below Q with `READ_PHONE_STATE` get null, everyone else `SecurityException`. The type
  allocation code needs no permission.
- phone number: `READ_PHONE_NUMBERS`, `READ_SMS`, the default SMS app's `WRITE_SMS` app op,
  or `READ_PHONE_STATE` for targets ≤ Q.
- `SubscriptionInfo` hides the ICCID/card string without identifier access and the number
  without phone-number access. Operator name and numeric in `ServiceState` need coarse
  location (permission, app op and the location switch).

As in AOSP, permission checks run before the phone lookup, so a caller without permission
gets `SecurityException` even on a machine without a modem.

`TelephonyCallback`/`PhoneStateListener` deliver, for phone 0: service state, signal
strength(s), data connection state and the (always idle) call state, initially and on
change; subscription listeners hear SIM changes. Other events are permission-checked as in
AOSP and then never delivered.

Mobile data is the WWAN IP interface, which `ConnectivityService` already reports as a
`TRANSPORT_CELLULAR` network.

## Unsupported (Android's own answers)

- Voice calls: no `FEATURE_TELEPHONY_CALLING`, `isVoiceCapable()` false, call state idle,
  no voice registration.
- SMS/MMS: no `FEATURE_TELEPHONY_MESSAGING`, no `isms` service; `SmsManager` send methods
  throw AOSP's `UnsupportedOperationException("Sms is not supported")`.
- Carrier configuration, carrier identification (`UNKNOWN_CARRIER_ID`), carrier
  privileges (`CARRIER_PRIVILEGE_STATUS_NO_ACCESS`), eUICC, IMS, cell info
  (`getAllCellInfo` empty), network scans, IMEISV, `isDataEnabled` (Default false), data
  activity and `TelephonyDisplayInfo` are not backed by the modem.
- One modem only; a second WWAN interface is ignored.
- Telephony broadcasts (`ACTION_SIM_CARD_STATE_CHANGED`, service-state and
  default-subscription broadcasts) are not sent.

## Validation boundaries

- `tools/test_windows_cellular_native.py` compiles the unchanged native adapter against an
  explicit COM boundary under ASan/UBSan: absent service/interface, errors, apartments,
  exact UTF-8 fields, unreported fields, bounds and that every BSTR, SAFEARRAY and COM
  reference is released. Two negative controls (leaked capability strings,
  uninitializing a foreign apartment) must fail.
- `tools/test_telephony.py` runs the production Java services on a host JVM with synthetic
  modems and callers: features, translations, permission rules, redaction, listeners,
  SIM removal/insertion, read failures and the no-modem path.
- `ManagerConstructionBatchTest` (ART, Windows CI) runs the real Mobile Broadband probe on a
  runner without a modem and checks the absence of telephony features and AOSP's no-phone
  answers through `TelephonyManager`.

No physical modem has been tested: the native adapter's interface declarations were
checked against the Windows SDK's `mbnapi.h` locally, but real WwanSvc behavior, access
rights for subscriber information, modem timing and the Windows cellular UI are
unverified until someone runs Gesher on a machine with a WWAN modem.
