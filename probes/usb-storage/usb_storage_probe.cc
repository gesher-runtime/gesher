// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include "../../compat/usb_storage/host_disk.h"

#include <charconv>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <limits>
#include <string_view>

namespace {

using gesher::usb_storage::Access;
using gesher::usb_storage::HostDisk;
using gesher::usb_storage::ListDisks;
using gesher::usb_storage::kMaxTransfer;
bool ParseNumber(std::wstring_view argument, unsigned long long& value) {
  char ascii[20];
  if (argument.empty() || argument.size() > sizeof(ascii) ||
      argument.find_first_not_of(L"0123456789") != std::wstring_view::npos) {
    return false;
  }
  for (size_t i = 0; i < argument.size(); ++i) {
    ascii[i] = static_cast<char>(argument[i]);
  }
  return std::from_chars(ascii, ascii + argument.size(), value).ec == std::errc{};
}

int Fail(const wchar_t* operation, DWORD error) {
  std::fwprintf(stderr, L"%ls failed (Windows error %lu)\n", operation, error);
  return 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  const bool list = argc == 2 && std::wstring_view(argv[1]) == L"--list";
  unsigned long long offset = 0;
  unsigned long long length = 0;
  if (!list && (argc != 5 || std::wstring_view(argv[1]) != L"--read" ||
                !ParseNumber(argv[3], offset) || !ParseNumber(argv[4], length) ||
                offset > static_cast<unsigned long long>(std::numeric_limits<LONGLONG>::max()) ||
                length == 0 || length > kMaxTransfer)) {
    std::fwprintf(stderr, L"Usage: usb_storage_probe --list | --read <device-path> <offset> <bytes>\n");
    return 2;
  }

  if (list) {
    std::vector<std::wstring> paths;
    DWORD error = ListDisks(&paths);
    if (error != ERROR_SUCCESS) return Fail(L"Disk enumeration", error);
    if (_setmode(_fileno(stdout), _O_U8TEXT) < 0) {
      return Fail(L"Device list output", ERROR_WRITE_FAULT);
    }
    for (const auto& path : paths) {
      if (std::wprintf(L"%ls\n", path.c_str()) < 0) {
        return Fail(L"Device list output", ERROR_WRITE_FAULT);
      }
    }
    return std::fflush(stdout) == 0 ? 0 : Fail(L"Device list output", ERROR_WRITE_FAULT);
  }
  std::unique_ptr<HostDisk> disk;
  DWORD error = HostDisk::Open(argv[2], Access::kReadOnly, &disk);
  if (error != ERROR_SUCCESS) return Fail(L"Read-only USB storage open", error);
  if (offset % disk->info().sector_bytes != 0 || length % disk->info().sector_bytes != 0) {
    return Fail(L"USB storage read", ERROR_INVALID_PARAMETER);
  }
  std::vector<std::byte> bytes(static_cast<size_t>(length));
  error = disk->Read(offset, bytes);
  if (error != ERROR_SUCCESS) return Fail(L"USB storage read", error);
  if (_setmode(_fileno(stdout), _O_BINARY) < 0 ||
      std::fwrite(bytes.data(), 1, bytes.size(), stdout) != bytes.size() ||
      std::fflush(stdout) != 0) {
    return Fail(L"USB storage read output", ERROR_WRITE_FAULT);
  }
  return 0;
}
