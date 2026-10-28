// CI-only actual closer-stack diagnostic. Export/module description is reused
// from qualified ThreadWatch source blob e65d1789f8ca8be3a72e2dfffdc9376a78d0a532.
// This is evidence collection only; it never replaces the actual WCT criterion.
#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
namespace native3_stack_probe {
// The export of `module` at or before `rva` (nullptr when none).
const char* NearestExport(HMODULE module, DWORD rva, DWORD* offset) {
  const BYTE* base = reinterpret_cast<const BYTE*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (dir.VirtualAddress == 0 || dir.Size == 0) return nullptr;
  const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
  const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
  const auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
  const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
  const char* best = nullptr;
  DWORD best_rva = 0;
  for (DWORD i = 0; i < exports->NumberOfNames; i++) {
    if (ordinals[i] >= exports->NumberOfFunctions) continue;
    const DWORD f = functions[ordinals[i]];
    // A forwarder's "rva" is in the export directory itself: not code.
    if (f >= dir.VirtualAddress && f < dir.VirtualAddress + dir.Size) continue;
    if (f <= rva && (best == nullptr || f > best_rva)) {
      best_rva = f;
      best = reinterpret_cast<const char*>(base + names[i]);
    }
  }
  if (best != nullptr) *offset = rva - best_rva;
  return best;
}

std::string Describe(DWORD64 pc) {
  char line[512];
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(pc)), &module) ||
      module == nullptr) {
    snprintf(line, sizeof(line), "0x%llx", static_cast<unsigned long long>(pc));
    return line;
  }
  char path[MAX_PATH] = {0};
  GetModuleFileNameA(module, path, MAX_PATH);
  const char* name = path;
  for (const char* p = path; *p != 0; p++) {
    if (*p == 47 || *p == 92) name = p + 1;  // '/' or the backslash
  }
  const DWORD rva = static_cast<DWORD>(pc - reinterpret_cast<uintptr_t>(module));
  DWORD offset = 0;
  const char* symbol = NearestExport(module, rva, &offset);
  if (symbol != nullptr && offset < 0x10000) {
    snprintf(line, sizeof(line), "%s!%s+0x%lx (%s+0x%lx)", name, symbol,
             static_cast<unsigned long>(offset), name, static_cast<unsigned long>(rva));
  } else {
    snprintf(line, sizeof(line), "%s+0x%lx", name, static_cast<unsigned long>(rva));
  }
  return line;
}

inline uint64_t Birth(const FILETIME& value) {
  return (static_cast<uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}

// Match the original observed worker call shape using its actual bytes. An
// unwind restores nonvolatile RSI at this frame; mov RSI,RCX immediately before
// call [_lock_file IAT] makes that saved RSI the actual FILE argument. This is
// a checked call-site witness, never a nearest-export or generic-RSI inference.
inline bool ExactFileCall(DWORD64 pc, DWORD64 rsi, FILE* stream) {
  if (pc < 9 || rsi != reinterpret_cast<uintptr_t>(stream)) return false;
  HMODULE worker = GetModuleHandleW(nullptr);
  MEMORY_BASIC_INFORMATION instruction{}, import_region{};
  if (!VirtualQuery(reinterpret_cast<const void*>(pc - 9), &instruction, sizeof(instruction)) ||
      instruction.Type != MEM_IMAGE || instruction.AllocationBase != worker) return false;
  unsigned char code[9]{};
  SIZE_T copied = 0;
  if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(pc - 9), code, sizeof(code), &copied) ||
      copied != sizeof(code) || code[0] != 0x48 || code[1] != 0x89 || code[2] != 0xce ||
      code[3] != 0xff || code[4] != 0x15) return false;
  int32_t displacement = 0;
  std::memcpy(&displacement, code + 5, sizeof(displacement));
  uintptr_t slot = 0;
  if (displacement < 0) {
    uint64_t subtract = static_cast<uint64_t>(-static_cast<int64_t>(displacement));
    if (pc < subtract) return false;
    slot = pc - subtract;
  } else {
    if (pc > UINTPTR_MAX - static_cast<unsigned>(displacement)) return false;
    slot = pc + static_cast<unsigned>(displacement);
  }
  if (!VirtualQuery(reinterpret_cast<const void*>(slot), &import_region, sizeof(import_region)) ||
      import_region.Type != MEM_IMAGE || import_region.AllocationBase != worker) return false;
  uintptr_t target = 0;
  copied = 0;
  return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(slot), &target, sizeof(target), &copied) &&
      copied == sizeof(target) && target == reinterpret_cast<uintptr_t>(&_lock_file);
}

