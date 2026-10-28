#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <userenv.h>
#include <sddl.h>
#include <stdio.h>
#include <wchar.h>
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "advapi32.lib")

static int child(int argc, wchar_t** argv) {
  if (argc != 7) return 10;
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return 11;
  DWORD size = 0, isolated = 0;
  if (!GetTokenInformation(token, TokenIsAppContainer, &isolated, sizeof(isolated), &size) || isolated != 1) return 12;
  GetTokenInformation(token, TokenAppContainerSid, nullptr, 0, &size);
  BYTE* buffer = new BYTE[size];
  if (!GetTokenInformation(token, TokenAppContainerSid, buffer, size, &size)) return 13;
  PSID expected = nullptr;
  if (!ConvertStringSidToSidW(argv[2], &expected) || !EqualSid(expected, reinterpret_cast<TOKEN_APPCONTAINER_INFORMATION*>(buffer)->TokenAppContainer)) return 14;
  LocalFree(expected); delete[] buffer; CloseHandle(token);
  DWORD pid = wcstoul(argv[3], nullptr, 10), tid = wcstoul(argv[4], nullptr, 10);
  HANDLE p = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_VM_WRITE | PROCESS_CREATE_THREAD, FALSE, pid);
  if (p) { CloseHandle(p); return 15; }
  if (GetLastError() != ERROR_ACCESS_DENIED) return 16;
  HANDLE t = OpenThread(THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
  if (t) { CloseHandle(t); return 17; }
  if (GetLastError() != ERROR_ACCESS_DENIED) return 18;
  HANDLE f = CreateFileW(argv[5], GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f != INVALID_HANDLE_VALUE) { CloseHandle(f); return 19; }
  if (GetLastError() != ERROR_ACCESS_DENIED) return 20;
  HANDLE out = reinterpret_cast<HANDLE>(_wcstoui64(argv[6], nullptr, 10));
  DWORD written = 0; const char message[] = "TOKEN PROCESS THREAD FILE PIPE PASS\n";
  if (!WriteFile(out, message, sizeof(message)-1, &written, nullptr) || written != sizeof(message)-1) return 21;
  return 0;
}
int wmain(int argc, wchar_t** argv) {
  if (argc > 1 && wcscmp(argv[1], L"child") == 0) return child(argc, argv);
  wchar_t exe[MAX_PATH], tmp[MAX_PATH], file[MAX_PATH], profile[80];
  if (!GetModuleFileNameW(nullptr, exe, MAX_PATH) || !GetTempPathW(MAX_PATH, tmp)) return 1;
  if (!GetTempFileNameW(tmp, L"gsi", 0, file)) return 2;
  swprintf_s(profile, L"GesherIsolationProbe-%lu-%lu", GetCurrentProcessId(), GetTickCount());
  PSID sid = nullptr;
  HRESULT hr = CreateAppContainerProfile(profile, profile, profile, nullptr, 0, &sid);
  if (FAILED(hr)) { DeleteFileW(file); return 3; }
  LPWSTR sidText = nullptr;
  HANDLE readPipe = nullptr, writePipe = nullptr;
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  int result = 4;
  if (!ConvertSidToStringSidW(sid, &sidText) || !CreatePipe(&readPipe, &writePipe, &sa, 0) || !SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) goto done;
  {
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 2, 0, &bytes);
    auto attrs = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, bytes));
    if (!attrs || !InitializeProcThreadAttributeList(attrs, 2, 0, &bytes)) goto done;
    SECURITY_CAPABILITIES caps{}; caps.AppContainerSid = sid;
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &caps, sizeof(caps), nullptr, nullptr) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &writePipe, sizeof(writePipe), nullptr, nullptr)) {
      DeleteProcThreadAttributeList(attrs); HeapFree(GetProcessHeap(), 0, attrs); goto done;
    }
    wchar_t cmd[4096];
    swprintf_s(cmd, L"\"%s\" child %s %lu %lu \"%s\" %llu", exe, sidText, GetCurrentProcessId(), GetCurrentThreadId(), file, static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(writePipe)));
    STARTUPINFOEXW si{}; si.StartupInfo.cb = sizeof(si); si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    BOOL started = CreateProcessW(exe, cmd, nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED, nullptr, nullptr, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs); HeapFree(GetProcessHeap(), 0, attrs);
    if (!started) { fwprintf(stderr, L"CreateProcess failed: %lu\n", GetLastError()); goto done; }
    CloseHandle(writePipe); writePipe = nullptr;
    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) { TerminateProcess(pi.hProcess, 99); }
    if (WaitForSingleObject(pi.hProcess, 15000) != WAIT_OBJECT_0) { TerminateProcess(pi.hProcess, 98); WaitForSingleObject(pi.hProcess, 5000); }
    DWORD code = 99; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if (code != 0) { fwprintf(stderr, L"child exit: %lu\n", code); goto done; }
    char buf[128]{}; DWORD n = 0;
    if (!ReadFile(readPipe, buf, sizeof(buf)-1, &n, nullptr) || n == 0) goto done;
    printf("%s", buf); result = 0;
  }
done:
  if (readPipe) CloseHandle(readPipe);
  if (writePipe) CloseHandle(writePipe);
  if (sidText) LocalFree(sidText);
  FreeSid(sid);
  if (FAILED(DeleteAppContainerProfile(profile))) result = 5;
  if (!DeleteFileW(file)) result = 6;
  return result;
}
