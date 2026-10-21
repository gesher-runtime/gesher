// gesher-base: Linux interfaces with little or no Windows equivalent —
// termios on the console, prctl, capabilities, extended attributes, the
// mount table and sendfile.
#include <errno.h>
#include <mntent.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/prctl.h>
#include <sys/sendfile.h>
#include <sys/xattr.h>
#include <termios.h>
#include <unistd.h>

#include "internal.h"

namespace {

// The console input behind fd, which may be the console's input or output;
// nullptr (errno set) if fd is not a console. *owned is set when the handle
// was opened here and must be closed.
HANDLE ConsoleInput(int fd, bool* owned) {
  *owned = false;
  HANDLE h = gesher::HandleFromFd(fd);
  if (h == INVALID_HANDLE_VALUE) return errno = EBADF, nullptr;
  DWORD mode, events;
  if (!GetConsoleMode(h, &mode)) return errno = ENOTTY, nullptr;
  if (GetNumberOfConsoleInputEvents(h, &events)) return h;
  // A console output: the terminal's settings are its input's.
  h = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                  nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return errno = ENOTTY, nullptr;
  *owned = true;
  return h;
}

struct ConsoleHandle {
  explicit ConsoleHandle(int fd) : handle(ConsoleInput(fd, &owned)) {}
  ~ConsoleHandle() {
    if (owned) CloseHandle(handle);
  }
  HANDLE handle;
  bool owned;
};

}  // namespace

