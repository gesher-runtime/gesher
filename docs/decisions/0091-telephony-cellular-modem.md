# 0091 — Telephony over a Windows cellular modem

## Status

Proposed.

## Context

Gesher served `phone`, `iphonesubinfo`, `isub` and `telephony.registry` as Default
stand-ins: no SIM, no phone, no permission checks. framework-res kept AOSP's phone defaults
(`config_voice_capable`, `config_sms_capable`, `config_mobile_data_capable` all true), and
with a vendor API level below V `TelephonyManager` uses those resources when a telephony
feature is absent, so every app saw a voice-capable GSM phone (`getPhoneType()` GSM,
`getActiveModemCount()` 1) on machines without any cellular hardware. Some Windows
machines have a WWAN modem that Windows exposes through the Mobile Broadband API.

## Decision

1. A framework-res device overlay (`config/framework-res-overlay`, applied by
   `tools/build-framework-res.sh`) sets the three capability resources to false, as
   Android tablets' device overlays do. Capability then follows declared features only.
2. A read-only native adapter reads the modem through `IMbnInterfaceManager` (no writes,
   no connection control, no SMS).
3. `TelephonyService` implements the four services over one modem model (phone 0) with
   AOSP's permission and identifier rules, and publishes the `gsm.*` properties the phone
   process publishes on Android.
4. Features telephony/subscription/radio_access/data (+gsm) are declared only for a modem
   present at boot. Calling and messaging are never declared; SMS and voice keep Android's
   unsupported behavior.

## Consequences

- Without a modem, apps see a device without telephony (no features, no phones,
  `PHONE_TYPE_NONE`). `phone`, `iphonesubinfo` and `isub` answer as AOSP's managers do
  when those services do not exist (AOSP publishes them only with `FEATURE_TELEPHONY`): no
  permission checks, placeholder values, and the managers' `IllegalStateException` where
  they throw one. `telephony.registry` keeps AOSP's listener permission checks.
- With a modem, data-only telephony works for state, identifiers and listeners; SMS, voice,
  carrier configuration and cell info remain unsupported and are documented in
  docs/telephony.md.
- framework-res must be rebuilt for the overlay; the framework workflow's cache key
  includes the overlay directory.
- Validation is host fixtures, a native COM-boundary fixture and an ART no-modem check;
  physical-modem validation is outstanding.
