// gesher-base: symbolic links on disk, as Linux code sees them (ADR 0045).
//
// NTFS symbolic links and junctions are reparse points; lstat reports them as
// S_IFLNK and readlink returns their target as a Linux path. symlink,
// readlink and the rest of the path calls are in fsops.cc.
#include <errno.h>
#include <string.h>
#include <windows.h>
#include <winioctl.h>

#include <string>

#include "internal.h"

namespace {

// REPARSE_DATA_BUFFER (ntifs.h, not in the SDK's user-mode headers).
struct ReparseData {
  ULONG tag;
  USHORT data_length;
  USHORT reserved;
  USHORT substitute_offset;
  USHORT substitute_length;
  USHORT print_offset;
  USHORT print_length;
  // Symbolic links have a ULONG of flags here; junctions start their names.
};
constexpr ULONG kSymlinkFlagRelative = 1;

}  // namespace

namespace gesher {

bool IsSymlinkHandle(HANDLE h) {
  FILE_ATTRIBUTE_TAG_INFO tag;
  if (!GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof(tag))) return false;
  return (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
         (tag.ReparseTag == IO_REPARSE_TAG_SYMLINK || tag.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT);
}

bool ReadSymlinkHandle(HANDLE h, std::string* target) {
  alignas(8) unsigned char buffer[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
  DWORD n = 0;
  if (!DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, nullptr, 0, buffer, sizeof(buffer), &n,
                       nullptr)) {
    if (GetLastError() == ERROR_NOT_A_REPARSE_POINT) return errno = EINVAL, false;
    return FailWithLastError(), false;
  }
  const ReparseData* data = reinterpret_cast<const ReparseData*>(buffer);
  const unsigned char* names = buffer + sizeof(ReparseData);
  bool relative = false;
  if (data->tag == IO_REPARSE_TAG_SYMLINK) {
    ULONG flags;
    memcpy(&flags, names, sizeof(flags));
    relative = (flags & kSymlinkFlagRelative) != 0;
    names += sizeof(ULONG);
  } else if (data->tag != IO_REPARSE_TAG_MOUNT_POINT) {
    return errno = EINVAL, false;
  }
  // The print name is the target as it was given; the substitute name, an NT
  // path (\??\C:\...), when there is no print name.
  USHORT offset = data->print_length != 0 ? data->print_offset : data->substitute_offset;
  USHORT length = data->print_length != 0 ? data->print_length : data->substitute_length;
  if (names + offset + length > buffer + n) return errno = EIO, false;
  std::wstring name(reinterpret_cast<const wchar_t*>(names + offset), length / sizeof(wchar_t));
  if (name.rfind(L"\\??\\UNC\\", 0) == 0) {
    name = L"\\\\" + name.substr(8);
  } else if (name.rfind(L"\\??\\", 0) == 0) {
    name = name.substr(4);
  }
  if (relative) {
    for (wchar_t& c : name) {
      if (c == L'\\') c = L'/';
    }
    *target = Narrow(name.c_str());
  } else {
    *target = LinuxPath(name.c_str());
  }
  return true;
}

}  // namespace gesher
