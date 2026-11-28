// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Windows host contracts for compat/devices/hello_owner.cc (ADR 0091, phase 2b), run
// without showing any UI: Windows' build number, and how a window is classified
// relative to the window that owns Windows Hello's prompt. The windows are real and
// never shown: this process's own, one that a child process creates with this
// process's window as its owner (as Windows' prompt is owned), and unrelated ones.
// tests/biometrics/hello_probe.cc reports the same classification for the real prompt.

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "hello_owner.h"
#include "hello_win.h"

using namespace gesher::devices;

static int failures, checks;

static void Check(bool ok, const char* what) {
  checks++;
  if (!ok) failures++;
  printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  fflush(stdout);
}

static HWND Window(HWND owner) {
  return CreateWindowExW(0, L"STATIC", L"gesher hello owner test", WS_POPUP, 0, 0, 10, 10, owner,
                         nullptr, GetModuleHandleW(nullptr), nullptr);
}

// Child mode: a hidden window owned by the parent's window; its handle goes to
// stdout, and it lives until the parent closes this process's stdin.
static int Child(const char* owner_text) {
  HWND owner = reinterpret_cast<HWND>(static_cast<UINT_PTR>(strtoull(owner_text, nullptr, 10)));
  HWND window = Window(owner);
  printf("%llu\n", static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(window)));
  fflush(stdout);
  char byte;
  DWORD read = 0;
  while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), &byte, 1, &read, nullptr) && read != 0) {
  }
  if (window != nullptr) DestroyWindow(window);
  return window != nullptr ? 0 : 1;
}

struct ChildProcess {
  HANDLE process = nullptr, to_child = nullptr, from_child = nullptr;
  HWND window = nullptr;
};

static bool StartChild(HWND owner, ChildProcess* child) {
  SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, TRUE};
  HANDLE child_in = nullptr, child_out = nullptr;
  if (!CreatePipe(&child_in, &child->to_child, &inherit, 0) ||
      !CreatePipe(&child->from_child, &child_out, &inherit, 0)) {
    return false;
  }
  SetHandleInformation(child->to_child, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(child->from_child, HANDLE_FLAG_INHERIT, 0);
  wchar_t path[MAX_PATH];
  if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return false;
  std::wstring command = L"\"" + std::wstring(path) + L"\" --owned " +
                         std::to_wstring(reinterpret_cast<UINT_PTR>(owner));
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = child_in;
  startup.hStdOutput = child_out;
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION info = {};
  BOOL started = CreateProcessW(nullptr, &command[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                nullptr, nullptr, &startup, &info);
  CloseHandle(child_in);
  CloseHandle(child_out);
  if (!started) return false;
  CloseHandle(info.hThread);
  child->process = info.hProcess;
  char text[32] = {};
  DWORD used = 0;
  while (used < sizeof(text) - 1) {
    DWORD read = 0;
    if (!ReadFile(child->from_child, text + used, 1, &read, nullptr) || read == 0) break;
    if (text[used] == '\n') break;
    used += read;
  }
  child->window = reinterpret_cast<HWND>(static_cast<UINT_PTR>(strtoull(text, nullptr, 10)));
  return child->window != nullptr;
}

static void StopChild(ChildProcess* child) {
  if (child->to_child != nullptr) CloseHandle(child->to_child);
  if (child->process != nullptr) {
    WaitForSingleObject(child->process, 10000);
    CloseHandle(child->process);
  }
  if (child->from_child != nullptr) CloseHandle(child->from_child);
}

int main(int argc, char** argv) {
  if (argc == 3 && strcmp(argv[1], "--owned") == 0) return Child(argv[2]);

  int build = hello::WindowsBuild();
  printf("Windows build %d\n", build);
  Check(build >= 10240, "RtlGetVersion reports a Windows 10 or later build number");

  const DWORD self = GetCurrentProcessId();
  HWND owner = Window(nullptr);
  HWND sibling = Window(nullptr);
  HWND popup = Window(owner);
  Check(owner != nullptr && sibling != nullptr && popup != nullptr, "the test's windows exist");

  using hello::Relation;
  Check(hello::RelationOf(owner, self, owner) == Relation::kOwner, "the owner itself: owner");
  Check(hello::RelationOf(owner, self, popup) == Relation::kOwner,
        "a window of the owner's process that it owns: owner");
  Check(hello::RelationOf(owner, self, sibling) == Relation::kOwner,
        "another top-level window of the owner's process: owner");
  Check(hello::RelationOf(owner, self, nullptr) == Relation::kNone, "no foreground window: none");

  ChildProcess owned;
  bool child = StartChild(owner, &owned);
  Check(child, "a child process created a window owned by this process's window");
  if (child) {
    DWORD child_pid = 0;
    GetWindowThreadProcessId(owned.window, &child_pid);
    Check(child_pid != 0 && child_pid != self && GetAncestor(owned.window, GA_ROOTOWNER) == owner,
          "Windows reports this process's window as its root owner");
    Check(hello::RelationOf(owner, self, owned.window) == Relation::kOwned,
          "another process's window owned by the owner: owned (Windows' prompt)");
    Check(hello::RelationOf(sibling, self, owned.window) == Relation::kOther,
          "the same window relative to a window that does not own it: other");
    // Seen from the child, this process's unrelated window is another app's.
    Check(hello::RelationOf(owned.window, child_pid, sibling) == Relation::kOther,
          "an unrelated process's window: other");
    Check(hello::RelationOf(owned.window, self, sibling) == Relation::kGone,
          "an owner that belongs to another process than recorded: gone");
  }
  StopChild(&owned);

  DestroyWindow(popup);
  DestroyWindow(owner);
  Check(hello::RelationOf(owner, self, sibling) == Relation::kGone, "a destroyed owner: gone");
  Check(hello::RelationOf(nullptr, self, sibling) == Relation::kGone, "no owner: gone");
  DestroyWindow(sibling);

  // A console test process owns no foreground window: nothing would own a prompt.
  Check(hello::OwnerWindow(0) == nullptr, "pid 0 owns no window");
  Check(hello::OwnerWindow(self) == nullptr ||
            hello::RelationOf(hello::OwnerWindow(self), self, GetForegroundWindow()) ==
                Relation::kOwner,
        "this process's owner window, when it has the foreground, classifies as owner");

  printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
