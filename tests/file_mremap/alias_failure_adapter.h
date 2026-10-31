/* Dedicated failure-object compilation ONLY. Never installed or runtime-selected. */
#ifndef GESHER_FILE_MREMAP_FAILURE_ADAPTER_H
#define GESHER_FILE_MREMAP_FAILURE_ADAPTER_H
#ifndef GESHER_FILE_MREMAP_FAILURE_TEST
#error This header requires the separately compiled failure-test object
#endif
#include <windows.h>
#ifdef __cplusplus
extern "C" {
#endif
LPVOID WINAPI FileAliasMapViewOfFile(HANDLE,DWORD,DWORD,DWORD,SIZE_T);
BOOL WINAPI FileAliasCloseHandle(HANDLE);
void FileAliasFailureReset(int mode);
int FileAliasFailureOutstanding(void);
int FileAliasFailureMetric(int index);
int FileAliasFailureCleanup(void);
#ifdef __cplusplus
}
#endif
#endif
