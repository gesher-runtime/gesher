// gesher-base internals shared by the implementation files.
#pragma once

#include <errno.h>
#include <windows.h>

#include <string>

namespace gesher {

// POSIX paths are UTF-8; Windows APIs take UTF-16.
std::wstring Widen(const char* utf8);
std::string Narrow(const wchar_t* utf16);

// Maps a Win32 error code to the closest errno value.
int ErrnoFromWin32(DWORD error);

// Sets errno from GetLastError() and returns -1.
inline int FailWithLastError() {
  errno = ErrnoFromWin32(GetLastError());
  return -1;
}

// The Win32 handle behind a CRT file descriptor, or INVALID_HANDLE_VALUE.
HANDLE HandleFromFd(int fd);

}  // namespace gesher
