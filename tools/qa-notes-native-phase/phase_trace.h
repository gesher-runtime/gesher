#pragma once
// Diagnostic-only, bounded phase data. No URLs, script bodies, arguments,
// environment dumps, screenshots or unrelated memory are captured here.
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

inline void QaNotesPhase(const char* phase, const void* object = nullptr,
                         long long a = 0, long long b = 0,
                         long long c = 0, long long d = 0) {
  DWORD saved = GetLastError();
  wchar_t directory[2048];
  DWORD length = GetEnvironmentVariableW(L"GESHER_QA_NOTES_PHASE_DIR", directory, 2048);
  if (length == 0 || length >= 2048) { SetLastError(saved); return; }
  static volatile LONG count = 0;
  LONG sequence = InterlockedIncrement(&count);
  if (sequence > 1024) { SetLastError(saved); return; }
  wchar_t path[2120];
  int n = swprintf(path, 2120, L"%ls\\native-phase-%lu.jsonl", directory, GetCurrentProcessId());
  if (n <= 0 || n >= 2120) { SetLastError(saved); return; }
  char line[512];
  int size = snprintf(line, sizeof(line),
      "{\"phase\":\"%s\",\"tick\":%llu,\"pid\":%lu,\"tid\":%lu,\"seq\":%ld,"
      "\"object\":%llu,\"a\":%lld,\"b\":%lld,\"c\":%lld,\"d\":%lld}\n",
      phase, static_cast<unsigned long long>(GetTickCount64()), GetCurrentProcessId(),
      GetCurrentThreadId(), sequence, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(object)),
      a, b, c, d);
  if (size > 0 && size < static_cast<int>(sizeof(line))) {
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      DWORD written = 0;
      WriteFile(file, line, static_cast<DWORD>(size), &written, nullptr);
      CloseHandle(file);
    }
  }
  SetLastError(saved);
}
