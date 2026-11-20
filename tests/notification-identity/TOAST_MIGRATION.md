# Windows toast identity migration

Windows' [ToastNotification.Tag](https://learn.microsoft.com/en-us/uwp/api/windows.ui.notifications.toastnotification.tag?view=winrt-26100)
supports 64 characters on Windows 10 Creators Update and later. The old Gesher
mapping shortened long Android tags with Java `hashCode`, so distinct tags such
as `"Aa".repeat(40)` and `"BB".repeat(40)` replaced each other's Windows toasts.
A shortened tag could also collide with an ordinary short literal tag.

Canonical persisted identities use a 64-character SHA-256 digest of
the ID and the service/platform's existing tag-key equivalence. The Android key,
application tag, content, permissions and callback workflows are unchanged.
In particular, actual `StatusBarNotification.getKey()` treats a null tag and the
literal `"null"` as equivalent; this change preserves that existing equivalence.
New hex-only identities are disjoint from legacy `id|tag` and decimal-ID names.

At the Windows boundary, `windowsToastTag` encodes all 32 digest bytes as
unpadded Base64url with a `sha256-` prefix (50 characters total). This representation
retains every bit, including leading zeroes, and its prefix is disjoint from both
canonical hex and legacy names. The adapter rejects noncanonical input. Actual
Windows Server 2025 diagnostics showed a 64-character Tag setter returning S_OK
while its immediate getter returned E_INVALIDARG; the same 64-character input
round-tripped on Server 2022, and 63/32/16-character inputs round-tripped on both.
This measured boundary behavior motivates the representation; it is not a
truncated digest or an assumed 16-character platform limit.

The store format, canonical digest, Android keys and XML activation payloads stay
unchanged. New posts use the encoded physical Tag. After a successful post, the
service retires the old raw-hex spelling through per-tag removal; cancellation
tries both spellings and preserves any removal error. Native kept handles use
the same physical Tag as Show. Legacy decimal/id|tag aliases continue to require
the XML owner check below. No migration operation clears unrelated notifications.

The standalone toast branch's store version4 records the current Windows identity
and an optional legacy alias. The combined delegation/toast writer now uses
explicitly marked version5; its reader distinguishes both historical v4 layouts
as described in [the integration rules](../../docs/verification/notification-persistence-integration-2026-11-18.md).
Versions 1–3 remain readable with their original Windows identity. Reposting keeps
legacy cleanup metadata until the old toast is proven absent or removed. History
API failures and owner mismatches preserve the alias, allowing another attempt
and cancellation after a restart. Both legacy null-tag spellings can be retired,
each subject to the same owner check.

A saved record alone cannot prove who owns an old Windows identity: persistence
precedes showing the toast, which can fail, and two legacy records may share one
old Windows tag. The new native `removeToastIfLaunch` therefore obtains history
for the exact app ID, matches tag/group, reads the root XML `launch` attribute and
requires the full `n|<Android notification key>` before removing a legacy identity.
An owner mismatch returns S_FALSE; API/XML/string-allocation failures propagate.
No unknown owner is removed. This process serializes show, remove, clear and
query/remove operations; no cross-process conditional-removal atomicity claim is
made for independent processes manipulating the same Windows app ID.
The C++ boundary rejects embedded U+0000 before NUL-terminated C conversion, so
an expected full key cannot silently become a shorter owner's identity.

Cancellation, retained activation auto-cancel, channel removal, cancel-all of
active records and repost cleanup use the common identity-aware removal paths.
Package removal and permission revocation still clear the app's entire toast
collection. Shell fallback and the existing Android lifecycle rules are preserved.

## Validation

The results below describe the standalone PR145 tree. The combined tree retains
these original assertions and adds the format/ownership corpus, but its actual
Windows execution remains pending. Prior-branch Windows results do not qualify it.

- `tools/test_notification_identity.py`: actual production record and service
  methods, with explicit host Parcel serialization and Windows history seams;
  49 checks across separate JVM processes. The added 22 checks cover long-tag and
  short-tag collisions, legacy owner mismatch/failure, repost retirement, null-tag
  equivalent updates, and restart cancellation.
- `tools/test_notification_identity_art.py`: actual Windows ART, Android Parcel,
  Bitmap and PNG paths, plus the same explicit history/removal boundary; 43 checks
  across two VMs. No real Windows notification is shown or removed.
- `tools/test_toast_history_identity.py --cc <llvm-mingw clang>`: 46 native checks
  compile the actual production C implementation. COM history/vector/XML and final
  removal are controlled boundaries. Matching/absent/wrong owners, intermediate
  API failures, removal failures and COM reference cleanup are covered. Actual
  final-removal HSTRING failures are injected before any history call. The actual
  C++ argument validator rejects embedded NUL in each boundary argument.
- The changed C++ JNI/Win32 wrappers pass syntax checks using the existing native
  build's production includes, definitions and warnings-as-errors flags. A full
  clean runtime rebuild/link and live Windows toast-history integration were not
  performed locally. CI builds the runtime and executes the separate COM fixture.

For a negative control, pass `--tag-only --source-ref ae069416` to either Java
runner with the usual compile/runtime inputs. Unchanged production source fails
with `Distinct long Android tags alias the same Windows toast identity`.
Without the source control, `--tag-only` tests the current implementation.
