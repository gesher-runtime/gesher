// Explicit Linux-only Win32 boundary for the complete ELF loader contract test.
// The production parser, graph, relocation, rewrite and TLS sources are unchanged.
#pragma once
#ifdef _WIN32
#error "The ELF host boundary must never replace real Windows headers"
#endif
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <cstring>
#define WINAPI
#define CALLBACK
#define TRUE 1
#define FALSE 0
#define _strdup strdup
using BOOL = int;
using DWORD = uint32_t;
using DWORD64 = uint64_t;
using LONG = int32_t;
using ULONG_PTR = uintptr_t;
using SIZE_T = size_t;
using PVOID = void*;
using HANDLE = void*;
#define INVALID_HANDLE_VALUE reinterpret_cast<HANDLE>(intptr_t{-1})
struct INIT_ONCE { std::once_flag once; };
using PINIT_ONCE = INIT_ONCE*;
#define INIT_ONCE_STATIC_INIT {}
using InitCallback = BOOL (*)(PINIT_ONCE, PVOID, PVOID*);
using FlsCallback = void (*)(void*);
using SRWLOCK = std::shared_mutex;
#define SRWLOCK_INIT {}
constexpr DWORD TLS_OUT_OF_INDEXES = 0xffffffffu, FLS_OUT_OF_INDEXES = 0xffffffffu;
constexpr DWORD MEM_COMMIT = 0x1000, MEM_RESERVE = 0x2000, MEM_DECOMMIT = 0x4000;
constexpr DWORD MEM_RELEASE = 0x8000, MEM_FREE = 0x10000, MEM_PRIVATE = 0x20000;
constexpr DWORD PAGE_NOACCESS = 1, PAGE_READONLY = 2, PAGE_READWRITE = 4, PAGE_WRITECOPY = 8;
constexpr DWORD PAGE_EXECUTE_READ = 0x20, PAGE_EXECUTE_READWRITE = 0x40;
constexpr DWORD PAGE_EXECUTE_WRITECOPY = 0x80, PAGE_GUARD = 0x100;
constexpr DWORD FILE_MAP_READ = 4, FILE_MAP_WRITE = 2;
constexpr DWORD EXCEPTION_ACCESS_VIOLATION = 0xc0000005, EXCEPTION_BREAKPOINT = 0x80000003;
constexpr DWORD EXCEPTION_ILLEGAL_INSTRUCTION = 0xc000001d;
constexpr LONG EXCEPTION_CONTINUE_EXECUTION = -1, EXCEPTION_CONTINUE_SEARCH = 0;
struct NT_TIB { void* StackLimit = nullptr; };
struct MEMORY_BASIC_INFORMATION {
  void* BaseAddress = nullptr;
  size_t RegionSize = 0;
  DWORD State = 0, Protect = 0, Type = MEM_PRIVATE;
};
struct CONTEXT {
  DWORD64 Rax = 0, Rcx = 0, Rdx = 0, Rbx = 0, Rsp = 0, Rbp = 0, Rsi = 0, Rdi = 0;
  DWORD64 R8 = 0, R9 = 0, R10 = 0, R11 = 0, R12 = 0, R13 = 0, R14 = 0, R15 = 0, Rip = 0;
  DWORD EFlags = 0;
};
struct EXCEPTION_RECORD {
  DWORD ExceptionCode = 0, NumberParameters = 0;
  void* ExceptionAddress = nullptr;
  ULONG_PTR ExceptionInformation[15]{};
};
struct EXCEPTION_POINTERS { EXCEPTION_RECORD* ExceptionRecord; CONTEXT* ContextRecord; };
struct LARGE_INTEGER { int64_t QuadPart; };
struct SYSTEM_INFO { DWORD dwAllocationGranularity = 65536; };
DWORD GetCurrentThreadId();
unsigned long GetCurrentProcessId();  // Win32 %lu formatting on this host
DWORD TlsAlloc();
BOOL TlsFree(DWORD index);
void* TlsGetValue(DWORD index);
BOOL TlsSetValue(DWORD index, void* value);
DWORD FlsAlloc(FlsCallback callback);
void* FlsGetValue(DWORD index);
BOOL FlsSetValue(DWORD index, void* value);
BOOL InitOnceExecuteOnce(PINIT_ONCE once, InitCallback callback, PVOID param, PVOID* context);
void* VirtualAlloc(void* address, size_t size, DWORD type, DWORD protection);
BOOL VirtualFree(void* address, size_t size, DWORD type);
BOOL VirtualProtect(void* address, size_t size, DWORD protection, DWORD* old);
size_t VirtualQuery(void* address, MEMORY_BASIC_INFORMATION* info, size_t size);
void GetCurrentThreadStackLimits(ULONG_PTR* low, ULONG_PTR* high);
void* NtCurrentTeb();
void* _aligned_malloc(size_t size, size_t align);
void _aligned_free(void* address);
void AcquireSRWLockExclusive(SRWLOCK* lock);
void ReleaseSRWLockExclusive(SRWLOCK* lock);
void AcquireSRWLockShared(SRWLOCK* lock);
void ReleaseSRWLockShared(SRWLOCK* lock);
LONG InterlockedIncrement(volatile LONG* value);
PVOID AddVectoredExceptionHandler(ULONG_PTR, LONG (*handler)(EXCEPTION_POINTERS*));
HANDLE GetCurrentProcess();
BOOL FlushInstructionCache(HANDLE, const void* address, size_t size);
BOOL ReadProcessMemory(HANDLE, const void* source, void* target, size_t size, SIZE_T* copied);
HANDLE CreateFileMappingW(HANDLE, void*, DWORD, DWORD, DWORD, const wchar_t*);
void* MapViewOfFile(HANDLE, DWORD, DWORD, DWORD, SIZE_T);
BOOL UnmapViewOfFile(void*);
BOOL CloseHandle(HANDLE);
BOOL FlushViewOfFile(const void*, SIZE_T);
BOOL GetFileSizeEx(HANDLE, LARGE_INTEGER*);
void GetSystemInfo(SYSTEM_INFO*);

namespace elf_host_platform {
size_t LiveReservations();
size_t ReservedBytes();
void FailNextVirtualProtect();
// No Windows VEH, TEB-based execution, stack growth or file-view semantics are
// emulated. Tests run ordinary x86 code and use the production copy mapping path.
}
