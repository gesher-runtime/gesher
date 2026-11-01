/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "notifications.h"

#include <jni.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

// devsvc_toast / devsvc_win32 exports
namespace gesher {
namespace devsvc {
long ShowToast(const std::u16string& app_id, const std::u16string& tag,
               const std::u16string& group, const std::u16string& xml,
               bool suppress_popup, bool high);
long RemoveToast(const std::u16string& app_id, const std::u16string& tag,
                 const std::u16string& group);
}  // namespace devsvc
}  // namespace gesher

namespace gesher {
namespace windows {
namespace {

const wchar_t* W(const std::u16string& s) {
  return reinterpret_cast<const wchar_t*>(s.c_str());
}

std::u16string EscapeXml(const std::u16string& s) {
  std::u16string out;
  out.reserve(s.size());
  for (char16_t c : s) {
    switch (c) {
      case u'&': out += u"&amp;"; break;
      case u'<': out += u"&lt;"; break;
      case u'>': out += u"&gt;"; break;
      case u'"': out += u"&quot;"; break;
      case u'\'': out += u"&apos;"; break;
      default: out += c; break;
    }
  }
  return out;
}

std::u16string BuildToastXml(const NotificationRequest& req) {
  std::u16string xml = u"<toast><visual><binding template=\"ToastGeneric\">";
  if (!req.title.empty()) {
    xml += u"<text id=\"1\">" + EscapeXml(req.title) + u"</text>";
  }
  if (!req.text.empty()) {
    xml += u"<text id=\"2\">" + EscapeXml(req.text) + u"</text>";
  }
  if (!req.icon_path.empty()) {
    xml += u"<image placement=\"appLogoOverride\" hint-crop=\"circle\" src=\"" +
           EscapeXml(req.icon_path) + u"\"/>";
  }
  xml += u"</binding></visual></toast>";
  return xml;
}

std::mutex g_tray_lock;
HWND g_tray_window = nullptr;
std::map<int, HICON> g_tray_icons;  // guarded by g_tray_lock

LRESULT CALLBACK TrayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND GetTrayHelperWindow() {
  std::lock_guard<std::mutex> lock(g_tray_lock);
  if (g_tray_window != nullptr) return g_tray_window;

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = TrayProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"GesherTrayNotificationHelper";
  RegisterClassExW(&wc);

  g_tray_window = CreateWindowExW(0, wc.lpszClassName, L"GesherTrayHelper",
                                  0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                  wc.hInstance, nullptr);
  return g_tray_window;
}

}  // namespace

long ShowWinRTToastNotification(const NotificationRequest& req) {
  std::u16string xml = BuildToastXml(req);
  return devsvc::ShowToast(req.app_id, req.tag, req.group, xml,
                           req.suppress_popup, req.high_priority);
}

long ShowShellBalloonNotification(const NotificationRequest& req) {
  HWND hwnd = GetTrayHelperWindow();
  if (hwnd == nullptr) return E_FAIL;

  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(NOTIFYICONDATAW);
  nid.hWnd = hwnd;
  nid.uID = static_cast<UINT>(req.id);
  nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_INFO;
  nid.uCallbackMessage = WM_USER + 0x200;

  HICON icon = nullptr;
  bool custom_icon = false;
  if (!req.icon_path.empty()) {
    icon = static_cast<HICON>(LoadImageW(
        nullptr, W(req.icon_path), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_LOADFROMFILE));
    if (icon != nullptr) custom_icon = true;
  }
  if (icon == nullptr) {
    icon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
  }
  nid.hIcon = icon;

  if (!req.title.empty()) {
    wcsncpy(nid.szTip, W(req.title), sizeof(nid.szTip) / sizeof(wchar_t) - 1);
    wcsncpy(nid.szInfoTitle, W(req.title), sizeof(nid.szInfoTitle) / sizeof(wchar_t) - 1);
  } else {
    wcsncpy(nid.szTip, L"Gesher Notification", sizeof(nid.szTip) / sizeof(wchar_t) - 1);
    wcsncpy(nid.szInfoTitle, L"Gesher", sizeof(nid.szInfoTitle) / sizeof(wchar_t) - 1);
  }

  if (!req.text.empty()) {
    wcsncpy(nid.szInfo, W(req.text), sizeof(nid.szInfo) / sizeof(wchar_t) - 1);
  }

  nid.dwInfoFlags = NIIF_INFO | NIIF_LARGE_ICON;

  {
    std::lock_guard<std::mutex> lock(g_tray_lock);
    auto it = g_tray_icons.find(req.id);
    if (it != g_tray_icons.end()) {
      if (it->second != nullptr) DestroyIcon(it->second);
      g_tray_icons.erase(it);
    }
    if (custom_icon) {
      g_tray_icons[req.id] = icon;
    }
  }

  // Try NIM_MODIFY first in case an icon with this ID is already active, else NIM_ADD.
  if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) {
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
      if (custom_icon) DestroyIcon(icon);
      return E_FAIL;
    }
  }

  return S_OK;
}

