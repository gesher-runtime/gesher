# 0094: SIM state without a cellular modem follows AOSP (UNKNOWN)

Status: accepted, 2026-11-23 (maintainer decision). Supersedes the SIM state part of
ADR 0087 (decision 5, `SIM_STATE_ABSENT`); the rest of ADR 0087 stands.

## Context

ADR 0087 replaced `ITelephony.Default`'s 0 (`SIM_STATE_UNKNOWN`) from
`getSimStateForSlotIndex` with `SIM_STATE_ABSENT`, reasoning that apps read UNKNOWN as
"still loading" and wait on it. ADR 0092 later made telephony follow a real Windows
cellular modem: without one, Gesher reports no telephony features, and `phone`,
`iphonesubinfo` and `isub` answer as AOSP's managers do when those services do not
exist. The SIM state was the one answer left different (docs/telephony.md).

AOSP (android-16.0.0_r4) on a device without `FEATURE_TELEPHONY` does not publish `phone`
or `isub` (`PhoneGlobals.onCreate` returns early), and then:

- `TelephonyManager.getSimStateForSlotIndex(slot)` (static, behind `getSimState(slot)`,
  `getSimApplicationState(physicalSlot[, port])` and `SubscriptionManager.getSimStateForSlotIndex`)
  returns `SIM_STATE_UNKNOWN` when `getITelephony()` is null.
- `getSimState()` calls `getSlotIndex()`: `SubscriptionManager.getSlotIndex(getSubId())` gives
  its cache default `INVALID_SIM_SLOT_INDEX` (-1) when `ISub` is null, which equals
  `SIM_NOT_INSERTED` (`Telephony.SimInfo`, -1), so `getSlotIndex()` returns
  `DEFAULT_SIM_SLOT_INDEX`. That index is not negative, so `getSimStateIncludingLoaded()`
  calls `getSimStateForSlotIndex(DEFAULT_SIM_SLOT_INDEX)`: `SIM_STATE_UNKNOWN`. The
  "all slots absent" loop (which would give ABSENT for zero phones) is not reached.
- `getSimApplicationState()` and `getSimCardState()` map UNKNOWN to UNKNOWN.

## Decision

The maintainer decided to follow AOSP. Without a cellular modem, `ITelephony.getSimStateForSlotIndex`
answers `SIM_STATE_UNKNOWN` for every slot, so `getSimState()`, `getSimState(slot)` and
the states derived from them are UNKNOWN, as on an Android device without telephony.

With a modem, the state stays the modem's (`WindowsCellular.simState`): `ABSENT` when the
modem reports no SIM (`SIM_NOT_INSERTED`, `NO_ESIM_PROFILE`), and UNKNOWN only for a slot
that is not phone 0 or a negative index.

## Consequences

- An app that waits for the SIM state to leave UNKNOWN waits on Gesher without a modem as
  it does on a Wi-Fi-only tablet; such apps should check `FEATURE_TELEPHONY` (absent) or
  `getPhoneCount()` (0) first, as they must on Android.
- No `gsm.sim.state` property is written without a modem (unchanged).
- Tests: `tests/telephony/TelephonyHost.java` (no-modem slots 0 and DEFAULT_SIM_SLOT_INDEX),
  `ManagerConstructionBatchTest` (`getSimState()` and `getSimState(0)` through the real
  managers and service), `SettingsIntentsTest` (the service without a modem).
