// gesher-base: <dirent.h> over FindFirstFileW / FindNextFileW.

#include <dirent.h>

#include <errno.h>
#include <string.h>
#include <windows.h>

#include <string>

#include "internal.h"

struct gesher_DIR {
  std::wstring pattern;
  HANDLE find = INVALID_HANDLE_VALUE;
  WIN32_FIND_DATAW data;
  bool have_data = false;  // `data` holds an entry not yet returned
  long long position = 0;
  struct dirent entry;
};

namespace {

bool Start(DIR* dir) {
  dir->find = FindFirstFileExW(dir->pattern.c_str(), FindExInfoBasic, &dir->data,
                               FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
  dir->have_data = dir->find != INVALID_HANDLE_VALUE;
  dir->position = 0;
  return dir->have_data || GetLastError() == ERROR_FILE_NOT_FOUND;
}

}  // namespace

extern "C" {

DIR* opendir(const char* name) {
  std::wstring path = gesher::Widen(name);
  DWORD attrs = GetFileAttributesW(path.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    gesher::FailWithLastError();
    return nullptr;
  }
  if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    errno = ENOTDIR;
    return nullptr;
  }
  DIR* dir = new gesher_DIR();
  if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path += L'\\';
  dir->pattern = path + L'*';
  if (!Start(dir)) {
    gesher::FailWithLastError();
    delete dir;
    return nullptr;
  }
  return dir;
}

DIR* fdopendir(int) {
  errno = ENOSYS;
  return nullptr;
}

struct dirent* readdir(DIR* dir) {
  if (!dir->have_data) {
    if (dir->find == INVALID_HANDLE_VALUE || !FindNextFileW(dir->find, &dir->data)) {
      return nullptr;  // end of directory; errno unchanged
    }
  }
  dir->have_data = false;

  struct dirent* e = &dir->entry;
  std::string name = gesher::Narrow(dir->data.cFileName);
  size_t len = name.size() < sizeof(e->d_name) - 1 ? name.size() : sizeof(e->d_name) - 1;
  memcpy(e->d_name, name.data(), len);
  e->d_name[len] = '\0';
  DWORD attrs = dir->data.dwFileAttributes;
  if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
      dir->data.dwReserved0 == IO_REPARSE_TAG_SYMLINK) {
    e->d_type = DT_LNK;
  } else {
    e->d_type = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 ? DT_DIR : DT_REG;
  }
  e->d_ino = 0;
  e->d_off = ++dir->position;
  e->d_reclen = sizeof(*e);
  return e;
}

int closedir(DIR* dir) {
  if (dir->find != INVALID_HANDLE_VALUE) FindClose(dir->find);
  delete dir;
  return 0;
}

void rewinddir(DIR* dir) {
  if (dir->find != INVALID_HANDLE_VALUE) FindClose(dir->find);
  Start(dir);
}

int dirfd(DIR*) {
  errno = ENOTSUP;
  return -1;
}

}  // extern "C"
