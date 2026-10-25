// Gesher: the Windows side of the device-state system services (devsvc track,
// docs/services.md "Device services", docs/decisions/0038-devsvc-windows-backed-services.md):
// the system server's alarm, job scheduler, clipboard, notification and power
// services keep Android's semantics in Java (com.gesher.server) and ask
// Windows through these functions. The JNI side is devsvc_natives.cc
// (com.gesher.server.WindowsHost).
#pragma once

#include <cstdint>
#include <string>

namespace gesher {
namespace devsvc {

// What the event window saw since the last WaitEvents (a bit set).
enum Event : unsigned {
  kClipboard = 1u << 0,       // WM_CLIPBOARDUPDATE (AddClipboardFormatListener)
  kTimeChanged = 1u << 1,     // WM_TIMECHANGE: the clock or the time zone was set
  kResumed = 1u << 2,         // WM_POWERBROADCAST PBT_APMRESUMEAUTOMATIC/RESUMESUSPEND
  kPowerStatus = 1u << 3,     // WM_POWERBROADCAST PBT_APMPOWERSTATUSCHANGE (AC, battery)
  kSuspending = 1u << 4,      // WM_POWERBROADCAST PBT_APMSUSPEND
  kSettingChanged = 1u << 5,  // WM_SETTINGCHANGE (time zone, regional settings...)
  kToastActivated = 1u << 6,  // a toast this process showed was clicked (see PopActivation)
};

// Starts the event thread: a hidden top-level window (message-only windows get
// no broadcasts such as WM_TIMECHANGE and WM_POWERBROADCAST) listening to the
// clipboard. Idempotent; returns the window's HWND as an integer (the tests
// send it WM_TIMECHANGE by handle: FindWindow from another window station or
// desktop does not see it), 0 if the window could not be made.
unsigned long long StartEvents();

// Waits up to `timeout_ms` (negative: forever) for events; returns and clears
// the set that arrived (0 on a time-out).
unsigned WaitEvents(int timeout_ms);

// The oldest toast activation not yet taken: the app id, tag and group of the
// toast and the arguments of what was clicked. False when there is none.
bool PopActivation(std::u16string* app_id, std::u16string* tag, std::u16string* group,
                   std::u16string* arguments);

// --- clipboard -----------------------------------------------------------------------
// CF_UNICODETEXT; false if the clipboard has no text (or could not be opened).
bool ClipboardText(std::u16string* text);
// The fragment of the "HTML Format" (CF_HTML) data, as UTF-16; false if none.
bool ClipboardHtml(std::u16string* html);
// Replaces the clipboard's contents with `text` (CF_UNICODETEXT) and, when not
// empty, `html` as CF_HTML; the event window is the owner. Empty text and html
// just empty the clipboard.
bool SetClipboard(const std::u16string& text, const std::u16string& html);
// GetClipboardSequenceNumber.
uint32_t ClipboardSequence();

// --- alarms ----------------------------------------------------------------------------
// Waits until the wall clock reaches `utc_ms` (milliseconds since 1970, a
// waitable timer: it follows clock changes and fires after a resume if the time
// passed while Windows slept; with `wakeup` it may wake Windows from sleep, if
// wake timers are allowed), until AlarmKick, or for at most `timeout_ms`.
// Returns 0 (the time came), 1 (kicked) or 2 (time-out).
int AlarmWait(int64_t utc_ms, bool wakeup, int timeout_ms);
void AlarmKick();

// --- power ------------------------------------------------------------------------------
// GetSystemPowerStatus: AC line status (0 off, 1 on, 255 unknown), battery flag
// (bits: 1 high, 2 low, 4 critical, 8 charging, 128 no battery; 255 unknown),
// battery percent (255 unknown), system status flag (1: battery saver on).
bool PowerStatus(int out[4]);
// A power request for the process (PowerCreateRequest/PowerSetRequest, shown by
// `powercfg /requests` with `reason`): `system` keeps Windows from sleeping,
// `display` the display on. Both false clears it.
bool KeepAwake(const std::u16string& reason, bool system, bool display);
// Milliseconds since the user's last input (GetLastInputInfo).
uint32_t IdleMillis();
// The network list manager's view: bit 0 connected, bit 1 internet, bit 2 the
// internet connection is metered (not unrestricted). -1 if it cannot tell.
int NetworkState();

// --- time zone --------------------------------------------------------------------------
// The Windows time zone's key name ("Israel Standard Time"); empty if unknown.
std::u16string TimeZoneKeyName();

// --- toasts -----------------------------------------------------------------------------
// Shows the toast `xml` for the AppUserModelID `app_id` with `tag` and `group`
// (one replacing an earlier with the same tag and group): its popup suppressed
// with `suppress_popup`, high priority with `high`. Clicks come back as
// kToastActivated events while this process runs. Returns S_OK or the HRESULT
// that failed.
long ShowToast(const std::u16string& app_id, const std::u16string& tag,
               const std::u16string& group, const std::u16string& xml, bool suppress_popup,
               bool high);
// Removes the toast from the screen and from the notification center.
long RemoveToast(const std::u16string& app_id, const std::u16string& tag,
                 const std::u16string& group);
// Removes every toast of `app_id`.
long ClearToasts(const std::u16string& app_id);
// HKCU\Software\Classes\AppUserModelId\<app_id>: DisplayName and IconUri, which
// name and picture the app's toasts when it has no Start menu shortcut (an
// empty `icon` leaves IconUri out).
long RegisterAppId(const std::u16string& app_id, const std::u16string& display_name,
                   const std::u16string& icon);
long UnregisterAppId(const std::u16string& app_id);

// Gesher's toast activator (ADR 0051, devsvc_activator.c): this process's class
// object for it, single use (a launcher started by COM) or for every click (the
// system server); clicks come back as kToastActivated events. RegisterAppId
// names it as the app id's CustomActivator when it is registered as a local
// server (HKCU or HKLM Software\Classes\CLSID\{...}\LocalServer32: the installer).
extern const char16_t kToastActivatorClsid[];
long RegisterActivator(bool single_use);
void RevokeActivator();

// GetFullPathNameW: a drive-less path ("\\a\\b") made absolute on the current drive.
std::u16string FullPath(const std::u16string& path);

}  // namespace devsvc
}  // namespace gesher
