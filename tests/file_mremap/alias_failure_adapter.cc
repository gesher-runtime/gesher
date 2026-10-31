// Dedicated failure object ONLY, no production/env/runtime backend.
#include "alias_failure_adapter.h"
namespace {
int mode = 0;
int mapped_calls = 0, close_calls = 0, map_injected = 0, close_injected = 0;
HANDLE failed_close = nullptr;
}
extern "C" void FileAliasFailureReset(int value) { mode = value; }
extern "C" int FileAliasFailureOutstanding(void) { return failed_close != nullptr; }
extern "C" int FileAliasFailureMetric(int index) {
  switch (index) {
    case 0:return mapped_calls;
    case 1:return close_calls;
    case 2:return map_injected;
    case 3:return close_injected;
    default:return -1;
  }
}
extern "C" LPVOID WINAPI FileAliasMapViewOfFile(HANDLE section,DWORD access,DWORD high,DWORD low,SIZE_T bytes) {
  ++mapped_calls;
  if (mode == 1) { ++map_injected; SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
  return MapViewOfFile(section,access,high,low,bytes);
}
extern "C" BOOL WINAPI FileAliasCloseHandle(HANDLE handle) {
  ++close_calls;
  if (mode == 3) {
    ++close_injected;
    failed_close = handle;
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  return CloseHandle(handle);
}
extern "C" int FileAliasFailureCleanup(void) {
  // Controller cleanup AFTER recording failed final retirement, never recovery
  // evidence for the operation. The failure owner remains logically FAILED.
  if (!failed_close) return 0;
  if (!CloseHandle(failed_close)) return 1;
  failed_close = nullptr;
  return 0;
}
