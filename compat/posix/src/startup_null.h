// Exact startup NUL object validation. It conveys no Binder authority.
#pragma once
#include <windows.h>
#include <winternl.h>
#include <cstdint>
#include <cstring>

namespace gesher {
inline HANDLE DuplicateVerifiedNull(HANDLE inherited) {
  HANDLE candidate = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), inherited, GetCurrentProcess(), &candidate,
                       0, FALSE, DUPLICATE_SAME_ACCESS)) return nullptr;
  using Query = NTSTATUS (NTAPI*)(HANDLE, OBJECT_INFORMATION_CLASS, PVOID, ULONG, PULONG);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  auto query = ntdll ? reinterpret_cast<Query>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryObject"))) : nullptr;
  alignas(UNICODE_STRING) unsigned char buffer[4096]{}; ULONG returned = 0;
  bool valid = GetFileType(candidate) == FILE_TYPE_CHAR && query &&
      query(candidate, static_cast<OBJECT_INFORMATION_CLASS>(1), buffer, sizeof(buffer), &returned) >= 0 &&
      returned >= sizeof(UNICODE_STRING) && returned <= sizeof(buffer);
  if (valid) {
    auto* name = reinterpret_cast<UNICODE_STRING*>(buffer);
    auto begin = reinterpret_cast<uintptr_t>(buffer), text = reinterpret_cast<uintptr_t>(name->Buffer);
    valid = name->Length % sizeof(wchar_t) == 0 && name->Length <= name->MaximumLength &&
        text >= begin && text - begin <= returned && text % alignof(wchar_t) == 0 &&
        name->MaximumLength <= returned - (text - begin);
    if (valid) {
      static constexpr wchar_t expected[] = L"\\Device\\Null";
      valid = name->Length == sizeof(expected) - sizeof(wchar_t) &&
          std::memcmp(name->Buffer, expected, name->Length) == 0;
    }
  }
  if (valid) return candidate;
  CloseHandle(candidate);
  return nullptr;
}
}  // namespace gesher
