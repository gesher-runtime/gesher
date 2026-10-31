// Dedicated failure object ONLY, no production/env/runtime backend.
#include "alias_failure_adapter.h"
#include <cstdlib>
#include <new>
#if !defined(__cpp_exceptions)
#error only the dedicated failure allocator adapter must enable exceptions
#endif
namespace {
int mode = 0;
int mapped_calls = 0, close_calls = 0, map_injected = 0, close_injected = 0;
HANDLE failed_close = nullptr;
int nth=1;
long allocation_budget=-1;
int extra[8]={};
int extended[14]={};
int plain_unmap_calls=0,plain_unmap_injected=0;
}
extern "C" void FileAliasFailureReset(int value) {
  plain_unmap_calls=plain_unmap_injected=0;
  allocation_budget=-1;for(int& value:extra)value=0;
  mode=value; mapped_calls=close_calls=map_injected=close_injected=0;
  for(int& count:extended)count=0;
}
extern "C" void FileAliasFailureNth(int value) { nth=value;if(mode==12)allocation_budget=value-1; }
extern "C" void FileAliasAllocationDisable(void) { allocation_budget=-1; }
extern "C" int FileAliasFailureOutstanding(void) { return failed_close != nullptr; }
extern "C" int FileAliasFailureMetric(int index) {
  switch (index) {
    case 0:return mapped_calls;
    case 1:return close_calls;
    case 2:return map_injected;
    case 3:return close_injected;
    case 18:return plain_unmap_calls;
    case 19:return plain_unmap_injected;
    default:return index>=20&&index<28?extra[index-20]:index>=4&&index<18?extended[index-4]:-1;
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

namespace {
bool fail(int operation,int requested) {
  int call=++extended[operation];
  if(mode==requested && call==nth) {
    ++extended[operation+7]; SetLastError(ERROR_NOT_ENOUGH_MEMORY); return true;
  }
  return false;
}
}
extern "C" SIZE_T FileAliasQuery(const void* at,MEMORY_BASIC_INFORMATION* info,SIZE_T size) {
  return fail(0,4)?0:VirtualQuery(at,info,size);
}
extern "C" BOOL FileAliasProtect(void* at,SIZE_T size,DWORD prot,PDWORD old) {
  return fail(1,5)?FALSE:VirtualProtect(at,size,prot,old);
}
extern "C" void* FileAliasMap3(FileAliasMap3Routine fn,HANDLE section,HANDLE process,void* at,
    ULONG64 offset,SIZE_T size,ULONG flags,ULONG prot) {
  return fail(2,6)?nullptr:fn(section,process,at,offset,size,flags,prot,nullptr,0);
}
extern "C" BOOL FileAliasUnmap2(FileAliasUnmap2Routine fn,HANDLE process,void* at,ULONG flags) {
  return fail(3,7)?FALSE:fn(process,at,flags);
}
extern "C" BOOL FileAliasFree(void* at,SIZE_T size,DWORD flags) {
  if(fail(4,8))return FALSE;
  if(flags&0x1) {
    ++extra[5];
    if(mode==13 && extra[5]==nth){++extra[6];SetLastError(ERROR_NOT_ENOUGH_MEMORY);return FALSE;}
  }
  return VirtualFree(at,size,flags);
}
extern "C" BOOL FileAliasPublicationAllowed(void) { return fail(5,mode==11?11:9)?FALSE:TRUE; }
extern "C" BOOL WINAPI FileAliasUnmapViewOfFile(const void* view) {
  ++plain_unmap_calls;
  if((mode==10||mode==11)&&plain_unmap_calls==nth) {
    ++plain_unmap_injected;SetLastError(ERROR_ACCESS_DENIED);return FALSE;
  }
  return UnmapViewOfFile(view);
}

// Only linked into dedicated failure executables. Never the product allocator.
void* operator new(std::size_t bytes) {
  ++extra[0];
  if(allocation_budget==0) {
    allocation_budget=-1;++extra[1];throw std::bad_alloc();
  }
  if(allocation_budget>0)--allocation_budget;
  void* result=std::malloc(bytes?bytes:1);if(!result)throw std::bad_alloc();return result;
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value,std::size_t) noexcept { std::free(value); }
extern "C" HANDLE FileAliasCreateMapping(HANDLE file,DWORD protection) {
  ++extra[3];return CreateFileMappingW(file,nullptr,protection,0,0,nullptr);
}
extern "C" BOOL FileAliasDuplicate(HANDLE source,HANDLE* target) {
  ++extra[4];return DuplicateHandle(GetCurrentProcess(),source,GetCurrentProcess(),target,0,FALSE,DUPLICATE_SAME_ACCESS);
}
extern "C" void* FileAliasReserve(FileAliasAlloc2Routine fn,HANDLE process,void* at,SIZE_T size,ULONG flags,ULONG prot) {
  ++extra[2];return fn(process,at,size,flags,prot,nullptr,0);
}

extern "C" HANDLE FileAliasReOpenFile(HANDLE file,DWORD access,DWORD share,DWORD flags) {
  ++extra[7];return ReOpenFile(file,access,share,flags);
}