// Reuse the established Rtl unwind pattern on this exact owned thread. Only
// context/unwind/guarded reads run while suspended; description/logging follows
// checked resume. The original caller's five-second deadline keeps running.
inline bool Capture(DWORD tid, uint64_t expected_birth, FILE* stream, int fd) {
  HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
  if (!thread) {
    std::fprintf(stderr, "ACTUAL_CLOSER_STACK open-failed tid=%lu error=%lu\n", tid, GetLastError());
    return false;
  }
  FILETIME created{}, exited{}, kernel{}, user{};
  bool owned = tid != GetCurrentThreadId() && GetProcessIdOfThread(thread) == GetCurrentProcessId() &&
      GetThreadTimes(thread, &created, &exited, &kernel, &user) && expected_birth != 0 && Birth(created) == expected_birth;
  if (!owned) {
    bool closed = CloseHandle(thread);
    std::fprintf(stderr, "ACTUAL_CLOSER_STACK identity-failed tid=%lu closed=%d\n", tid, closed);
    return false;
  }
  DWORD previous = SuspendThread(thread);
  if (previous == static_cast<DWORD>(-1)) {
    DWORD error = GetLastError();
    bool closed = CloseHandle(thread);
    std::fprintf(stderr, "ACTUAL_CLOSER_STACK suspend-failed tid=%lu error=%lu closed=%d\n", tid, error, closed);
    return false;
  }
  DWORD64 pcs[32]{};
  DWORD64 rsis[32]{};
  unsigned count = 0;
  CONTEXT context{};
  context.ContextFlags = CONTEXT_FULL;
  bool captured = GetThreadContext(thread, &context);
  DWORD error = captured ? ERROR_SUCCESS : GetLastError();
  while (captured && count < 32 && context.Rip != 0) {
    pcs[count] = context.Rip;
    rsis[count++] = context.Rsi;
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
    if (function) {
      void* handler_data = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                      &handler_data, &establisher, nullptr);
    } else {
      SIZE_T copied = 0;
      if (context.Rsp == 0 || context.Rsp > UINT64_MAX - sizeof(DWORD64)) break;
      if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(context.Rsp),
                             &context.Rip, sizeof(context.Rip), &copied) || copied != sizeof(context.Rip)) {
        error = GetLastError(); captured = false; break;
      }
      context.Rsp += sizeof(DWORD64);
    }
  }
  DWORD resumed = ResumeThread(thread);
  if (previous != 0 || resumed != 1) {
    // Do not enter CRT logging or leave a thread suspended after probe failure.
    static const char note[] = "ACTUAL_CLOSER_STACK resume-ownership-failed; owned worker terminates95\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), note, sizeof(note) - 1, &written, nullptr);
    TerminateProcess(GetCurrentProcess(), 95);
    std::_Exit(95);
  }
  bool closed = CloseHandle(thread);
  std::fprintf(stderr, "ACTUAL_CLOSER_STACK pid=%lu closer=%lu birth=%llu FILE=%p fd=%d captured=%d error=%lu frames=%u suspend-before=%lu resume-before=%lu closed=%d\n",
      GetCurrentProcessId(), tid, static_cast<unsigned long long>(expected_birth), static_cast<void*>(stream), fd,
      captured, error, count, previous, resumed, closed);
  HMODULE modules[32]{};
  unsigned module_count = 0;
  unsigned exact_file_calls = 0;
  for (unsigned index = 0; index < count; ++index) {
    std::fprintf(stderr, "ACTUAL_CLOSER_FRAME index=%u pc=0x%llx nearest-export=%s\n", index,
        static_cast<unsigned long long>(pcs[index]), Describe(pcs[index]).c_str());
    if (ExactFileCall(pcs[index], rsis[index], stream)) {
      ++exact_file_calls;
      std::fprintf(stderr, "ACTUAL_CLOSER_FILE_CALL index=%u pc=0x%llx unwound-rsi=0x%llx FILE=%p exact-mov-rsi-rcx-and-lock-file-iat=true\n",
          index, static_cast<unsigned long long>(pcs[index]), static_cast<unsigned long long>(rsis[index]),
          static_cast<void*>(stream));
    }
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(pcs[index])), &module)) {
      bool seen = false;
      for (unsigned previous_module = 0; previous_module < module_count; ++previous_module)
        seen |= modules[previous_module] == module;
      if (!seen) {
        modules[module_count++] = module;
        char path[MAX_PATH]{};
        DWORD bytes = GetModuleFileNameA(module, path, MAX_PATH);
        std::fprintf(stderr, "ACTUAL_CLOSER_MODULE base=%p path=%s complete=%d\n",
            static_cast<void*>(module), path, bytes > 0 && bytes < MAX_PATH);
      }
    }
  }
  std::fprintf(stderr, "ACTUAL_CLOSER_FILE_CALL_SUMMARY matches=%u required=1\n", exact_file_calls);
  return captured && count > 0 && closed && previous == 0 && resumed == 1 && exact_file_calls == 1;
}

}  // namespace native3_stack_probe


