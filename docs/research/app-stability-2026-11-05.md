# App stability investigation, 2026-11-05

These changes address observed application failures. They do not establish
complete Android API coverage or overall release readiness. The installed
shared Gesher device and existing apps were not stopped or restarted.

## VLC 3.7.2 (ARM64)

The startup abort was captured under LLDB at the ELF loader's unresolved
function handler: the missing function was `tsearch`. Supplying the search
family exposed a second abort at `__pthread_cleanup_push`.

The shared bionic shim now supplies an AVL search tree family and bionic's
LP64 pthread cleanup records, with per-thread LIFO execution before TLS/key
destructors. Regression checks in `libc_test.c` run through the existing x86-64
and translated ARM64 tracks. Focused Windows System V checks pass, and both
NDK test libraries compile. An isolated ARM64 preload using the same source
lets the real VLC APK draw its first frame and survive a 20-second observation.
This preload is diagnosis, not an installed-runtime update or playback proof.
The rebuilt native runtime still requires integrated tracks 40 and 47.

## OsmAnd 5.4.9

Startup failed because its external-sensors plugin constructs a BLE callback
before checking for a Bluetooth adapter. Real pinned AOSP callback, scan
result, advertisement and parcel value types replace the missing types while
the adapter remains unavailable. The existing device-configuration answer is
included because the installed server's null answer was another startup
blocker. Debug's Binder object counters use the already-built AOSP native
implementations; unavailable transaction statistics return -1.

The BLE regression passes in real ART. With the BLE changes and an app-only
proxy mirroring the source device-configuration answer, OsmAnd reaches
MapActivity and renders its bundled native basemap for 15 seconds. The new
Binder native registrations require the rebuilt host. See
[the detailed report](osmand-startup-stability-2026-11-05.md).

## Gmail: Manage notifications

The observed fatal exception is `ActivityNotFoundException` for
`android.settings.CHANNEL_NOTIFICATION_SETTINGS`. With no Android Settings
APK installed, the activity starter rejected that intent.

The fallback now delegates app/channel notification-settings actions to the
documented Windows `ms-settings:notifications` page, which controls the host
toasts. It uses a fixed URI, never package/channel extra values as shell input.
Individual Android channel editing is not implemented by that Windows page.
Foreign explicit targets and unsupported settings actions remain unresolved,
and a failed Windows launch is not reported as successful.

`NotificationSettingsIntentTest` passes in a separate real ART process with
the Windows opener mocked. The existing `BrowserIntentTest` also passes with
the changed starter. Both are wired into track 95. Live Gmail verification
requires updating the shared server after the user's permission.

## Telegram native networking

The x86-64 `libtmessages.49.so` mapping and four initialized native network
workers were verified in the live process. A bounded trace after the user's
proxy check observed `socket`, `connect`, `epoll_ctl`, `epoll_wait` and
`eventfd_write`, an established TCP connection to the configured local SOCKS5
tunnel, a Telegram pong and API response. The login window subsequently
advanced to "Choose a login email". No phone number or code was entered by
the agent. Temporary diagnostics were restored and the debugger detached.

No causal networking shim change is justified by these observations. In
particular, the successful installed binary predates commit `657fb27a`; this
is not verification of that commit. A new native regression covers a worker
thread, eventfd wakeups, opaque epoll cookies, nonblocking TCP and a synthetic
SOCKS5/PING/PONG loopback exchange across four bounded cycles. It passes on
the installed runtime in an isolated ART process and joins `libc_test.c`.

The user's inability to foreground the subsequent login window remains a
separate open investigation; successful network activity does not close it.

## Integration scope

The isolated candidate branch is based on `6b69e2af`. It contains only this
investigation's changes and the required device-configuration answer; unrelated
local microG/account/process-authentication changes are excluded. A complete
CI build and runtime integration remain required before changing the installed
runtime. Diagnostic logs are local under `out/` and are not committed.
