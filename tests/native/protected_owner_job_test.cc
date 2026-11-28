// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Actual Windows job lifecycle and raw descendant inheritance. No elevated
// authority or AppContainer launch is claimed by this standalone fixture.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../../compat/usb_storage/protected_owner_jobs.h"
void Check(bool ok, const char* label) {
  if (!ok) {
    std::fprintf(stderr, "FAIL %s Win32=%lu\n", label, GetLastError());
    std::exit(1);
  }
}
void Transfer(HANDLE pipe, void* bytes, DWORD n, bool write) {
  DWORD moved = 0;
  Check((write ? WriteFile(pipe, bytes, n, &moved, nullptr)
               : ReadFile(pipe, bytes, n, &moved, nullptr)) &&
            moved == n,
        "actual owned fixture transfer");
}
uint64_t Birth(HANDLE process) {
  FILETIME c{}, e{}, k{}, u{};
  Check(GetProcessTimes(process, &c, &e, &k, &u), "actual process generation");
  return (uint64_t(c.dwHighDateTime) << 32) | c.dwLowDateTime;
}
struct Descendant {
  DWORD pid;
  uint64_t birth;
};
int wmain(int argc, wchar_t** argv) {
  wchar_t image[32768]{};
  Check(GetModuleFileNameW(nullptr, image, 32768) > 0, "own image");
  if (argc == 2) {
    Sleep(INFINITE);
    return 0;
  }
  if (argc == 3) {
    HANDLE pipe = CreateFileW(argv[2], GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    Check(pipe != INVALID_HANDLE_VALUE, "child owned pipe");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    std::wstring command = L"\"" + std::wstring(image) + L"\" --grandchild";
    Check(CreateProcessW(image, command.data(), nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child),
          "raw descendant creation without job attributes");
    Descendant info{child.dwProcessId, Birth(child.hProcess)};
    Transfer(pipe, &info, sizeof(info), true);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    BYTE receipt = 0;
    Transfer(pipe, &receipt, 1, false);
    Sleep(INFINITE);
    return 0;
  }
  for (bool crash : {false, true}) {
    HANDLE job = nullptr;
    Check(gesher::usb_storage::CreateProtectedOwnerJob(&job) == 0 && job,
          "actual production owner job helper");
    DWORD flags = 0;
    Check(GetHandleInformation(job, &flags) && !(flags & HANDLE_FLAG_INHERIT),
          "guest cannot inherit authority job handle");
    std::wstring name = L"\\\\.\\pipe\\Gesher.OwnerJobFixture." +
                        std::to_wstring(GetCurrentProcessId()) +
                        (crash ? L".crash" : L".retire");
    HANDLE pipe = CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 4096, 0, nullptr);
    Check(pipe != INVALID_HANDLE_VALUE, "owned job fixture pipe");
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<BYTE> storage(bytes);
    auto attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)storage.data();
    Check(
        InitializeProcThreadAttributeList(attrs, 1, 0, &bytes) &&
            UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                      &job, sizeof(job), nullptr, nullptr),
        "atomic owner job creation attribute");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attrs;
    PROCESS_INFORMATION child{};
    std::wstring command =
        L"\"" + std::wstring(image) + L"\" --child \"" + name + L"\"";
    Check(CreateProcessW(image, command.data(), nullptr, nullptr, FALSE,
                         EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED |
                             CREATE_NO_WINDOW,
                         nullptr, nullptr, &startup.StartupInfo, &child),
          "actual direct child");
    BOOL member = FALSE;
    Check(IsProcessInJob(child.hProcess, job, &member) && member,
          "actual assignment precedes resume");
    Check(ResumeThread(child.hThread) != (DWORD)-1, "child resume");
    DeleteProcThreadAttributeList(attrs);
    ULONGLONG deadline = GetTickCount64() + 8000;
    while (!ConnectNamedPipe(pipe, nullptr) &&
           GetLastError() != ERROR_PIPE_CONNECTED) {
      Check(GetTickCount64() < deadline &&
                WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT,
            "bounded child connection");
      Sleep(1);
    }
    DWORD mode = PIPE_READMODE_BYTE | PIPE_WAIT;
    Check(SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr),
          "fixture receive mode");
    Descendant info{};
    Transfer(pipe, &info, sizeof(info), false);
    HANDLE grandchild = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, info.pid);
    Check(grandchild && Birth(grandchild) == info.birth,
          "hold exact descendant generation");
    member = FALSE;
    Check(IsProcessInJob(grandchild, job, &member) && member,
          "raw descendant remains in exact installation job");
    if (crash) {
      Check(CloseHandle(job), "simulate authority handle loss");
      job = nullptr;
    } else
      Check(gesher::usb_storage::StopProtectedOwnerJob(job) == 0,
            "actual retirement waits whole family");
    Check(WaitForSingleObject(child.hProcess, 8000) == WAIT_OBJECT_0 &&
              WaitForSingleObject(grandchild, 8000) == WAIT_OBJECT_0,
          "both enrolled child and raw descendant actually terminate");
    if (job) CloseHandle(job);
    CloseHandle(grandchild);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    std::puts(
        crash
            ? "PASS actual kill-on-close authority-loss family control"
            : "PASS actual explicit owner retirement includes raw descendant");
  }
  return 0;
}
