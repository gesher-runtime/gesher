# Dynamic shortcuts and compatibility QA

PR 4 was merged as `6fa48216` after all 52 reported checks passed. It already
contains `d47ae428` with its original history. This follow-up incorporates
`4087e702` and `67d168c6`, retaining their original commits and correcting the
issues found during review and runtime testing.

## Dynamic shortcut publication and launch

Dynamic shortcuts now persist in the system profile and create Windows Start
menu links. Publication authenticates the attached Binder caller, package and
user. Saved payloads carry the installed publisher's code identity. Arbitrary
shortcut IDs become SHA-256 keys before entering host command arguments.
Labels cannot introduce path separators, and cleanup accepts only owned links
directly inside the English or Hebrew Gesher apps folder.

The host sends the opaque package/key pair to SystemServer. The server reads
the stored payload itself and retains the exact publisher object through
resolution and delivery. Package removal or replacement retires that launch
authority. Launches use the publisher's authority, and URI grant flags,
selectors, chooser intents and cross-package targets are rejected or removed.
Missing or disabled shortcuts fail without falling back to the main activity.
Removing a shortcut revokes its stored capability before Windows link cleanup.

The implementation supports set/add/update/push, disable/enable and removal,
with a 15-entry bound and bounded serialized payloads. Pinning and share-target
support remain unavailable. Icons currently use the application's existing
shell icon. The implementation launches the final shortcut intent; it does
not reconstruct an Android back stack from the preceding intents.

`DynamicShortcutTest` covers authenticated ownership, persistent state,
mutation isolation, unsafe intents, opaque arguments, limits, owned-link
validation, actual Unicode Windows link creation and publisher replacement
between capability lookup and activity resolution. The isolated Windows CLI
regression publishes to a real SystemServer and launches a synthetic app
through the new Binder transaction, checking onResume and its exit receipt.
This test caught and fixed an incorrect transaction-range upper bound.

## Compatibility changes retained after review

Wi-Fi containers serialize their exposed fields; logical lock tokens track
held/reference-count state without claiming hardware Wi-Fi control. Network
statistics containers can represent empty results, while unsupported real
network queries still fail. PackageInstaller resolves package names from
actual staged APK bytes, including valid files named `0`, rather than trusting
the session's package-name hint. Blob cleanup calls are idempotent when there
is no stored blob, while creation/access remain denied. Unmanaged device
policy reports default permission policy without claiming storage encryption.

The original broad compatibility patch also removed authentication and URI
checks, acknowledged unsupported administrative operations and called account
removal from a package-removal preflight. Those changes are excluded from the
resulting tree. Account preservation, notification attribution, provider URI
access, owner-bound URI grants, durable-grant restrictions, device-idle and
rollback denials remain in place. No user accounts were reset during QA.

## Validation

The final Java artifact and complete five-space boot-image chain passed all
71 offline microG integration checks, including real Binder package install,
shortcut launch, account persistence/restart and client-removal regressions.
The focused shortcut test passed 36 checks. Focused Wi-Fi (42), network
statistics (7), blob, device-idle and software-service contract tests also
passed. Translation/RTL validation and the selected TCP source policy passed.
The native executable is unchanged from the fully green PR 4 artifact.

## App verification boundaries

VLC's internal file picker was confirmed working by the user before this
follow-up. Claude sign-in and Gmail startup remain unresolved application
observations. Gemini subsequently worked when the user retried through the
explicitly authorized Google-only SSH CONNECT relay. The same runtime failed
through the local TCP-only relay, so this points to a local network-path
difference; the exact network/filtering cause is unproven. That per-launch
configuration is a local diagnostic and is not packaged as a product fix.

## Follow-up: empty routing Activity windows

The actual microG sign-in flow exposed a separate desktop-presentation defect:
an empty transparent routing Activity acquired its own native title bar only
32 pixels high alongside the real sign-in dialog. Transparent Activity decor
can have theme padding even when its content frame has no children.

WindowSession now retains that window's surface and Activity lifecycle while
withholding desktop presentation and initial focus. This applies only to a
base Activity window with a translucent theme, no action bar, a transparent
decor background and an empty content frame. Dialog windows and Activities
with content keep their existing behavior. The condition is rechecked before
showing, so adding actual content to the Activity reveals it normally.

The expanded private-desktop window-promotion fixture reproduces a dialog
shown from onCreate before its empty Activity's first frame. On the old runtime
the native controller observes two visible windows, including a 32-pixel base
window, and fails. On the corrected runtime it observes only the real dialog;
after adding Activity content, both windows are visible. It also checks actual
presented synthetic pixels. The existing service-first and explicit-headless
cases continue to pass. No account content is used or captured by the fixture.
The final rebuilt Java/five-space image tuple passed all 71 offline integration
checks and GraphicsTest, including the surface-padding/cropping regressions.

This removes the empty extra window. The sign-in card itself is a microG
BottomSheetDialog, intentionally anchored to the bottom of its Android view;
this change does not force application content into a different layout.
