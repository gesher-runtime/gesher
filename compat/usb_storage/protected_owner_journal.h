// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "protected_profile_name.h"

namespace gesher::usb_storage {

struct ProtectedOwnerRecord {
  uint32_t magic = 0x47534f57, version = 1;
  wchar_t installation[37]{};
  wchar_t signer[65]{};
};

// The authority supplies a sealed SYSTEM/Administrators-only directory, never
// a guest path. The exclusive file lock serializes admission across processes.
// Retired and profile records are immutable; replacing the current pointer
// never erases the evidence needed to deny an earlier installation.
class ProtectedOwnerJournal {
public:
  ~ProtectedOwnerJournal() {
    if (lock_ != INVALID_HANDLE_VALUE)
      CloseHandle(lock_);
  }
  ProtectedOwnerJournal() = default;
  ProtectedOwnerJournal(const ProtectedOwnerJournal &) = delete;
  ProtectedOwnerJournal &operator=(const ProtectedOwnerJournal &) = delete;
  DWORD Open(const std::wstring &directory) {
    std::wstring absolute(32768, L'\0');
    const DWORD length =
        GetFullPathNameW(directory.c_str(), static_cast<DWORD>(absolute.size()),
                         absolute.data(), nullptr);
    if (!length || length >= absolute.size())
      return ERROR_FILENAME_EXCED_RANGE;
    absolute.resize(length);
    std::replace(absolute.begin(), absolute.end(), L'/', L'\\');
    if (absolute.size() < 3 || absolute[1] != L':' || absolute[2] != L'\\')
      return ERROR_INVALID_PARAMETER;
    directory_ = L"\\\\?\\" + absolute;
    lock_ = CreateFileW(Path(L"admission.lock").c_str(),
                        GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (lock_ == INVALID_HANDLE_VALUE)
      return GetLastError();
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(lock_, &info))
      return GetLastError();
    return (info.dwFileAttributes &
            (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
                   info.nNumberOfLinks != 1
               ? ERROR_ACCESS_DENIED
               : ERROR_SUCCESS;
  }
  DWORD Current(ProtectedOwnerRecord *record, bool *exists) const {
    return Read(L"current", record, exists);
  }
  DWORD Retired(std::wstring_view owner, bool *retired) const {
    if (!ValidInstallationOwner(owner))
      return ERROR_INVALID_PARAMETER;
    ProtectedOwnerRecord record{};
    const DWORD error =
        Read(std::wstring(owner) + L".retired", &record, retired);
    return error == ERROR_SUCCESS && *retired && owner != record.installation
               ? ERROR_INVALID_DATA
               : error;
  }
  DWORD Retire(const ProtectedOwnerRecord &record) const {
    return Immutable(std::wstring(record.installation) + L".retired", record);
  }
  DWORD Publish(const ProtectedOwnerRecord &record) const {
    if (!Valid(record))
      return ERROR_INVALID_PARAMETER;
    bool retired = false;
    DWORD status = Retired(record.installation, &retired);
    if (status != ERROR_SUCCESS || retired)
      return status != ERROR_SUCCESS ? status : ERROR_ACCESS_DENIED;
    ProtectedOwnerRecord previous{};
    bool exists = false;
    status = Current(&previous, &exists);
    if (status != ERROR_SUCCESS)
      return status;
    if (exists &&
        std::wstring_view(previous.installation) == record.installation &&
        std::wstring_view(previous.signer) != record.signer)
      return ERROR_ACCESS_DENIED;
    DWORD error = Immutable(std::wstring(record.installation) + L"." +
                                record.signer + L".profile",
                            record);
    if (error != ERROR_SUCCESS)
      return error;
    // An already-current identity is an idempotent admission, not a head
    // replacement. Keep the validated history requirement above, but avoid
    // needlessly replacing an identical head held by readers or scanners.
    if (exists &&
        std::wstring_view(previous.installation) == record.installation &&
        std::wstring_view(previous.signer) == record.signer)
      return ERROR_SUCCESS;
    BYTE nonce[16]{};
    if (BCryptGenRandom(nullptr, nonce, sizeof(nonce),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
      return ERROR_GEN_FAILURE;
    std::wstring name = L"current.tmp.";
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (BYTE value : nonce) {
      name.push_back(hex[value >> 4]);
      name.push_back(hex[value & 15]);
    }
    error = Write(name, record);
    if (error != ERROR_SUCCESS)
      return error;
    // A crash before rename leaves an ignored temporary, never a partial head.
    const ULONGLONG deadline = GetTickCount64() + 1000;
    do {
      if (MoveFileExW(Path(name).c_str(), Path(L"current").c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return ERROR_SUCCESS;
      error = GetLastError();
      // A scanner/reader may briefly prevent replacing the old head. Retry
      // only this same atomic rename while the exclusive journal lock is
      // retained. An error can be ambiguous about whether a rename committed;
      // retain all recovery evidence and fail closed after the bound.
      if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION &&
          error != ERROR_LOCK_VIOLATION)
        return error;
      Sleep(2);
    } while (GetTickCount64() < deadline);
    return error;
  }
  DWORD Profiles(std::wstring_view owner,
                 std::vector<ProtectedOwnerRecord> *records) const {
    if (!ValidInstallationOwner(owner) || !records || !records->empty())
      return ERROR_INVALID_PARAMETER;
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW(
        Path(std::wstring(owner) + L".*.profile").c_str(), &data);
    if (search == INVALID_HANDLE_VALUE)
      return GetLastError() == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS
                                                    : GetLastError();
    DWORD error = ERROR_SUCCESS;
    do {
      if (records->size() >= 256 ||
          (data.dwFileAttributes &
           (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        error = ERROR_NOT_ENOUGH_QUOTA;
        break;
      }
      ProtectedOwnerRecord record{};
      bool exists = false;
      error = Read(data.cFileName, &record, &exists);
      if (error != ERROR_SUCCESS || !exists || owner != record.installation ||
          std::wstring(data.cFileName) !=
              std::wstring(owner) + L"." + record.signer + L".profile") {
        if (error == ERROR_SUCCESS)
          error = ERROR_INVALID_DATA;
        break;
      }
      records->push_back(record);
    } while (FindNextFileW(search, &data));
    const DWORD last = GetLastError();
    FindClose(search);
    return error != ERROR_SUCCESS        ? error
           : last == ERROR_NO_MORE_FILES ? ERROR_SUCCESS
                                         : last;
  }

private:
  static bool Valid(const ProtectedOwnerRecord &record) {
    return record.magic == 0x47534f57 && record.version == 1 &&
           record.installation[36] == 0 &&
           ValidInstallationOwner(record.installation) &&
           record.signer[64] == 0 &&
           std::wstring_view(record.signer).size() == 64 &&
           std::wstring_view(record.signer)
                   .find_first_not_of(L"0123456789abcdef") ==
               std::wstring_view::npos;
  }
  std::wstring Path(std::wstring_view name) const {
    return directory_ + L"\\" + std::wstring(name);
  }
  DWORD Read(std::wstring_view name, ProtectedOwnerRecord *record,
             bool *exists) const {
    *exists = false;
    HANDLE file =
        CreateFileW(Path(name).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE)
      return GetLastError() == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS
                                                    : GetLastError();
    BY_HANDLE_FILE_INFORMATION info{};
    DWORD received = 0;
    DWORD error = ERROR_SUCCESS;
    if (!GetFileInformationByHandle(file, &info))
      error = GetLastError();
    else if ((info.dwFileAttributes &
              (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
             info.nNumberOfLinks != 1 || info.nFileSizeHigh ||
             info.nFileSizeLow != sizeof(*record))
      error = ERROR_INVALID_DATA;
    else if (!ReadFile(file, record, sizeof(*record), &received, nullptr))
      error = GetLastError();
    else if (received != sizeof(*record) || !Valid(*record))
      error = ERROR_INVALID_DATA;
    CloseHandle(file);
    if (error == ERROR_SUCCESS)
      *exists = true;
    return error;
  }
  DWORD Write(std::wstring_view name,
              const ProtectedOwnerRecord &record) const {
    if (!Valid(record))
      return ERROR_INVALID_PARAMETER;
    HANDLE file = CreateFileW(
        Path(name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE)
      return GetLastError();
    DWORD written = 0;
    DWORD error = ERROR_SUCCESS;
    if (!WriteFile(file, &record, sizeof(record), &written, nullptr) ||
        written != sizeof(record))
      error = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
    else if (!FlushFileBuffers(file))
      error = GetLastError();
    CloseHandle(file);
    return error;
  }
  DWORD Immutable(std::wstring_view name,
                  const ProtectedOwnerRecord &record) const {
    ProtectedOwnerRecord before{};
    bool present = false;
    DWORD error = Read(name, &before, &present);
    if (error != ERROR_SUCCESS)
      return error;
    if (present) {
      const bool retirement = name.ends_with(L".retired");
      return std::wstring_view(before.installation) == record.installation &&
                     (retirement ||
                      std::wstring_view(before.signer) == record.signer)
                 ? ERROR_SUCCESS
                 : ERROR_INVALID_DATA;
    }
    BYTE nonce[16]{};
    if (BCryptGenRandom(nullptr, nonce, sizeof(nonce),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
      return ERROR_GEN_FAILURE;
    std::wstring temporary = std::wstring(name) + L".tmp.";
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (BYTE value : nonce) {
      temporary.push_back(hex[value >> 4]);
      temporary.push_back(hex[value & 15]);
    }
    error = Write(temporary, record);
    if (error != ERROR_SUCCESS)
      return error;
    if (MoveFileExW(Path(temporary).c_str(), Path(name).c_str(),
                    MOVEFILE_WRITE_THROUGH))
      return ERROR_SUCCESS;
    error = GetLastError();
    if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
      return error;
    // The losing temporary remains authority-only recovery evidence.
    ProtectedOwnerRecord existing{};
    bool exists = false;
    error = Read(name, &existing, &exists);
    // A retired owner stays retired even if its verified signer later differs.
    return error != ERROR_SUCCESS ? error
           : !exists || std::wstring_view(existing.installation) !=
                            record.installation
               ? ERROR_INVALID_DATA
               : ERROR_SUCCESS;
  }
  std::wstring directory_;
  HANDLE lock_ = INVALID_HANDLE_VALUE;
};

} // namespace gesher::usb_storage
