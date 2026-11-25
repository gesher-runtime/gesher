// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Fatal native diagnostics for this standalone probe only. No recovery or
// synthetic success: the previous filter/default termination still runs.
// https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-setunhandledexceptionfilter
// https://learn.microsoft.com/en-us/windows/win32/api/dbghelp/nf-dbghelp-stackwalk64
#pragma once

#include <windows.h>
#include <dbghelp.h>
#include <stdint.h>
#include <stdio.h>

namespace native_exception_probe {

inline LPTOP_LEVEL_EXCEPTION_FILTER previous_filter = nullptr;
inline LONG reporting = 0;

// Write directly: a fault can interrupt another thread while it owns the
// probe's stdio or log mutex. All diagnostic text uses bounded stack buffers.
inline void Write(const char* text, size_t size) {
  HANDLE output = GetStdHandle(STD_ERROR_HANDLE);
  while (size != 0 && output && output != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    if (!WriteFile(output, text, static_cast<DWORD>(size), &written, nullptr) || written == 0)
      break;
    text += written;
    size -= written;
  }
}

inline DWORD64 WINAPI ModuleBase(HANDLE, DWORD64 address) {
  MEMORY_BASIC_INFORMATION memory{};
  if (VirtualQuery(reinterpret_cast<const void*>(static_cast<uintptr_t>(address)),
                   &memory, sizeof(memory)) != sizeof(memory) || memory.Type != MEM_IMAGE)
    return 0;
  return reinterpret_cast<uintptr_t>(memory.AllocationBase);
}

inline PVOID WINAPI FunctionTable(HANDLE, DWORD64 address) {
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_ARM64) || defined(__aarch64__)
  DWORD64 base = 0;
  return RtlLookupFunctionEntry(address, &base, nullptr);
#else
  return nullptr;
#endif
}

inline void Address(const char* label, unsigned index, DWORD64 address, DWORD64 stack = 0) {
  const DWORD64 base = ModuleBase(nullptr, address);
  wchar_t path[1024]{};
  const DWORD length = base ? GetModuleFileNameW(
      reinterpret_cast<HMODULE>(static_cast<uintptr_t>(base)), path,
      static_cast<DWORD>(sizeof(path) / sizeof(path[0]))) : 0;
  char module[256] = "<unmapped>";
  if (length && length < sizeof(path) / sizeof(path[0])) {
    size_t begin = 0;
    for (size_t i = 0; i < length; ++i) if (path[i] == L'\\' || path[i] == L'/') begin = i + 1;
    size_t count = 0;
    while (begin < length && count + 1 < sizeof(module)) {
      wchar_t c = path[begin++];
      module[count++] = c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '?';
    }
    module[count] = '\0';
  }
  char line[512];
  const int length_out = snprintf(line, sizeof(line),
      "native-exception: %s[%u] pc=0x%016llx sp=0x%016llx module=%s base=0x%016llx rva=0x%llx\r\n",
      label, index, static_cast<unsigned long long>(address), static_cast<unsigned long long>(stack),
      module, static_cast<unsigned long long>(base),
      static_cast<unsigned long long>(base ? address - base : 0));
  if (length_out > 0 && static_cast<size_t>(length_out) < sizeof(line))
    Write(line, static_cast<size_t>(length_out));
}

inline LONG WINAPI OnException(EXCEPTION_POINTERS* information) {
  if (InterlockedCompareExchange(&reporting, 1, 0) == 0 && information &&
      information->ExceptionRecord && information->ContextRecord) {
    const EXCEPTION_RECORD& exception = *information->ExceptionRecord;
    char line[512];
    const int length = snprintf(line, sizeof(line),
        "native-exception: code=0x%08lx flags=0x%08lx thread=%lu parameters=%lu access=0x%llx target=0x%llx\r\n",
        static_cast<unsigned long>(exception.ExceptionCode),
        static_cast<unsigned long>(exception.ExceptionFlags),
        static_cast<unsigned long>(GetCurrentThreadId()),
        static_cast<unsigned long>(exception.NumberParameters),
        static_cast<unsigned long long>(exception.NumberParameters > 0 ? exception.ExceptionInformation[0] : 0),
        static_cast<unsigned long long>(exception.NumberParameters > 1 ? exception.ExceptionInformation[1] : 0));
    if (length > 0 && static_cast<size_t>(length) < sizeof(line)) Write(line, static_cast<size_t>(length));
    Address("fault", 0, reinterpret_cast<uintptr_t>(exception.ExceptionAddress));

    // The exception's context is never modified. StackWalk64 reads the actual
    // faulting thread, with the OS runtime table and mapped image bases; no
    // symbol download, stack scanning or guessed caller addresses are used.
    CONTEXT context = *information->ContextRecord;
    STACKFRAME64 frame{};
    DWORD machine = 0;
#if defined(_M_X64) || defined(__x86_64__)
    machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = context.Rip;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrFrame.Offset = context.Rbp;
    const int registers = snprintf(line, sizeof(line),
        "native-exception: rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx rsi=0x%llx rdi=0x%llx r8=0x%llx r9=0x%llx\r\n",
        static_cast<unsigned long long>(context.Rax), static_cast<unsigned long long>(context.Rbx),
        static_cast<unsigned long long>(context.Rcx), static_cast<unsigned long long>(context.Rdx),
        static_cast<unsigned long long>(context.Rsi), static_cast<unsigned long long>(context.Rdi),
        static_cast<unsigned long long>(context.R8), static_cast<unsigned long long>(context.R9));
    if (registers > 0 && static_cast<size_t>(registers) < sizeof(line))
      Write(line, static_cast<size_t>(registers));
#elif defined(_M_ARM64) || defined(__aarch64__)
    machine = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset = context.Pc;
    frame.AddrStack.Offset = context.Sp;
    frame.AddrFrame.Offset = context.Fp;
#endif
    frame.AddrPC.Mode = frame.AddrStack.Mode = frame.AddrFrame.Mode = AddrModeFlat;
    if (machine) {
      Address("frame", 0, frame.AddrPC.Offset, frame.AddrStack.Offset);
      DWORD64 last_pc = frame.AddrPC.Offset, last_sp = frame.AddrStack.Offset;
      for (unsigned index = 1; index < 48; ++index) {
        if (!StackWalk64(machine, GetCurrentProcess(), GetCurrentThread(), &frame, &context,
                         nullptr, FunctionTable, ModuleBase, nullptr) || !frame.AddrPC.Offset)
          break;
        if (frame.AddrPC.Offset == last_pc && frame.AddrStack.Offset == last_sp) continue;
        Address("frame", index, frame.AddrPC.Offset, frame.AddrStack.Offset);
        last_pc = frame.AddrPC.Offset;
        last_sp = frame.AddrStack.Offset;
      }
    }
  }
  // Preserve the original fatal result; this diagnostic never resumes a fault
  // or converts a crash into a successful fixture observation.
  return previous_filter ? previous_filter(information) : EXCEPTION_CONTINUE_SEARCH;
}

inline void Install() { previous_filter = SetUnhandledExceptionFilter(OnException); }

// A separate, explicit subprocess control for the diagnostic itself. This code
// is distinguishable from a real access violation and must terminate nonzero.
inline int SelfTest() {
  if (!SetThreadErrorMode(GetThreadErrorMode() | SEM_NOGPFAULTERRORBOX, nullptr)) return 1;
  const ULONG_PTR parameters[] = {0x47534852, 0x53544143};
  RaiseException(0xe0470001, EXCEPTION_NONCONTINUABLE, 2, parameters);
  return 1;
}

}  // namespace native_exception_probe
