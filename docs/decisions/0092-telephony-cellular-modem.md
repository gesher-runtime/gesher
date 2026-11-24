# 0092 — Telephony over a Windows cellular modem

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
2. A native adapter reads the modem through `IMbnInterfaceManager`. Its one write is the
   connection mode of the SIM's default connection profile, with the connect or
   disconnect that follows it, for Android's mobile data switches (no PIN, registration,
   radio or SMS control).
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
  permission checks, placeholder values (`SIM_STATE_UNKNOWN` included, ADR 0094), and the
  managers' `IllegalStateException` where they throw one. `telephony.registry` keeps AOSP's listener permission checks.
- With a modem, data-only telephony works for state, identifiers and listeners; SMS, voice,
  carrier configuration and cell info remain unsupported. Every ITelephony/ISub method a
  public manager API reaches answers consistently with the modem or with AOSP's unsupported
  answer (no Default answer contradicts the modem or leaves a callback unanswered); the
  mobile data and data roaming switches are Windows' connection behavior for the modem
  (the default profile's `ConnectionMode`: `manual` off, `auto` on with roaming,
  `auto-home` on without), read from Windows and mirrored into
  `Settings.Global.MOBILE_DATA` and `DATA_ROAMING`. AOSP's setters, with their permission
  checks, change Windows' cellular connection for the whole machine (disconnecting or
  connecting it) and report a change, and notify listeners, only once Windows has taken it;
  users change them with `gesher-app mobile-data`. The policy, carrier and thermal switches
  cannot be applied apart from that mode and stay on (docs/telephony.md, "Mobile data
  switches"). The write path is unvalidated on hardware.
- Reading the modem costs an out-of-process WwanSvc session, so it is demand-driven:
  polled every five seconds only while telephony.registry has listeners or for 30 seconds
  after a query, with a bounded read before answering from an old reading, and every read
  bounded by a timeout (docs/telephony.md).
- framework-res must be rebuilt for the overlay; the framework workflow's cache key
  includes the overlay directory.
- Validation is host fixtures, a native COM-boundary fixture and an ART no-modem check;
  physical-modem validation is outstanding.
