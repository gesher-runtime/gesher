/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <string>

namespace gesher {
namespace windows {

struct NotificationRequest {
  std::u16string app_id;
  std::u16string tag;
  std::u16string group;
  std::u16string title;
  std::u16string text;
  std::u16string icon_path;
  int id = 0;
  bool suppress_popup = false;
  bool high_priority = false;
};

// Shows a notification by bridging to Windows WinRT Action Center Toasts,
// falling back to Shell_NotifyIconW balloon notifications with app icon and title
// if WinRT is unavailable or on editions without WinRT toasts.
long ShowBridgeNotification(const NotificationRequest& req);

// Cancels and removes the notification from both Action Center and the taskbar tray.
long CancelBridgeNotification(const std::u16string& app_id, const std::u16string& tag,
                             const std::u16string& group, int id);

// Explicit WinRT Action Center Toast notification.
long ShowWinRTToastNotification(const NotificationRequest& req);

// Explicit Shell_NotifyIconW balloon notification.
long ShowShellBalloonNotification(const NotificationRequest& req);

// Removes a Shell_NotifyIconW balloon/tray notification.
long RemoveShellBalloonNotification(int id);

}  // namespace windows
}  // namespace gesher
