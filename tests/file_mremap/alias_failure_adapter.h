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
typedef void*(WINAPI* FileAliasMap3Routine)(HANDLE,HANDLE,void*,ULONG64,SIZE_T,ULONG,ULONG,void*,ULONG);
typedef BOOL(WINAPI* FileAliasUnmap2Routine)(HANDLE,void*,ULONG);
void* FileAliasMap3(FileAliasMap3Routine,HANDLE,HANDLE,void*,ULONG64,SIZE_T,ULONG,ULONG);
BOOL FileAliasUnmap2(FileAliasUnmap2Routine,HANDLE,void*,ULONG);
SIZE_T FileAliasQuery(const void*,MEMORY_BASIC_INFORMATION*,SIZE_T);
BOOL FileAliasProtect(void*,SIZE_T,DWORD,PDWORD);
BOOL FileAliasFree(void*,SIZE_T,DWORD);
BOOL FileAliasPublicationAllowed(void);
void FileAliasFailureReset(int mode);
void FileAliasFailureNth(int nth);
int FileAliasFailureOutstanding(void);
int FileAliasFailureMetric(int index);
int FileAliasFailureCleanup(void);
#ifdef __cplusplus
}
#endif
#endif