extern "C" {

// --- termios ----------------------------------------------------------------------
//
// A Windows console is the only terminal: ECHO, ICANON and ISIG map to its
// input mode. Other fds are not terminals (ENOTTY).

int tcgetattr(int fd, struct termios* t) {
  ConsoleHandle console(fd);
  if (console.handle == nullptr) return -1;
  DWORD mode;
  if (!GetConsoleMode(console.handle, &mode)) return gesher::FailWithLastError();
  memset(t, 0, sizeof(*t));
  if (mode & ENABLE_ECHO_INPUT) t->c_lflag |= ECHO;
  if (mode & ENABLE_LINE_INPUT) t->c_lflag |= ICANON;
  if (mode & ENABLE_PROCESSED_INPUT) t->c_lflag |= ISIG;
  return 0;
}

int tcsetattr(int fd, int action, const struct termios* t) {
  if (action != TCSANOW && action != TCSADRAIN && action != TCSAFLUSH) return errno = EINVAL, -1;
  ConsoleHandle console(fd);
  if (console.handle == nullptr) return -1;
  DWORD mode;
  if (!GetConsoleMode(console.handle, &mode)) return gesher::FailWithLastError();
  mode &= ~static_cast<DWORD>(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
  // The console only echoes in line mode, so ECHO without ICANON is dropped.
  if ((t->c_lflag & ECHO) && (t->c_lflag & ICANON)) mode |= ENABLE_ECHO_INPUT;
  if (t->c_lflag & ICANON) mode |= ENABLE_LINE_INPUT;
  if (t->c_lflag & ISIG) mode |= ENABLE_PROCESSED_INPUT;
  if (action == TCSAFLUSH) FlushConsoleInputBuffer(console.handle);
  return SetConsoleMode(console.handle, mode) ? 0 : gesher::FailWithLastError();
}

int tcdrain(int fd) {
  ConsoleHandle console(fd);
  return console.handle != nullptr ? 0 : -1;
}

int tcflush(int fd, int queue) {
  ConsoleHandle console(fd);
  if (console.handle == nullptr) return -1;
  if (queue != TCIFLUSH && queue != TCOFLUSH && queue != TCIOFLUSH) return errno = EINVAL, -1;
  if (queue != TCOFLUSH) FlushConsoleInputBuffer(console.handle);
  return 0;
}

int tcsendbreak(int fd, int) {
  ConsoleHandle console(fd);
  return console.handle != nullptr ? 0 : -1;
}

int tcflow(int fd, int) {
  ConsoleHandle console(fd);
  return console.handle != nullptr ? 0 : -1;
}

// --- prctl --------------------------------------------------------------------------

// Linux declares the arguments unsigned long, 64 bits there but 32 here
// (LLP64), so each is read as the type callers actually pass.
int prctl(int option, ...) {
  va_list args;
  va_start(args, option);
  int result;
  switch (option) {
    case PR_SET_NAME: {
      // Linux keeps the first 15 bytes.
      char name[16];
      snprintf(name, sizeof(name), "%s", va_arg(args, const char*));
      int error = pthread_setname_np(pthread_self(), name);
      result = error == 0 ? 0 : (errno = error, -1);
      break;
    }
    case PR_GET_NAME: {
      int error = pthread_getname_np(pthread_self(), va_arg(args, char*), 16);
      result = error == 0 ? 0 : (errno = error, -1);
      break;
    }
    case PR_GET_DUMPABLE:
      result = 1;
      break;
    case PR_SET_DUMPABLE:
    case PR_SET_NO_NEW_PRIVS:
      result = 0;
      break;
    case PR_CAPBSET_READ: {
      // No capability is in the bounding set (see capget).
      unsigned cap = static_cast<unsigned>(va_arg(args, int));
      result = cap <= CAP_LAST_CAP ? 0 : (errno = EINVAL, -1);
      break;
    }
    default:
      result = (errno = EINVAL, -1);
      break;
  }
  va_end(args);
  return result;
}

// --- capabilities ----------------------------------------------------------------
//
// A Windows process holds no Linux capabilities.

int capget(cap_user_header_t header, cap_user_data_t data) {
  if (header == nullptr) return errno = EFAULT, -1;
  int words;
  switch (header->version) {
    case _LINUX_CAPABILITY_VERSION_1: words = _LINUX_CAPABILITY_U32S_1; break;
    case _LINUX_CAPABILITY_VERSION_2: words = _LINUX_CAPABILITY_U32S_2; break;
    case _LINUX_CAPABILITY_VERSION_3: words = _LINUX_CAPABILITY_U32S_3; break;
    default:
      // Linux reports the version it prefers.
      header->version = _LINUX_CAPABILITY_VERSION_3;
      return errno = EINVAL, -1;
  }
  if (header->pid != 0 && header->pid != getpid()) return errno = ESRCH, -1;
  if (data != nullptr) memset(data, 0, sizeof(*data) * words);
  return 0;
}

int capset(cap_user_header_t header, const cap_user_data_t data) {
  if (header == nullptr || data == nullptr) return errno = EFAULT, -1;
  int words;
  switch (header->version) {
    case _LINUX_CAPABILITY_VERSION_1: words = _LINUX_CAPABILITY_U32S_1; break;
    case _LINUX_CAPABILITY_VERSION_2: words = _LINUX_CAPABILITY_U32S_2; break;
    case _LINUX_CAPABILITY_VERSION_3: words = _LINUX_CAPABILITY_U32S_3; break;
    default:
      header->version = _LINUX_CAPABILITY_VERSION_3;
      return errno = EINVAL, -1;
  }
  if (header->pid != 0 && header->pid != getpid()) return errno = EPERM, -1;
  // Keeping no capabilities succeeds; gaining any is not allowed.
  for (int i = 0; i < words; ++i) {
    if (data[i].effective != 0 || data[i].permitted != 0 || data[i].inheritable != 0) {
      return errno = EPERM, -1;
    }
  }
  return 0;
}

// --- extended attributes ------------------------------------------------------------

ssize_t getxattr(const char*, const char*, void*, size_t) { return errno = ENOTSUP, -1; }
ssize_t lgetxattr(const char*, const char*, void*, size_t) { return errno = ENOTSUP, -1; }
ssize_t fgetxattr(int, const char*, void*, size_t) { return errno = ENOTSUP, -1; }
int setxattr(const char*, const char*, const void*, size_t, int) { return errno = ENOTSUP, -1; }
int lsetxattr(const char*, const char*, const void*, size_t, int) { return errno = ENOTSUP, -1; }
int fsetxattr(int, const char*, const void*, size_t, int) { return errno = ENOTSUP, -1; }
ssize_t listxattr(const char*, char*, size_t) { return errno = ENOTSUP, -1; }
ssize_t llistxattr(const char*, char*, size_t) { return errno = ENOTSUP, -1; }
ssize_t flistxattr(int, char*, size_t) { return errno = ENOTSUP, -1; }
int removexattr(const char*, const char*) { return errno = ENOTSUP, -1; }
int lremovexattr(const char*, const char*) { return errno = ENOTSUP, -1; }
int fremovexattr(int, const char*) { return errno = ENOTSUP, -1; }

// --- mount table --------------------------------------------------------------------
//
// There is no mount table: setmntent opens an empty stream.

FILE* setmntent(const char*, const char* type) { return fopen("NUL", type != nullptr ? type : "r"); }

struct mntent* getmntent(FILE*) { return nullptr; }

struct mntent* getmntent_r(FILE*, struct mntent*, char*, int) { return nullptr; }

int endmntent(FILE* stream) {
  if (stream != nullptr) fclose(stream);
  return 1;
}

// The option `opt` in the comma-separated mnt_opts, alone or as opt=value.
char* hasmntopt(const struct mntent* mnt, const char* opt) {
  size_t len = strlen(opt);
  char* p = mnt->mnt_opts;
  while (p != nullptr && *p != '\0') {
    if (strncmp(p, opt, len) == 0 && (p[len] == '\0' || p[len] == ',' || p[len] == '=')) {
      return p;
    }
    p = strchr(p, ',');
    if (p != nullptr) ++p;
  }
  return nullptr;
}

// --- sendfile -----------------------------------------------------------------------

// Copied through a buffer with read and write, so out_fd may be a socket.
// With an offset, in_fd's file position is left unchanged, as on Linux.
ssize_t sendfile(int out_fd, int in_fd, off_t* offset, size_t count) {
  char buffer[64 * 1024];
  size_t total = 0;
  while (total < count) {
    size_t chunk = count - total < sizeof(buffer) ? count - total : sizeof(buffer);
    ssize_t n = offset != nullptr ? pread(in_fd, buffer, chunk, *offset)
                                  : read(in_fd, buffer, static_cast<unsigned>(chunk));
    if (n < 0) return total > 0 ? static_cast<ssize_t>(total) : -1;
    if (n == 0) break;
    ssize_t written = 0;
    while (written < n) {
      int w = write(out_fd, buffer + written, static_cast<unsigned>(n - written));
      if (w < 0) {
        if (offset != nullptr) *offset += written;
        total += static_cast<size_t>(written);
        return total > 0 ? static_cast<ssize_t>(total) : -1;
      }
      written += w;
    }
    if (offset != nullptr) *offset += n;
    total += static_cast<size_t>(n);
    if (static_cast<size_t>(n) < chunk) break;
  }
  return static_cast<ssize_t>(total);
}

}  // extern "C"
