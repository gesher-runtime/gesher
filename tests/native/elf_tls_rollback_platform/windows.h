// Host-only Win32 boundary for compiling the complete production tls.cc.
// This directory is put on the include path only by test_elf_tls_rollback.py.
#pragma once

#ifdef _WIN32
#error "The ELF TLS rollback host boundary must not replace Windows headers"
#endif

#include <cstddef>
#include <cstdint>
#include <mutex>

#define WINAPI
#define CALLBACK
#define TRUE 1

using BOOL = int;
using DWORD = uint32_t;
using ULONG_PTR = uintptr_t;
using PVOID = void*;
struct INIT_ONCE { std::once_flag once; };
using PINIT_ONCE = INIT_ONCE*;
#define INIT_ONCE_STATIC_INIT {}
using InitCallback = BOOL (*)(PINIT_ONCE, PVOID, PVOID*);
using FlsCallback = void (*)(void*);

constexpr DWORD TLS_OUT_OF_INDEXES = 0xffffffffu;
constexpr DWORD FLS_OUT_OF_INDEXES = 0xffffffffu;
constexpr DWORD MEM_COMMIT = 0x1000;
constexpr DWORD MEM_RESERVE = 0x2000;
constexpr DWORD MEM_DECOMMIT = 0x4000;
constexpr DWORD MEM_RELEASE = 0x8000;
constexpr DWORD PAGE_NOACCESS = 1;
constexpr DWORD PAGE_READWRITE = 4;
constexpr DWORD PAGE_GUARD = 0x100;

struct NT_TIB { void* StackLimit = nullptr; };
struct MEMORY_BASIC_INFORMATION {
  void* BaseAddress;
  size_t RegionSize;
  DWORD State;
  DWORD Protect;
};

DWORD GetCurrentThreadId();
DWORD TlsAlloc();
BOOL TlsFree(DWORD index);
void* TlsGetValue(DWORD index);
BOOL TlsSetValue(DWORD index, void* value);
DWORD FlsAlloc(FlsCallback callback);
void* FlsGetValue(DWORD index);
BOOL FlsSetValue(DWORD index, void* value);
BOOL InitOnceExecuteOnce(PINIT_ONCE once, InitCallback callback, PVOID param, PVOID* context);
void* VirtualAlloc(void* address, size_t size, DWORD type, DWORD protect);
BOOL VirtualFree(void* address, size_t size, DWORD type);
BOOL VirtualProtect(void* address, size_t size, DWORD protect, DWORD* old);
size_t VirtualQuery(void* address, MEMORY_BASIC_INFORMATION* info, size_t size);
void GetCurrentThreadStackLimits(ULONG_PTR* low, ULONG_PTR* high);
void* NtCurrentTeb();
void* _aligned_malloc(size_t size, size_t align);
void _aligned_free(void* address);
