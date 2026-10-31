// Link ONLY with a separately compiled mman failure object in CI.
#include "alias_failure_adapter.h"
namespace {
int mode = 0;
HANDLE owned[16] = {};
}
extern "C" void FileAliasFailureReset(int value) { mode = value; }
extern "C" int FileAliasFailureOutstanding(void) {
  int count = 0;
  for (HANDLE handle : owned) if (handle != nullptr) ++count;
  return count;
}
extern "C" BOOL WINAPI FileAliasDuplicateHandle(HANDLE source_process,HANDLE source,
    HANDLE target_process,LPHANDLE target,DWORD access,BOOL inherit,DWORD options) {
  if (mode == 1) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  HANDLE* slot = nullptr;
  for (HANDLE& handle : owned) if (handle == nullptr) { slot = &handle; break; }
  if (slot == nullptr) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  if (!DuplicateHandle(source_process,source,target_process,target,access,inherit,options)) return FALSE;
  *slot = *target;
  return TRUE;
}
extern "C" LPVOID WINAPI FileAliasMapViewOfFile(HANDLE section,DWORD access,DWORD high,DWORD low,SIZE_T bytes) {
  if (mode == 2 || mode == 3) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
  return MapViewOfFile(section,access,high,low,bytes);
}
extern "C" BOOL WINAPI FileAliasCloseHandle(HANDLE handle) {
  if (mode == 3) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  if (!CloseHandle(handle)) return FALSE;
  for (HANDLE& entry : owned) if (entry == handle) entry = nullptr;
  return TRUE;
}
extern "C" int FileAliasFailureCleanup(void) {
  // Controller cleanup AFTER recording the actual failed invariant; never
  // disguises the production operation's missing release as success.
  int failed = 0;
  for (HANDLE& handle : owned) if (handle != nullptr) {
    if (!CloseHandle(handle)) ++failed;
    else handle = nullptr;
  }
  return failed;
}
