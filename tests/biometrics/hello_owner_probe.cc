// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Manual Windows probe for compat/devices/hello_owner.cc (ADR 0091, phase 2b): what
// Windows' foreground window is, relative to the prompt's owner, while the real
// Windows Hello prompt shows. It answers the question the host test cannot: is the
// prompt a window of another process that the app's window owns?
//
//   hello_owner_probe build            Windows' build number; shows no UI
//   hello_owner_probe watch <pid>      asks for verification owned by the foreground
//                                      window of process <pid> (start `hello_probe host`
//                                      first and pass its pid), prints every change of
//                                      the foreground window and its classification,
//                                      and cancels as com.gesher.server.WindowsHello
//                                      does: after the prompt was seen as an owned
//                                      window, two samples of another window, or the
//                                      owner's window gone
//   hello_owner_probe observe <pid>    the same without canceling
//
// Not run in CI: watch and observe need a person at the machine.

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <thread>

#include "hello_owner.h"
#include "hello_win.h"

using namespace gesher::devices;

static const char* RelationName(hello::Relation r) {
  static const char* names[] = {"owner", "owned (the prompt)", "other", "gone", "none"};
  int i = static_cast<int>(r);
  return i >= 0 && i <= 4 ? names[i] : "?";
}

static const char* ResultName(int r) {
  static const char* names[] = {"Verified",   "DeviceNotPresent", "NotConfiguredForUser",
                                "DisabledByPolicy", "DeviceBusy", "RetriesExhausted",
                                "Canceled"};
  return r >= 0 && r <= 6 ? names[r] : "FAILED";
}

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "build";
  int build = hello::WindowsBuild();
  printf("windows build=%d (RequestVerificationForWindowAsync is used from 22000: %s)\n", build,
         build >= 22000 ? "yes" : "no");
  if (strcmp(mode, "build") == 0) return build > 0 ? 0 : 1;
  bool cancel_on_leave = strcmp(mode, "watch") == 0;
  if (!cancel_on_leave && strcmp(mode, "observe") != 0) {
    printf("usage: hello_owner_probe build | watch <pid> | observe <pid>\n");
    return 2;
  }
  DWORD pid = argc > 2 ? strtoul(argv[2], nullptr, 10) : 0;
  HWND owner = hello::OwnerWindow(pid);
  printf("owner pid=%lu window=%p\n", (unsigned long)pid, (void*)owner);
  if (owner == nullptr) {
    printf("start failed: process %lu does not own the foreground window\n", (unsigned long)pid);
    return 2;
  }
  const wchar_t message[] = L"Gesher owner probe: confirm it is you";
  hello::StartFailure failure;
  HRESULT hr = S_OK;
  hello::Verification* v = hello::Start(pid, message, wcslen(message), &failure, &hr);
  if (v == nullptr) {
    printf("start failed: %s hr=0x%08lx\n",
           failure == hello::StartFailure::kNotForeground ? "not foreground" : "windows",
           (unsigned long)hr);
    return 2;
  }
  printf("started\n");
  fflush(stdout);
  std::atomic<int> result{-2};
  DWORD started = GetTickCount();
  std::thread waiter([&] { result = hello::Await(v); });
  // com.gesher.server.OwnerWatch, restated: the probe is native only.
  bool prompt_seen = false, canceled = false;
  int away = 0;
  HWND last = reinterpret_cast<HWND>(static_cast<INT_PTR>(-1));
  while (result == -2) {
    HWND foreground = GetForegroundWindow();
    hello::Relation relation = hello::RelationOf(owner, pid, foreground);
    if (foreground != last) {
      last = foreground;
      DWORD foreground_pid = 0;
      if (foreground != nullptr) GetWindowThreadProcessId(foreground, &foreground_pid);
      char name[128] = "";
      if (foreground != nullptr) GetClassNameA(foreground, name, sizeof(name));
      printf("%6lu ms foreground=%p pid=%lu class=\"%s\" root owner=%p: %s\n",
             (unsigned long)(GetTickCount() - started), (void*)foreground,
             (unsigned long)foreground_pid, name,
             (void*)(foreground != nullptr ? GetAncestor(foreground, GA_ROOTOWNER) : nullptr),
             RelationName(relation));
      fflush(stdout);
    }
    bool leave = false;
    switch (relation) {
      case hello::Relation::kGone: leave = true; break;
      case hello::Relation::kOwned: prompt_seen = true; away = 0; break;
      case hello::Relation::kOwner: away = 0; break;
      case hello::Relation::kOther: leave = prompt_seen && ++away >= 2; break;
      case hello::Relation::kNone: break;
    }
    if (leave && !canceled) {
      canceled = true;
      printf("%6lu ms the owner %s%s\n", (unsigned long)(GetTickCount() - started),
             relation == hello::Relation::kGone ? "is gone" : "left the foreground",
             cancel_on_leave ? ": canceling" : " (observe: not canceling)");
      fflush(stdout);
      if (cancel_on_leave) hello::Cancel(v);
    }
    Sleep(250);
  }
  waiter.join();
  hello::Release(v);
  printf("prompt seen as an owned window: %s\n", prompt_seen ? "yes" : "NO");
  printf("result=%d (%s) after %lu ms\n", result.load(), ResultName(result),
         (unsigned long)(GetTickCount() - started));
  return 0;
}
