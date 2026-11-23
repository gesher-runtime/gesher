# F09: runtime permission denial and rationale

This round addresses the persisted user-choice and requestability part of
[issue #56](https://github.com/gesher-runtime/gesher/issues/56). It does not close
the broad F09 requirement.

## Contract and implementation

Before this change, the host recorded successful grants but discarded an
explicit Deny, `shouldShowRequestPermissionRationale` always returned false,
and the consent controller could prompt for user-, policy-, or system-fixed
permissions. Applications could neither distinguish a first denial from an
untouched permission nor stop an endless request loop.

The host dialog now returns three distinct outcomes. Enter on its default Deny
button is an explicit denial. Escape, window close, failure to create the dialog,
and an unavailable native library leave user-choice history unchanged. Other
host consent consumers retain their boolean interface and accept only Allow.

| Current state / choice | Permission result | User-choice flags | Rationale |
| --- | --- | --- | --- |
| Untouched / first explicit Deny | Denied | `USER_SET` | True |
| Previously set foreground or background subgroup / explicit Deny | Denied | `USER_SET`, `USER_FIXED` | False |
| Requestable / explicit Allow | Granted | `USER_SET` | False |
| User-, policy-, or system-fixed denial / request | Denied without a prompt | Unchanged | False |
| Any denied state / dismissal or unavailable UI | Denied | Unchanged | Unchanged |

Rationale queries authenticate the calling package/UID, allow same-UID siblings,
reject unknown callers/users/devices, and suppress granted, unknown, non-runtime,
fixed, and hard-restricted permissions. Soft restriction remains distinct from
hard denial: permission consent does not remove its AppOps restriction. The
Android background-location rationale change applies from target SDK 30.

Logical platform groups come from Android 16 PermissionController's
`PermissionMapping`, not `PermissionInfo.group`: the framework manifest exposes
the same raw `android.permission-group.UNDEFINED` value for camera, contacts,
microphone, and other unrelated permissions. That placeholder cannot share
denial history. Foreground and background decisions use separate histories
within a logical group. The background subgroup follows the corresponding
foreground definitions' `backgroundPermission` links. Valid custom groups
retain both their declared name and their defining package identity. `RuntimePermissionGroups` adapts AOSP's mapping and
retains its Apache-2.0 notice.

The public request-state query and host-dialog eligibility remain distinct. A
target-22 app requesting the runtime-only `ANSWER_PHONE_CALLS` permission with
no fixed flags gets Android's `REQUESTABLE` query result. It still receives no
modern consent dialog or permission grant from this controller.

An explicit decision is committed to the existing UID permission ledger under
the package-manager and policy monitors. Group history is captured before any
permission is changed, so multiple names or duplicates in one prompt cannot
become two denials. A package replacement/removal, newly fixed permission, or
concurrent grant invalidates the affected pending decision. Persistence,
cache invalidation, and AppOps publication use the existing mechanisms.

## Primary contract evidence

The policy was checked against AOSP tag `android-16.0.0_r4`:

- [PermissionService.kt](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/permission/java/com/android/server/permission/access/permission/PermissionService.kt):
  `getPermissionRequestState`, `shouldShowRequestPermissionRationale`,
  `UNREQUESTABLE_MASK`, and `BACKGROUND_RATIONALE_CHANGE_ID`.
- [PermissionFlags.kt](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/permission/java/com/android/server/permission/access/permission/PermissionFlags.kt):
  hard `RESTRICTION_REVOKED` versus `SOFT_RESTRICTED` and public flag conversion.
- [BasicGrantBehavior.kt](https://android.googlesource.com/platform/packages/modules/Permission/+/android-16.0.0_r4/PermissionController/src/com/android/permissioncontroller/permission/ui/model/grantPermissions/BasicGrantBehavior.kt):
  a previously user-set group selects the fixed-denial action.
- [PermissionMapping.kt](https://android.googlesource.com/platform/packages/modules/Permission/+/android-16.0.0_r4/PermissionController/src/com/android/permissioncontroller/permission/utils/PermissionMapping.kt):
  canonical platform permission groups and declared-group fallback; source SHA-256
  `79fc715c67c77d9f0373d8c08ee5082c6f8e760d6a5af19227dbb6c16f46e235`.
- [Framework AndroidManifest.xml](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/res/AndroidManifest.xml):
  raw permission groups, protection levels, flags, and background permissions used
  at the host metadata boundary; source SHA-256
  `201d8390999de22fc6363a40ffdda84c2ed1a4cbd3c4f18cab6d721b505797a9`.
- [AppIdPermissionPolicy.kt](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/permission/java/com/android/server/permission/access/permission/AppIdPermissionPolicy.kt):
  legacy runtime-only permissions remain denied without acquiring fixed flags.
- [LightAppPermGroup.kt](https://android.googlesource.com/platform/packages/modules/Permission/+/android-16.0.0_r4/PermissionController/src/com/android/permissioncontroller/permission/model/livedatatypes/LightAppPermGroup.kt)
  and [KotlinUtils.kt](https://android.googlesource.com/platform/packages/modules/Permission/+/android-16.0.0_r4/PermissionController/src/com/android/permissioncontroller/permission/utils/KotlinUtils.kt):
  group history and grant/revoke user-choice flags.

## Reproducible checks and limits

`tools/test_runtime_permission_state.py` runs the production controller, policy,
UID store, AppOps, and caller authentication on a host JVM. Its APK metadata and
native identity boundary are explicit fixtures. The same
`RuntimePermissionStateTest` is registered in Windows track 94 with generated
`IPermissionManager` Binder Parcel transport and the real framework permission
catalog. Coverage includes persistence, group decisions, duplicates, overlapping
requests, UID isolation, stale APK generations, changed policy, concurrent grants,
and target-SDK background rationale. The suite explicitly checks raw framework
`UNDEFINED` metadata, unrelated camera/contacts/microphone decisions, same-group
READ_CONTACTS/WRITE_CONTACTS decisions, custom groups read through production
`PermissionCatalog`, and legacy runtime-only request state without a dialog.

`tests/native/permission_dialog_test.c`, already registered in Windows track 99,
checks native Allow, Deny, Enter, Escape, window close, and Tab-to-Allow in both
supported UI locales on a private test desktop. The account crypto DLL and this
test must both compile with `-Wall -Wextra -Werror`. The existing AppOps contract
suite remains a regression gate for grants, revocation, publication and lifetime.

Host tests and cross-compilation do not establish Windows execution or Android
differential parity. The Windows CI result must be reported separately. No
physical-device or Android permission-controller differential run is claimed.
Split-permission grants, full one-time/reset lifecycle, specialized permission
group prompts (including notification/selected-media/background-access flows),
and activity/process-driven cancellation remain separate F09 work.
