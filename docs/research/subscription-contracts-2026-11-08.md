# Absent subscription query contracts, 2026-11-08

## Finding

`DeviceServices.SubscriptionService` inherited every `ISub.Default` answer.
Its comment promised invalid subscription IDs on a device with no SIMs, but
the generated integer methods returned `0`. Android treats `0` as a valid
subscription identifier: `SubscriptionManager.isValidSubscriptionId` accepts
every value greater than `INVALID_SUBSCRIPTION_ID` (`-1`). The service therefore
reported a default/active subscription while its subscription inventory was
empty. An app checking the public validity predicate could enter initialization
that assumes a SIM subscription exists. This is a confirmed query-contract
defect, not a claim that a particular APK crash was reproduced.

## Correction

The absent-subscription service now explicitly returns:

| Query | Result without subscriptions |
| --- | --- |
| System, data, voice and SMS defaults, including user-specific entry points | `INVALID_SUBSCRIPTION_ID` |
| Active data and preferred data without a PhoneSwitcher backend | `INVALID_SUBSCRIPTION_ID` |
| Subscription from a SIM slot | `INVALID_SUBSCRIPTION_ID` |
| SIM slot from a subscription | `INVALID_SIM_SLOT_INDEX` |
| Active subscription identifiers, both visibility variants | Empty non-null `int[]` |
| Enabled subscription in a valid SIM slot | `INVALID_SUBSCRIPTION_ID`; an invalid slot raises `IllegalArgumentException` |

The phone-index mapping has a distinct AOSP rule: an invalid subscription, or
the default subscription when none exists, maps to `INVALID_PHONE_INDEX`.
A numerically valid but unmapped subscription maps to `DEFAULT_PHONE_INDEX`
(`Integer.MAX_VALUE`). Returning `0` or replacing every case with `-1` would
lose this distinction.

No subscriptions, modem support, SIM identities or successful telephony
operations are invented. Full radio services, telephony state callbacks and
subscription mutation remain separate backend work.

## Reference and validation

The comparison uses Android tag `android-16.0.0_r4`:

- [SubscriptionManagerService](https://android.googlesource.com/platform/frameworks/opt/telephony/+/refs/tags/android-16.0.0_r4/src/java/com/android/internal/telephony/subscription/SubscriptionManagerService.java): default-ID initialization, `getSlotIndex`, `getSubId`, `getPhoneId`, `getActiveSubIdList`, `getEnabledSubscriptionId`, and the active/preferred data fallback without PhoneSwitcher. Source SHA256: `e876350a79d8c287854b81e892ee0aff4d62743f46007a35ff346495cca42f52`.
- [SubscriptionManager](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/telephony/java/android/telephony/SubscriptionManager.java): validity predicates, index constants, and actual public wrapper calls. Source SHA256: `b4f28fd3481e6246b40825b7bd9bd831036f3b324cdcccb419e7eb67e19b3433`.

`SubscriptionContractTest` constructs the production service and exercises 26
identifier, user, mapping and array assertions. It runs on the host with the
actual Android framework/core classes and no replacement Android classes or
native-method shims. The corrected service passes all 26 assertions; the same
test against the baseline fails immediately because the default ID is `0`.
A diagnostic probe of all 26 cases observed all 26 baseline mismatches.

`ManagerConstructionBatchTest` now also constructs the real
`SubscriptionManager` from `ContextImpl`, queries the public default/active
ID wrappers, checks the two active-ID views and the phone/slot mapping, and
checks enabled-subscription validation. It reuses the service assertions
through the generated service Binder. Track 50's expected construction count
is updated from 17 to 18 managers.

The changed production source and both fixtures compile with JDK 21 against
the real Android 16 framework/core artifacts. The host cannot run the complete
manager fixture: `TelephonyManager` class initialization requires Android JNI
for process identity and system properties. The Windows ART fixture remains
the integration test for those wrappers and was not executed locally.
