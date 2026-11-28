// An inherited NUL handle backs CRT descriptors in an AppContainer.
#pragma once

#include <windows.h>
#include <winternl.h>

#include <cstdint>
#include <cstring>

namespace gesher {

// An inert CRT reservation, never a real application /dev/null descriptor.
HANDLE OpenDescriptorPlaceholder(bool inheritable = false);
HANDLE OpenNullDevice(DWORD access, bool inheritable);

inline HANDLE DuplicateVerifiedNull(HANDLE inherited) {
  HANDLE candidate = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), inherited, GetCurrentProcess(), &candidate,
                       0, FALSE, DUPLICATE_SAME_ACCESS)) {
    return nullptr;
  }
  using QueryObject = NTSTATUS(NTAPI*)(HANDLE, OBJECT_INFORMATION_CLASS, PVOID, ULONG, PULONG);
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  const auto query = ntdll == nullptr ? nullptr : reinterpret_cast<QueryObject>(
      reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryObject")));
  alignas(UNICODE_STRING) unsigned char buffer[4096]{};
  ULONG returned = 0;
  // The object manager's ObjectNameInformation distinguishes NUL from console devices.
  constexpr auto kObjectNameInformation = static_cast<OBJECT_INFORMATION_CLASS>(1);
  if (GetFileType(candidate) == FILE_TYPE_CHAR && query != nullptr &&
      query(candidate, kObjectNameInformation, buffer, sizeof(buffer), &returned) >= 0 &&
      returned >= sizeof(UNICODE_STRING) && returned <= sizeof(buffer)) {
    const auto& name = *reinterpret_cast<const UNICODE_STRING*>(buffer);
    const auto start = reinterpret_cast<uintptr_t>(buffer);
    const auto text = reinterpret_cast<uintptr_t>(name.Buffer);
    constexpr wchar_t kNullDevice[] = L"\\Device\\Null";
    constexpr size_t kNameLength = sizeof(kNullDevice) - sizeof(wchar_t);
    if (name.Length == kNameLength && name.Length <= name.MaximumLength && text >= start &&
        text - start <= returned && name.MaximumLength <= returned - (text - start) &&
        text % alignof(wchar_t) == 0 && std::memcmp(name.Buffer, kNullDevice, kNameLength) == 0) {
      return candidate;
    }
  }
  CloseHandle(candidate);
  return nullptr;
}

}  // namespace gesher
