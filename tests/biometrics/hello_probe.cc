// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Manual Windows probe for compat/devices/hello_win.cc (ADR 0094, phase 2).
// It drives the production functions against the real Windows Hello:
//
//   hello_probe availability           CheckAvailabilityAsync only; shows no UI
//   hello_probe verify                 a window of this process asks for verification
//   hello_probe cancel                 the same, canceled by the "app" after 3 s
//   hello_probe host [seconds]         shows a window (30 s by default) and prints its
//                                      pid: the "app" of the two modes below
//   hello_probe foreign-verify <pid>   this process (the "system server") asks for
//   hello_probe foreign-cancel <pid>   verification owned by the foreground window of
//                                      process <pid>: the cross-process owner Gesher uses
//
// Not run in CI: every mode but availability needs a person at the machine.

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <thread>

#include "hello_win.h"

using namespace gesher::devices;

static const char* AvailabilityName(int a) {
  static const char* names[] = {"Available", "DeviceNotPresent", "NotConfiguredForUser",
                                "DisabledByPolicy", "DeviceBusy"};
  return a >= 0 && a <= 4 ? names[a] : "FAILED";
}

static const char* ResultName(int r) {
  static const char* names[] = {"Verified",   "DeviceNotPresent", "NotConfiguredForUser",
                                "DisabledByPolicy", "DeviceBusy", "RetriesExhausted",
                                "Canceled"};
  return r >= 0 && r <= 6 ? names[r] : "FAILED";
}

static void Pump() {
  MSG m;
  while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
}

static HWND MakeWindow(const wchar_t* title) {
  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"GesherHelloProbe";
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  RegisterClassW(&wc);
  HWND w = CreateWindowExW(0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, 480, 240, nullptr, nullptr,
                           wc.hInstance, nullptr);
  SetForegroundWindow(w);
  for (int i = 0; i < 20 && GetForegroundWindow() != w; i++) {
    Pump();
    Sleep(50);
  }
  return w;
}

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "availability";
  if (strcmp(mode, "host") == 0) {
    HWND w = MakeWindow(L"Gesher Windows Hello probe: app window");
    printf("host pid=%lu foreground=%s\n", (unsigned long)GetCurrentProcessId(),
           GetForegroundWindow() == w ? "yes" : "no");
    fflush(stdout);
    DWORD seconds = argc > 2 ? strtoul(argv[2], nullptr, 10) : 30;
    for (DWORD end = GetTickCount() + seconds * 1000; GetTickCount() < end;) {
      Pump();
      Sleep(20);
    }
    return 0;
  }

  HRESULT hr = S_OK;
  int a = hello::Availability(&hr);
  printf("availability=%d (%s) hr=0x%08lx\n", a, AvailabilityName(a), (unsigned long)hr);
  if (strcmp(mode, "availability") == 0) return a >= 0 ? 0 : 1;

  bool foreign = strncmp(mode, "foreign-", 8) == 0;
  bool cancel = strstr(mode, "cancel") != nullptr;
  DWORD owner = GetCurrentProcessId();
  HWND w = nullptr;
  if (foreign) {
    owner = argc > 2 ? strtoul(argv[2], nullptr, 10) : 0;
    DWORD foreground = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
    printf("owner pid=%lu, this pid=%lu, foreground pid=%lu\n", (unsigned long)owner,
           (unsigned long)GetCurrentProcessId(), (unsigned long)foreground);
  } else {
    w = MakeWindow(L"Gesher Windows Hello probe");
    printf("foreground is probe window: %s\n", GetForegroundWindow() == w ? "yes" : "no");
  }

  const wchar_t message[] = L"Gesher probe: confirm it is you";
  hello::StartFailure failure;
  hello::Verification* v = hello::Start(owner, message, wcslen(message), &failure, &hr);
  if (v == nullptr) {
    printf("start failed: %s hr=0x%08lx\n",
           failure == hello::StartFailure::kNotForeground ? "not foreground" : "windows",
           (unsigned long)hr);
    return 2;
  }
  printf("started%s\n", foreign ? " with another process's window as owner" : "");
  fflush(stdout);
  int result = -2;
  DWORD started = GetTickCount();
  std::thread waiter([&] { result = hello::Await(v); });
  bool canceled = false;
  while (result == -2) {
    if (w != nullptr) Pump();
    if (cancel && !canceled && GetTickCount() - started > 3000) {
      printf("app cancels after 3 s\n");
      hello::Cancel(v);
      hello::Cancel(v);  // idempotent
      canceled = true;
    }
    Sleep(20);
  }
  waiter.join();
  hello::Release(v);
  printf("result=%d (%s) after %lu ms\n", result, ResultName(result),
         (unsigned long)(GetTickCount() - started));
  if (w != nullptr) DestroyWindow(w);
  return 0;
}
