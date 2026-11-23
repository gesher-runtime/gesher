# Telephony over a Windows cellular modem

Gesher backs Android telephony with the host's cellular (WWAN) modem when Windows
reports one, and otherwise behaves as an Android device without telephony. The
design decision is [ADR 0092](decisions/0092-telephony-cellular-modem.md).

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
value, bit error rate), the packet connection's activation state and the radio state
(`IMbnRadio`: the software radio, which airplane mode and Windows' Cellular switch turn off,
and the hardware radio). It never connects, disconnects, scans, unlocks a PIN, changes
registration, switches the radio or touches SMS. `REGDB_E_CLASSNOTREG`,
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
| software or hardware radio OFF, ready state OFF | `ServiceState.STATE_POWER_OFF`, `isRadioOn()` false, no data registration (the SIM state is unchanged) |
| a field Windows did not report | unreported, never a value: numeric fields are unsigned 32-bit, so `0xFFFFFFFF` stays a reported value |

`TelephonyService` keeps one model of the modem (Android phone 0). The modem present at
boot fixes the phone count and the system features, as on Android. Each read creates an
out-of-process WwanSvc object and makes about a dozen calls to it, so reads are
demand-driven: without a modem nothing is read after the boot probe; with one, a daemon
worker reads every five seconds only while `telephony.registry` holds a listener (phone
state or subscriptions) or for 30 seconds after the last query, then stops. A query that
finds the reading older than seven seconds (polling had stopped) reads the modem before
answering, so an answer is never an old reading presented as current. Every read is
bounded to two seconds (the boot probe to five); a read still blocked in WwanSvc is not
joined by a second one. A read that fails or times out keeps the last good reading only
for the next two reads and only while that reading is less than 15 seconds old; otherwise,
as for a modem that disappears, phone 0 has radio-off service state and
`SIM_STATE_NOT_READY` until a read succeeds. Subscription IDs are assigned per ICCID from 1
for the boot session (not persisted).

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
  without phone-number access. Operator name and numeric in `ServiceState` (from
  `getServiceState()` and from listeners) follow `LocationAccessPolicy` with minimum SDK Q,
  as PhoneInterfaceManager and TelephonyRegistry check them: from target Q coarse location
  (permission and app op) is needed; below Q no location permission is; the location switch
  off hides them at every target.
- Cell location and cell info listeners: `LocationAccessPolicy` with fine location from Q
  and coarse location at every target. A hard denial (no permission, an errored app op)
  throws `SecurityException("Unable to listen for events ...")`; a soft denial (location
  switch off, an ignored app op) registers nothing, without an exception.

These rules apply only when a modem was present at boot. Without one the device has no
`FEATURE_TELEPHONY`, and AOSP's `PhoneGlobals.onCreate` then returns before publishing
`phone`, `iphonesubinfo` or `isub` (`config_force_phone_globals_creation` is false).
`TelephonyManager` and `SubscriptionManager` answer from their "service is null" paths,
without any permission check. Gesher still registers the three services, and without a
modem they give exactly those answers: `NETWORK_TYPE_UNKNOWN`, null identifiers and numbers,
empty subscription lists and `IllegalStateException` from `getPrimaryImei()` and
`SubscriptionManager.getPhoneNumber()`, as the managers throw. The SIM state is
`UNKNOWN` too, for `getSimState()`, `getSimState(slot)` and what derives from them
(`getSimApplicationState()`, `getSimCardState()`): `TelephonyManager.getSimStateForSlotIndex`
answers `UNKNOWN` when `ITelephony` is null, and `getSimState()` asks it for
`DEFAULT_SIM_SLOT_INDEX` (`getSlotIndex()` turns `SubscriptionManager`'s null-service
`INVALID_SIM_SLOT_INDEX`, which is `SIM_NOT_INSERTED`, into it), so it does not depend on the
phone count (ADR 0094; ADR 0087 had answered `ABSENT`). With a modem, the state is the
modem's, `ABSENT` included when the modem reports no SIM (the table above). `telephony.registry`
(system_server, present on every Android device) keeps its listener permission checks with
or without a modem.

