# Clipboard access contracts

The clipboard policy follows the Android 16 AOSP
[ClipboardService at 45034f0663f9](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/core/java/com/android/server/clipboard/ClipboardService.java):

- Package ownership precedes privilege checks. Gesher also authenticates the
  package against the calling process's admitted Binder UID.
- Background reads require the granted `READ_CLIPBOARD_IN_BACKGROUND` permission
  or the package configured in secure `DEFAULT_INPUT_METHOD`; other reads need
  current Windows input focus. All paths then require an allowed AppOps mode.
- Data reads and callbacks note an attributed read; metadata probes check the
  mode. Device lock gates all getters. AOSP callbacks recheck their saved identity
  and AppOps without applying the getter's lock check.
- Background writes remain possible subject to write AppOps. A clear of an
  already empty clipboard does not send a second change notification.
- With one user and no VirtualDeviceManager, AOSP maps requested user/device IDs
  to the caller/default clipboard.

The foreground observation comes from
[GetForegroundWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getforegroundwindow),
its window thread/process, and the held process object's creation FILETIME. The
server accepts it only for a current ready process and its current installed
package generation. A resumed Android task alone does not grant access.
[WTS session flags](https://learn.microsoft.com/en-us/windows/win32/api/wtsapi32/ns-wtsapi32-wtsinfoex_level1_w)
provide the actual lock state on the supported Windows 10+ host; unknown state
does not authorize a read.

## Validation boundaries

`tools/test_clipboard.py` executes the production PM, ATM, Settings,
PermissionPolicy, AppOps and Clipboard services. APK metadata, admitted process
records, and the instance-owned native observation interface are constructed
fixtures. It runs with optional GMS integration both disabled and enabled, so
secure IME configuration and caller ownership cannot depend on that mode.
Callbacks use the real framework RemoteCallbackList, including reentry,
concurrent broadcasts, token death, package retirement and close ownership.
The existing `tools/test_pending_intent_callbacks.py --case clipboard` invokes
this complete fixture too.

`tools/test_clipboard_native.py` compiles the production native access code with
explicit Windows API boundaries. It checks failed/transient observations,
process identity, session flags and handle/buffer cleanup. The separate
`Clipboard native access` Windows CI job compiles against real SDK headers and
runs actual window activation/loss, process generation, WTS and resource checks.

The Windows ART framework track runs both Java test mains through the generated
Binder clipboard transport. `ClipboardCallbackTest` additionally uses the public
production constructor, real Windows clipboard writes/clear and native change
events. This gate must pass for the published commit; host fixtures do not stand
in for native Windows/ART execution. Locked/unlocked transition branches are
covered by the explicit native observation fixture; the Windows job observes
its real session without locking the CI worker.

A written native sequence is captured before releasing Windows' clipboard,
so a following external change cannot inherit Android's source attribution.
The service owns and releases its native subscription, package subscriptions,
callback registrations and current imported-image cache file. Content-provider
URIs remain provider URIs; their path is never interpreted as a local image
file. Direct image transfer accepts BMP data, matching the native DIB bridge.
