// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Gesher (devices track, ADR 0091 phase 2b): see hello_owner.h.

#include "hello_owner.h"

namespace gesher::devices::hello {

int WindowsBuild() {
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll == nullptr) return 0;
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  auto get = reinterpret_cast<RtlGetVersionFn>(
      reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
  if (get == nullptr) return 0;
  OSVERSIONINFOW info = {};
  info.dwOSVersionInfoSize = sizeof(info);
  if (get(&info) != 0) return 0;  // STATUS_SUCCESS is 0
  return static_cast<int>(info.dwBuildNumber);
}

Relation RelationOf(HWND owner, DWORD pid, HWND foreground) {
  DWORD owner_pid = 0;
  if (owner == nullptr || !IsWindow(owner) ||
      GetWindowThreadProcessId(owner, &owner_pid) == 0 || owner_pid != pid) {
    return Relation::kGone;
  }
  if (foreground == nullptr) return Relation::kNone;
  DWORD foreground_pid = 0;
  if (GetWindowThreadProcessId(foreground, &foreground_pid) == 0) return Relation::kNone;
  if (foreground_pid == pid) return Relation::kOwner;
  return GetAncestor(foreground, GA_ROOTOWNER) == owner ? Relation::kOwned : Relation::kOther;
}

Relation ForegroundRelation(HWND owner, DWORD pid) {
  return RelationOf(owner, pid, GetForegroundWindow());
}

}  // namespace gesher::devices::hello
