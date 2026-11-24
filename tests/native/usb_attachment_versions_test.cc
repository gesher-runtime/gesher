// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Executes the production attachment registry/callback with only notification
// registration adapted. No physical interface, USB endpoint or disk is opened.
#include <windows.h>
#include <cfgmgr32.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace fixture {
PCM_NOTIFY_CALLBACK callback = nullptr;
void* context = nullptr;
unsigned checks = 0;
CONFIGRET WINAPI Register(PCM_NOTIFY_FILTER, PVOID user, PCM_NOTIFY_CALLBACK changed,
                          PHCMNOTIFICATION handle) {
  context = user;
  callback = changed;
  *handle = reinterpret_cast<HCMNOTIFICATION>(uintptr_t{1});
  return CR_SUCCESS;
}
void Check(bool condition, const char* expression, int line) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "USB attachment registry: line %d: %s\n", line, expression);
    std::exit(1);
  }
}
}  // namespace fixture

#define CM_Register_Notification fixture::Register
#include "../../compat/devsvc/usb_winusb.cc"
#undef CM_Register_Notification

#define CHECK(expression) fixture::Check((expression), #expression, __LINE__)

void Changed(CM_NOTIFY_ACTION action, const std::wstring& path) {
  const size_t length = sizeof(CM_NOTIFY_EVENT_DATA) + (path.size() + 1) * sizeof(wchar_t);
  std::vector<std::max_align_t> storage((length + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
  auto* event = reinterpret_cast<CM_NOTIFY_EVENT_DATA*>(storage.data());
  event->FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
  constexpr size_t at = offsetof(CM_NOTIFY_EVENT_DATA, u.DeviceInterface.SymbolicLink);
  std::memcpy(reinterpret_cast<std::byte*>(event) + at, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
  CHECK(fixture::callback(reinterpret_cast<HCMNOTIFICATION>(uintptr_t{1}), fixture::context,
                          action, event, static_cast<DWORD>(length)) == ERROR_SUCCESS);
}

int main() {
  CHECK(gesher::usb::AttachmentMonitoringAvailable());
  std::vector<std::wstring> paths;
  for (unsigned i = 0; i < 512; ++i) {
    paths.push_back(L"\\\\?\\usb#fixture-" + std::to_wstring(i));
    CHECK(gesher::usb::AttachmentVersion(paths.back().c_str()) > 0);
  }
  const int64_t first = gesher::usb::AttachmentVersion(paths[0].c_str());
  CHECK(gesher::usb::AttachmentVersion(L"\\\\?\\USB#FIXTURE-0") == first);
  CHECK(gesher::usb::AttachmentVersion(L"\\\\?\\usb#overflow") == -1);
  Changed(CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL, paths[0]);
  CHECK(gesher::usb::AttachmentVersion(paths[0].c_str()) > first);
  Changed(CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL, paths[0]);
  const int64_t replacement = gesher::usb::AttachmentVersion(L"\\\\?\\usb#replacement");
  CHECK(replacement > first);
  CHECK(gesher::usb::AttachmentVersion(paths[0].c_str()) == -1);
  Changed(CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL, L"\\\\?\\usb#replacement");
  CHECK(gesher::usb::AttachmentVersion(paths[0].c_str()) > replacement);
  for (const auto& path : paths) Changed(CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL, path);
  int64_t previous = replacement;
  for (unsigned i = 0; i < 2000; ++i) {
    const auto path = L"\\\\?\\usb#attachment-" + std::to_wstring(i);
    const int64_t version = gesher::usb::AttachmentVersion(path.c_str());
    CHECK(version > previous);
    Changed(CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL, path);
    previous = version;
  }
  std::printf("USB attachment registry fixture: %u checks passed\n", fixture::checks);
}
