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

inline bool ReadMemory(uintptr_t address, void* value, size_t size) {
  SIZE_T read = 0;
  return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                           value, size, &read) && read == size;
}

// Match the faulting loaded image to a retained diagnostic DLL and its public
// PDB. ReadProcessMemory checks every read; corrupt/unavailable metadata never
// becomes a reason to dereference an unchecked address in the fault handler.
// https://learn.microsoft.com/en-us/windows/win32/debug/pe-format#debug-directory-image-only
inline void ImageIdentity(DWORD64 address) {
  const uintptr_t base = static_cast<uintptr_t>(ModuleBase(nullptr, address));
  if (!base) return;
  wchar_t path[1024]{};
  const DWORD length = GetModuleFileNameW(reinterpret_cast<HMODULE>(base), path,
      static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
  if (length && length < sizeof(path) / sizeof(path[0])) {
    char line[1100] = "native-exception: module-path=";
    size_t size = sizeof("native-exception: module-path=") - 1;
    for (size_t i = 0; i < length; ++i) {
      // The qualification runner's installed runtime path is ASCII. A path
      // outside that domain is explicitly uncopyable rather than misidentified.
      if (path[i] < 0x20 || path[i] >= 0x7f) { size = 0; break; }
      line[size++] = static_cast<char>(path[i]);
    }
    if (size) { line[size++] = '\r'; line[size++] = '\n'; Write(line, size); }
  }
  IMAGE_DOS_HEADER dos{};
  IMAGE_NT_HEADERS64 image{};
  if (!ReadMemory(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
      dos.e_lfanew <= 0 || base > UINTPTR_MAX - static_cast<uintptr_t>(dos.e_lfanew) ||
      !ReadMemory(base + static_cast<uintptr_t>(dos.e_lfanew), &image, sizeof(image)) ||
      image.Signature != IMAGE_NT_SIGNATURE || image.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      image.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
      !image.OptionalHeader.SizeOfImage) return;
  const DWORD image_size = image.OptionalHeader.SizeOfImage;
  if (base > UINTPTR_MAX - image_size || image_size < sizeof(image) ||
      static_cast<uintptr_t>(dos.e_lfanew) > image_size - sizeof(image)) return;
  char line[512];
  const int length_out = snprintf(line, sizeof(line),
      "native-exception: image base=0x%llx timestamp=0x%08lx size=0x%lx machine=0x%04x\r\n",
      static_cast<unsigned long long>(base), static_cast<unsigned long>(image.FileHeader.TimeDateStamp),
      static_cast<unsigned long>(image_size), static_cast<unsigned>(image.FileHeader.Machine));
  if (length_out > 0 && static_cast<size_t>(length_out) < sizeof(line)) Write(line, length_out);
  if (image.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG) return;
  const auto& directory = image.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
  if (!directory.VirtualAddress || directory.Size % sizeof(IMAGE_DEBUG_DIRECTORY) != 0 ||
      directory.VirtualAddress > image_size || directory.Size > image_size - directory.VirtualAddress ||
      directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY) > 32) return;
  for (size_t offset = 0; offset < directory.Size; offset += sizeof(IMAGE_DEBUG_DIRECTORY)) {
    IMAGE_DEBUG_DIRECTORY debug{};
    if (!ReadMemory(base + directory.VirtualAddress + offset, &debug, sizeof(debug))) return;
    struct CodeView { DWORD signature; GUID guid; DWORD age; };
    CodeView codeview{};
    if (debug.Type != IMAGE_DEBUG_TYPE_CODEVIEW || debug.SizeOfData <= sizeof(codeview) ||
        debug.AddressOfRawData > image_size || debug.SizeOfData > image_size - debug.AddressOfRawData ||
        !ReadMemory(base + debug.AddressOfRawData, &codeview, sizeof(codeview)) ||
        codeview.signature != 0x53445352) continue;  // RSDS
    char pdb[260]{};
    size_t path_bytes = debug.SizeOfData - sizeof(codeview);
    if (path_bytes > sizeof(pdb)) path_bytes = sizeof(pdb);
    if (!ReadMemory(base + debug.AddressOfRawData + sizeof(codeview), pdb, path_bytes)) continue;
    size_t end = 0, begin = 0;
    for (; end < path_bytes && pdb[end]; ++end) {
      if (pdb[end] < 0x20 || static_cast<unsigned char>(pdb[end]) >= 0x7f) break;
      if (pdb[end] == '\\' || pdb[end] == '/') begin = end + 1;
    }
    if (end == path_bytes || pdb[end] || end == begin) continue;
    const GUID& id = codeview.guid;
    const int count = snprintf(line, sizeof(line),
        "native-exception: codeview pdb=%s key=%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%lX\r\n",
        pdb + begin, static_cast<unsigned long>(id.Data1), static_cast<unsigned>(id.Data2),
        static_cast<unsigned>(id.Data3), static_cast<unsigned>(id.Data4[0]), static_cast<unsigned>(id.Data4[1]),
        static_cast<unsigned>(id.Data4[2]), static_cast<unsigned>(id.Data4[3]), static_cast<unsigned>(id.Data4[4]),
        static_cast<unsigned>(id.Data4[5]), static_cast<unsigned>(id.Data4[6]), static_cast<unsigned>(id.Data4[7]),
        static_cast<unsigned long>(codeview.age));
    if (count > 0 && static_cast<size_t>(count) < sizeof(line)) Write(line, count);
    return;
  }
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
    ImageIdentity(reinterpret_cast<uintptr_t>(exception.ExceptionAddress));

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