Without a modem the managers' null-service answers also cover: `getAllCellInfo()` null,
`getCellLocation()` null (the service returns an empty `CellIdentityGsm`, which the manager
turns into null; Default's null made the manager throw `NullPointerException`),
`isDataEnabledForReason()`, `isEmergencyNumber()`, `getEmergencyNumberList()`,
`isPremiumCapabilityAvailableForPurchase()`, `getNetworkSlicingConfiguration()`,
`uploadCallComposerPicture()` and `SubscriptionManager.getSubscriptionsInGroup()` throwing
`IllegalStateException`, `purchasePremiumCapability()` answering `REQUEST_FAILED`, GBA
`FEATURE_NOT_READY`, `getCardIdForDefaultEuicc()` `UNINITIALIZED_CARD_ID`, no UICC cards,
no network scan, and USSD and carrier-restriction requests dropped without a callback.

### With a modem: the rest of ITelephony

Every `ITelephony`/`ISub` method that a public `TelephonyManager`/`SubscriptionManager`
API reaches was checked for an inherited Default answer that contradicts the modem or
leaves a callback unanswered. The answers, with AOSP's permission checks:

- Phone type: Android 16's `getPhoneType()`/`getCurrentPhoneType()` are GSM for any phone
  (no `getActivePhoneTypeForSlot` exists any more); `gsm.current.phone-type` says GSM too,
  also for a CDMA-class modem.
- SIM card and radio: `hasIccCard()` follows the card (present while ready, locked, blocked,
  failed or not activated; not for an empty slot or a missing eSIM profile);
  `DEFAULT_SIM_SLOT_INDEX` (what `TelephonyManager` asks for without a SIM) is phone 0, as
  `PhoneFactory.getPhone` treats it, for the SIM state, IMEI, type allocation code, IMEISV,
  service state and network country (`INVALID_PHONE_INDEX` asks for the default phone
  there); `iphonesubinfo`'s device ID treats any invalid phone ID as phone 0, as
  PhoneSubInfoController does; the permission checks are unchanged.
  `isRadioOn*` need `READ_PHONE_STATE` (`SecurityException` without it, false with its app
  op revoked) and answer whether the subscription's phone is powered on: false while
  Windows' radio is off, and, as on AOSP, false from `isRadioOn()` without a SIM (no default
  subscription, so no phone). `isModemEnabledForSlot(0)` is true;
  `getCardIdForDefaultEuicc()` is `UNSUPPORTED_CARD_ID` (no eUICC);
  `getUiccCardsInfo()` lists the one removable card (sanitized for
  `READ_BASIC_PHONE_STATE`).
- Mobile data switches: Windows' Mobile Broadband API has no equivalent of Android's user,
  policy, carrier or thermal data switches (a connection profile's auto-connect mode only
  decides whether Windows connects by itself). `isDataEnabled()`,
  `isDataEnabledForReason()` and `isUserDataEnabled()` answer AOSP's default, enabled,
  while a subscription is active (false without one: no phone for it). This never
  contradicts `getDataState()`: a connected modem is enabled, and a disconnected one may
  be. `isDataRoamingEnabled()` answers AOSP's default, off, unless the modem is connected
  while roaming, which proves roaming allowed.
- Cell information: Windows reports no cells. `getAllCellInfo()` is an empty list,
  `getCellLocation()` an empty identity (null `CellLocation`),
  `requestCellInfoUpdate()` answers `CellInfoCallback.ERROR_MODEM_ERROR` with
  `CommandException`, as PhoneInterfaceManager answers a radio that does not support the
  request; all three with LocationAccessPolicy's checks (fine location at every target
  for `requestCellInfoUpdate`; a soft denial answers an empty list).
- Callback APIs: USSD needs `CALL_PHONE` and fails with `USSD_RETURN_FAILURE` (USSD APIs
  not supported); carrier restriction status throws "Not an authorized caller" (no allow
  list); call composer uploads need the dialer role (none);
  `purchasePremiumCapability()` answers `FEATURE_NOT_SUPPORTED` (no slicing upsell; the
  foreground check before it is not modelled); slicing configuration answers
  `ERROR_MODEM_ERROR` to privileged callers; GBA answers `FEATURE_NOT_SUPPORTED`; network
  scans need `MODIFY_PHONE_STATE` and return `INVALID_SCAN_ID`.
