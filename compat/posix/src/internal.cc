#include "internal.h"

#include <io.h>

namespace gesher {

std::wstring Widen(const char* utf8) {
  if (utf8 == nullptr || *utf8 == '\0') return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
  std::wstring out(n > 0 ? n - 1 : 0, L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), n);
  return out;
}

std::string Narrow(const wchar_t* utf16) {
  if (utf16 == nullptr || *utf16 == L'\0') return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, utf16, -1, nullptr, 0, nullptr, nullptr);
  std::string out(n > 0 ? n - 1 : 0, '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, utf16, -1, out.data(), n, nullptr, nullptr);
  return out;
}

int ErrnoFromWin32(DWORD error) {
  switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_MOD_NOT_FOUND:
    case ERROR_PROC_NOT_FOUND:
      return ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_PRIVILEGE_NOT_HELD:
      return EACCES;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
      return EEXIST;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case ERROR_COMMITMENT_LIMIT:
      return ENOMEM;
    case ERROR_INVALID_HANDLE:
      return EBADF;
    case ERROR_DIR_NOT_EMPTY:
      return ENOTEMPTY;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
      return ENOSPC;
    case ERROR_BROKEN_PIPE:
    case ERROR_NO_DATA:
      return EPIPE;
    case ERROR_DIRECTORY:
      return ENOTDIR;
    case ERROR_FILENAME_EXCED_RANGE:
      return ENAMETOOLONG;
    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED:
      return ENOSYS;
    case ERROR_TIMEOUT:
      return ETIMEDOUT;
    default:
      return EINVAL;
  }
}

HANDLE HandleFromFd(int fd) {
  return reinterpret_cast<HANDLE>(_get_osfhandle(fd));
}

}  // namespace gesher
