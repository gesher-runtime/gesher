// Reopen only an already possessed directory-list capability, with no path input.
#pragma once

#include <windows.h>
#include <winternl.h>
#include <cstring>
#include <string>

namespace gesher::directory_reopen {

inline bool Identity(HANDLE file, FILE_ID_INFO* id) {
  if (GetFileInformationByHandleEx(file, FileIdInfo, id, sizeof(*id))) return true;
  const DWORD error = GetLastError();
  if (error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED && error != ERROR_INVALID_FUNCTION)
    return false;
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(file, &info)) return false;
  *id = {};
  id->VolumeSerialNumber = info.dwVolumeSerialNumber;
  const unsigned long long index = (static_cast<unsigned long long>(info.nFileIndexHigh) << 32) |
      info.nFileIndexLow;
  std::memcpy(id->FileId.Identifier, &index, sizeof(index));
  return true;
}

// A single existing child, resolved by the current token under a held directory.
// No traversal, reparse following, creation, or token substitution is permitted.
inline DWORD OpenChild(HANDLE directory, const std::wstring& leaf, HANDLE* result,
                       bool deny_write = false) {
  *result = INVALID_HANDLE_VALUE;
  if (leaf.empty() || leaf == L"." || leaf == L".." ||
      leaf.find_first_of(L"\\/:\0", 0, 4) != std::wstring::npos || leaf.size() > 32767)
    return ERROR_INVALID_NAME;
  using Create = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                                 PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
  using Translate = ULONG(WINAPI*)(NTSTATUS);
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  const auto create = reinterpret_cast<Create>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtCreateFile")));
  const auto translate = reinterpret_cast<Translate>(
      reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlNtStatusToDosError")));
  if (!create || !translate) return ERROR_NOT_SUPPORTED;
  UNICODE_STRING name{static_cast<USHORT>(leaf.size() * sizeof(wchar_t)),
                      static_cast<USHORT>(leaf.size() * sizeof(wchar_t)), const_cast<wchar_t*>(leaf.data())};
  OBJECT_ATTRIBUTES attributes{};
  InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, directory, nullptr);
  IO_STATUS_BLOCK io{};
  // Read-data access makes the deny-write share check effective; attributes
  // alone do not participate in Windows read/write sharing accounting.
  const DWORD access = FILE_READ_ATTRIBUTES | SYNCHRONIZE | (deny_write ? FILE_READ_DATA : 0);
  const NTSTATUS status = create(result, access, &attributes, &io,
      nullptr, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_DELETE | (deny_write ? 0 : FILE_SHARE_WRITE),
      FILE_OPEN, FILE_NON_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
  if (status < 0) { *result = INVALID_HANDLE_VALUE; return translate(status); }
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(*result, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    CloseHandle(*result); *result = INVALID_HANDLE_VALUE; return ERROR_ACCESS_DENIED;
  }
  return ERROR_SUCCESS;
}

inline DWORD Open(HANDLE source, HANDLE* result, bool overlapped = false) {
  *result = INVALID_HANDLE_VALUE;
  constexpr DWORD access = FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
  if (GetFileType(source) != FILE_TYPE_DISK) return ERROR_INVALID_HANDLE;
  using Query = NTSTATUS(NTAPI*)(HANDLE, OBJECT_INFORMATION_CLASS, PVOID, ULONG, PULONG);
  const auto query = reinterpret_cast<Query>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject")));
  PUBLIC_OBJECT_BASIC_INFORMATION basic{};
  ULONG size = 0;
  if (!query || query(source, ObjectBasicInformation, &basic, sizeof(basic), &size) < 0)
    return ERROR_INVALID_HANDLE;
  if ((basic.GrantedAccess & access) != access) return ERROR_ACCESS_DENIED;
  BY_HANDLE_FILE_INFORMATION before{};
  if (!GetFileInformationByHandle(source, &before)) return GetLastError();
  if (!(before.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return ERROR_DIRECTORY;
  if (before.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_ACCESS_DENIED;
  FILE_ID_INFO identity{};
  if (!Identity(source, &identity)) return GetLastError();

  // An empty name relative to the source opens a new file object for that
  // exact directory. ReOpenFile can reject handles opened relative to a pinned
  // root by the storage broker, even with the same granted access.
  using Create = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                                 PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
  using Translate = ULONG(WINAPI*)(NTSTATUS);
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  const auto create = reinterpret_cast<Create>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtCreateFile")));
  const auto translate = reinterpret_cast<Translate>(
      reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlNtStatusToDosError")));
  if (!create || !translate) return ERROR_NOT_SUPPORTED;
  wchar_t empty[] = L"";
  UNICODE_STRING name{0, 0, empty};
  OBJECT_ATTRIBUTES attributes{};
  InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, source, nullptr);
  IO_STATUS_BLOCK io{};
  HANDLE reopened = INVALID_HANDLE_VALUE;
  const NTSTATUS status = create(&reopened, access, &attributes, &io, nullptr, FILE_ATTRIBUTE_NORMAL,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
      FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT | (overlapped ? 0 : FILE_SYNCHRONOUS_IO_NONALERT), nullptr, 0);
  if (status < 0) return translate(status);
  BY_HANDLE_FILE_INFORMATION after{};
  FILE_ID_INFO actual{};
  if (!GetFileInformationByHandle(reopened, &after) ||
      !(after.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
      (after.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !Identity(reopened, &actual) ||
      identity.VolumeSerialNumber != actual.VolumeSerialNumber ||
      std::memcmp(identity.FileId.Identifier, actual.FileId.Identifier, sizeof(actual.FileId.Identifier)) != 0) {
    CloseHandle(reopened);
    return ERROR_INVALID_HANDLE;
  }
  *result = reopened;
  return ERROR_SUCCESS;
}

}  // namespace gesher::directory_reopen