- Emergency numbers: EmergencyNumberTracker's fallback when the radio reports none and
  its database has no entry: "112,911" with a SIM, "112,911,000,08,110,118,119,999" without
  one, plus libphonenumber's short numbers for the registered network's country without a
  SIM. Gesher ships no emergency number database, so country-specific numbers from it are
  missing.
- Subscriptions: no subscription groups or opportunistic subscriptions.
- The remaining `iphonesubinfo` getters apply PhoneSubInfoController's checks before
  answering: GID1/GID2, line 1 alpha tag, voicemail number and tag and IMEISV need
  `READ_PHONE_STATE` (null with the app op revoked), the NAI subscriber-identifier access,
  the MSISDN phone-number access. The Mobile Broadband API reports only the subscriber
  ID, ICCID and telephone numbers, so the MSISDN is the first telephone number and the
  others are null after the check.

`TelephonyCallback`/`PhoneStateListener` deliver, for phone 0: service state, signal
strength(s), data connection state and the (always idle) call state, initially and on
change; the values TelephonyRegistry gives a new phone listener: data activity
(`DATA_ACTIVITY_NONE`), cell location (null) and cell info (empty) under its location
checks, display info (LTE with a 5G NSA cell is overridden as `NR_NSA`), the active data
subscription, the user mobile data state and the data enabled state, with their changes.
Subscription listeners hear SIM changes. Other events are permission-checked as in AOSP and
then never delivered.

Mobile data is the WWAN IP interface, which `ConnectivityService` already reports as a
`TRANSPORT_CELLULAR` network.

## Unsupported (Android's own answers)

- Voice calls: no `FEATURE_TELEPHONY_CALLING`, `isVoiceCapable()` false, call state idle,
  no voice registration.
- SMS/MMS: no `FEATURE_TELEPHONY_MESSAGING`, no `isms` service; `SmsManager` send methods
  throw AOSP's `UnsupportedOperationException("Sms is not supported")`.
- Carrier configuration, carrier identification (`UNKNOWN_CARRIER_ID`), carrier
  privileges (`CARRIER_PRIVILEGE_STATUS_NO_ACCESS`), eUICC, IMS, cell information, network
  scans and selection (`NETWORK_SELECTION_MODE_UNKNOWN`), IMEISV, data activity
  (`DATA_ACTIVITY_NONE`) and GID1 are not backed by the modem; their answers are listed
  above.
- One modem only; a second WWAN interface is ignored.
- Telephony broadcasts (`ACTION_SIM_CARD_STATE_CHANGED`, service-state and
  default-subscription broadcasts) are not sent.

## Validation boundaries

- `tools/test_windows_cellular_native.py` compiles the unchanged native adapter against an
  explicit COM boundary under ASan/UBSan: absent service/interface, errors, apartments,
  exact UTF-8 fields, unreported fields, software and hardware radio states, bounds and that
  every BSTR, SAFEARRAY and COM reference is released. Three negative controls (leaked
  capability strings, uninitializing a foreign apartment, swapped radio states) must fail.
- `tools/test_telephony.py` runs the production Java services on a host JVM with synthetic
  modems and callers: features, translations, permission rules (including
  `LocationAccessPolicy` for listeners and `ServiceState`), redaction, listeners, SIM
  removal/insertion, a modem without a SIM (default slot and invalid phone IDs), radio
  switches, read failures, hung reads, demand-driven polling under a controlled clock and
  the no-modem path.
- `ManagerConstructionBatchTest` (ART, Windows CI) runs the real Mobile Broadband probe on a
  runner without a modem and checks the absence of telephony features and AOSP's no-phone
  answers through `TelephonyManager`.

No physical modem has been tested: the native adapter's interface declarations (including
`IMbnRadio`, `{DCCCCAB6-201F-4BBB-AAEE-338E368AF6FA}`) were checked against the Windows
SDK's `mbnapi.h` (10.0.26100.0) locally, but real WwanSvc behavior, access
rights for subscriber information, modem timing and the Windows cellular UI are
unverified until someone runs Gesher on a machine with a WWAN modem.