long RemoveShellBalloonNotification(int id) {
  HWND hwnd = GetTrayHelperWindow();
  if (hwnd == nullptr) return S_OK;

  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(NOTIFYICONDATAW);
  nid.hWnd = hwnd;
  nid.uID = static_cast<UINT>(id);
  Shell_NotifyIconW(NIM_DELETE, &nid);

  std::lock_guard<std::mutex> lock(g_tray_lock);
  auto it = g_tray_icons.find(id);
  if (it != g_tray_icons.end()) {
    if (it->second != nullptr) DestroyIcon(it->second);
    g_tray_icons.erase(it);
  }
  return S_OK;
}

long ShowBridgeNotification(const NotificationRequest& req) {
  // First attempt native WinRT Action Center Toast Notification.
  long hr = ShowWinRTToastNotification(req);
  if (SUCCEEDED(hr)) {
    return hr;
  }

  // If WinRT Toast is unavailable (e.g. headless/bare runner or older edition),
  // fall back to Shell_NotifyIconW balloon notification with app icon and title.
  return ShowShellBalloonNotification(req);
}

long CancelBridgeNotification(const std::u16string& app_id, const std::u16string& tag,
                             const std::u16string& group, int id) {
  long hr_winrt = devsvc::RemoveToast(app_id, tag, group);
  long hr_shell = RemoveShellBalloonNotification(id);
  return SUCCEEDED(hr_winrt) ? hr_winrt : hr_shell;
}

}  // namespace windows
}  // namespace gesher

// --- JNI exports for com.gesher.server.WindowsHost ---

extern "C" {

JNIEXPORT jint JNICALL
Java_com_gesher_server_WindowsHost_nativeShowNotificationBridge(
    JNIEnv* env, jclass, jstring japp_id, jstring jtag, jstring jgroup,
    jstring jtitle, jstring jtext, jstring jicon_path, jint id) {
  gesher::windows::NotificationRequest req;
  req.id = static_cast<int>(id);

  if (japp_id != nullptr) {
    const jchar* chars = env->GetStringChars(japp_id, nullptr);
    req.app_id.assign(reinterpret_cast<const char16_t*>(chars), env->GetStringLength(japp_id));
    env->ReleaseStringChars(japp_id, chars);
  }
  if (jtag != nullptr) {
    const jchar* chars = env->GetStringChars(jtag, nullptr);
    req.tag.assign(reinterpret_cast<const char16_t*>(chars), env->GetStringLength(jtag));
    env->ReleaseStringChars(jtag, chars);
  }
  if (jgroup != nullptr) {
    const jchar* chars = env->GetStringChars(jgroup, nullptr);
    req.group.assign(reinterpret_cast<const char16_t*>(chars), env->GetStringLength(jgroup));
    env->ReleaseStringChars(jgroup, chars);
  }
  if (jtitle != nullptr) {
    const jchar* chars = env->GetStringChars(jtitle, nullptr);
    req.title.assign(reinterpret_cast<const char16_t*>(chars), env->GetStringLength(jtitle));
    env->ReleaseStringChars(jtitle, chars);
  }
  if (jtext != nullptr) {
    const jchar* chars = env->GetStringChars(jtext, nullptr);
    req.text.assign(reinterpret_cast<const char16_t*>(chars), env->GetStringLength(jtext));
    env->ReleaseStringChars(jtext, chars);
  }
  if (jicon_path != nullptr) {
    const jchar* chars = env->GetStringChars(jicon_path, nullptr);
    req.icon_path.assign(reinterpret_cast<const char16_t*>(chars), env->GetStringLength(jicon_path));
    env->ReleaseStringChars(jicon_path, chars);
  }

  return static_cast<jint>(gesher::windows::ShowBridgeNotification(req));
}

JNIEXPORT jint JNICALL
Java_com_gesher_server_WindowsHost_nativeCancelNotificationBridge(
    JNIEnv* env, jclass, jstring japp_id, jstring jtag, jstring jgroup, jint id) {
  std::u16string app_id;
  if (japp_id != nullptr) {
    const jchar* chars = env->GetStringChars(japp_id, nullptr);
    app_id.assign(reinterpret_cast<const char16_t*>(chars), env->GetStringLength(japp_id));
    env->ReleaseStringChars(japp_id, chars);
  }
  std::u16string tag;
  if (jtag != nullptr) {
    const jchar* chars = env->GetStringChars(jtag, nullptr);
    tag.assign(reinterpret_cast<const char16_t*>(chars), env->GetStringLength(jtag));
    env->ReleaseStringChars(jtag, chars);
  }
  std::u16string group;
  if (jgroup != nullptr) {
    const jchar* chars = env->GetStringChars(jgroup, nullptr);
    group.assign(reinterpret_cast<const char16_t*>(chars), env->GetStringLength(jgroup));
    env->ReleaseStringChars(jgroup, chars);
  }

  return static_cast<jint>(
      gesher::windows::CancelBridgeNotification(app_id, tag, group, static_cast<int>(id)));
}

}  // extern "C"
