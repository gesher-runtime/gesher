/* Dedicated failure-object compilation ONLY. Never installed or runtime-selected. */
#ifndef GESHER_FILE_MREMAP_FAILURE_ADAPTER_H
#define GESHER_FILE_MREMAP_FAILURE_ADAPTER_H
#ifndef GESHER_FILE_MREMAP_FAILURE_TEST
#error This header requires the separately compiled failure-test object
#endif
#include <windows.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
LPVOID WINAPI FileAliasMapViewOfFile(HANDLE,DWORD,DWORD,DWORD,SIZE_T);
BOOL WINAPI FileAliasCloseHandle(HANDLE);
BOOL WINAPI FileAliasUnmapViewOfFile(const void*);
HANDLE FileAliasCreateMapping(HANDLE,DWORD);
HANDLE FileAliasReOpenFile(HANDLE,DWORD,DWORD,DWORD);
BOOL FileAliasDuplicate(HANDLE,HANDLE*);
typedef void*(WINAPI* FileAliasAlloc2Routine)(HANDLE,void*,SIZE_T,ULONG,ULONG,void*,ULONG);
void* FileAliasReserve(FileAliasAlloc2Routine,HANDLE,void*,SIZE_T,ULONG,ULONG);
void FileAliasAllocationDisable(void);
typedef void*(WINAPI* FileAliasMap3Routine)(HANDLE,HANDLE,void*,ULONG64,SIZE_T,ULONG,ULONG,void*,ULONG);
typedef BOOL(WINAPI* FileAliasUnmap2Routine)(HANDLE,void*,ULONG);
void* FileAliasMap3(FileAliasMap3Routine,HANDLE,HANDLE,void*,ULONG64,SIZE_T,ULONG,ULONG);
BOOL FileAliasUnmap2(FileAliasUnmap2Routine,HANDLE,void*,ULONG);
SIZE_T FileAliasQuery(const void*,MEMORY_BASIC_INFORMATION*,SIZE_T);
BOOL FileAliasProtect(void*,SIZE_T,DWORD,PDWORD);
BOOL FileAliasFree(void*,SIZE_T,DWORD);
BOOL FileAliasPublicationAllowed(void);
/* Caller storage only: snapshot allocates nothing and holds registry lock.
 * query uses real VirtualQuery, bypassing injection; never a production ABI. */
typedef struct FileAliasPage {
  size_t page,region_page,region_size,live_pages,live_capacity,owner_refs,owner_views,owner_backing_pins;
  int present,kind,live,owner_state,owner_failure;
  DWORD ledger_protection,operation_error,query_state,query_protection,query_type;
  int mapping_handle_valid;
  int query_ok,group,failed_owners,failed_owner_state,rollback_view,failed_handle_valid,rollback_query_ok;
  DWORD rollback_state,rollback_protection,rollback_type;
  size_t backing_refs,query_region_size;
  uintptr_t query_base,allocation_base;
  DWORD failed_owner_error;
  size_t failed_owner_refs;
} FileAliasPage;
int FileAliasRegistrySnapshot(const void*,size_t,FileAliasPage*,size_t);
void FileAliasFailureReset(int mode);
void FileAliasFailureNth(int nth);
int FileAliasFailureOutstanding(void);
int FileAliasFailureMetric(int index);
int FileAliasFailureCleanup(void);
#ifdef __cplusplus
}
#endif
#endif
