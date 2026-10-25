// Gesher: the pieces of Microsoft's C++ runtime that the WebView2 SDK's
// WebView2LoaderStatic.lib (built with MSVC and Chromium's libc++) refers to
// and llvm-mingw's runtime does not define (docs/webview.md, "The loader").
// Everything else it imports (the C runtime, kernel32, advapi32, ole32) is
// in UCRT and the import libraries llvm-mingw links anyway.
//
// The names are MSVC's (decorated C++ names, set with asm labels):
//   operator new/delete variants, std::nothrow, libc++'s verbose abort,
//   thread-safe static initialization (_Init_thread_*: MSVC's "magic
//   statics"), and Control Flow Guard's dispatch pointer.
// Built with -fno-emulated-tls: _Init_thread_epoch must be a native TLS
// variable, which MSVC's code reads through _tls_index and a section-relative
// offset.

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <windows.h>

extern "C" {

// operator new(size_t, const std::nothrow_t&)
void* gesher_msvc_new_nothrow(size_t size, const void*) __asm__("??2@YAPEAX_KAEBUnothrow_t@std@@@Z");
// operator delete(void*, size_t)
void gesher_msvc_delete_sized(void* p, size_t) __asm__("??3@YAXPEAX_K@Z");
// operator new[](size_t)
void* gesher_msvc_new_array(size_t size) __asm__("??_U@YAPEAX_K@Z");
// operator new[](size_t, const std::nothrow_t&)
void* gesher_msvc_new_array_nothrow(size_t size, const void*) __asm__("??_U@YAPEAX_KAEBUnothrow_t@std@@@Z");
// operator delete[](void*)
void gesher_msvc_delete_array(void* p) __asm__("??_V@YAXPEAX@Z");
// operator delete[](void*, size_t)
void gesher_msvc_delete_array_sized(void* p, size_t) __asm__("??_V@YAXPEAX_K@Z");
// std::__Cr::__libcpp_verbose_abort(const char*, ...)
[[noreturn]] void gesher_msvc_verbose_abort(const char* format, ...) __asm__("?__libcpp_verbose_abort@__Cr@std@@YAXPEBDZZ");
// const std::nothrow_t std::nothrow
extern const char gesher_msvc_nothrow __asm__("?nothrow@std@@3Unothrow_t@1@B");

void _Init_thread_header(int* once);
void _Init_thread_footer(int* once);

}  // extern "C"

extern "C" {

const char gesher_msvc_nothrow = 0;

void* gesher_msvc_new_nothrow(size_t size, const void*) { return malloc(size != 0 ? size : 1); }
void gesher_msvc_delete_sized(void* p, size_t) { free(p); }
void* gesher_msvc_new_array(size_t size) {
  void* p = malloc(size != 0 ? size : 1);
  if (p == nullptr) abort();
  return p;
}
void* gesher_msvc_new_array_nothrow(size_t size, const void*) {
  return malloc(size != 0 ? size : 1);
}
void gesher_msvc_delete_array(void* p) { free(p); }
void gesher_msvc_delete_array_sized(void* p, size_t) { free(p); }

void gesher_msvc_verbose_abort(const char* format, ...) {
  va_list args;
  va_start(args, format);
  fputs("WebView2Loader: ", stderr);
  vfprintf(stderr, format, args);
  fputc('\n', stderr);
  va_end(args);
  abort();
}

// --- Thread-safe statics (MSVC's thread_safe_statics.cpp) ------------------
// A guard is 0 before initialization, -1 during it, and afterwards the epoch
// it finished in; each thread's _Init_thread_epoch is the last epoch it saw.

int _Init_global_epoch = INT_MIN;
__thread int _Init_thread_epoch = INT_MIN;

}  // extern "C"

namespace {
SRWLOCK g_init_lock = SRWLOCK_INIT;
CONDITION_VARIABLE g_init_done = CONDITION_VARIABLE_INIT;
}  // namespace

extern "C" void _Init_thread_header(int* once) {
  AcquireSRWLockExclusive(&g_init_lock);
  if (*once == 0) {
    *once = -1;
  } else {
    while (*once == -1) {
      SleepConditionVariableSRW(&g_init_done, &g_init_lock, INFINITE, 0);
      if (*once == 0) {  // the other thread's initialization was abandoned
        *once = -1;
        ReleaseSRWLockExclusive(&g_init_lock);
        return;
      }
    }
    _Init_thread_epoch = _Init_global_epoch;
  }
  ReleaseSRWLockExclusive(&g_init_lock);
}

extern "C" void _Init_thread_footer(int* once) {
  AcquireSRWLockExclusive(&g_init_lock);
  ++_Init_global_epoch;
  *once = _Init_global_epoch;
  _Init_thread_epoch = _Init_global_epoch;
  ReleaseSRWLockExclusive(&g_init_lock);
  WakeAllConditionVariable(&g_init_done);
}

// --- Control Flow Guard -----------------------------------------------------
// MSVC's /guard:cf code calls indirect targets through
// __guard_dispatch_icall_fptr with the target in rax; without CFG checking
// that is a plain jump. Weak: mingw-w64's runtime may define it.

extern "C" void gesher_guard_dispatch_icall_nop();
__asm__(
    ".text\n"
    ".globl gesher_guard_dispatch_icall_nop\n"
    "gesher_guard_dispatch_icall_nop:\n"
    "  jmp *%rax\n");

extern "C" {
__attribute__((weak)) void (*__guard_dispatch_icall_fptr)() = gesher_guard_dispatch_icall_nop;

// /GS stack cookies (weak: mingw-w64's gs_support may define them).
__attribute__((weak)) uintptr_t __security_cookie = 0x00002B992DDFA232ull;
__attribute__((weak)) void __security_check_cookie(uintptr_t cookie) {
  if (cookie != __security_cookie) abort();
}
}  // extern "C"
