// gesher-base: errno numbering (see <errno.h>). Translates the Windows
// CRT's and winpthreads' values to Linux's, and formats Linux's.
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"

extern "C" int gesher_errno_from_crt(int crt_errno) {
  // 1-34 are the same; the others are mingw-w64's <errno.h> values.
  if (crt_errno >= 0 && crt_errno <= 34) return crt_errno;
  switch (crt_errno) {
    case 36: return EDEADLK;
    case 38: return ENAMETOOLONG;
    case 39: return ENOLCK;
    case 40: return ENOSYS;
    case 41: return ENOTEMPTY;
    case 42: return EILSEQ;
    case 80: return ERANGE;  // STRUNCATE
    case 100: return EADDRINUSE;
    case 101: return EADDRNOTAVAIL;
    case 102: return EAFNOSUPPORT;
    case 103: return EALREADY;
    case 104: return EBADMSG;
    case 105: return ECANCELED;
    case 106: return ECONNABORTED;
    case 107: return ECONNREFUSED;
    case 108: return ECONNRESET;
    case 109: return EDESTADDRREQ;
    case 110: return EHOSTUNREACH;
    case 111: return EIDRM;
    case 112: return EINPROGRESS;
    case 113: return EISCONN;
    case 114: return ELOOP;
    case 115: return EMSGSIZE;
    case 116: return ENETDOWN;
    case 117: return ENETRESET;
    case 118: return ENETUNREACH;
    case 119: return ENOBUFS;
    case 120: return ENODATA;
    case 121: return ENOLINK;
    case 122: return ENOMSG;
    case 123: return ENOPROTOOPT;
    case 124: return ENOSR;
    case 125: return ENOSTR;
    case 126: return ENOTCONN;
    case 127: return ENOTRECOVERABLE;
    case 128: return ENOTSOCK;
    case 129: return ENOTSUP;
    case 130: return EOPNOTSUPP;
    case 132: return EOVERFLOW;
    case 133: return EOWNERDEAD;
    case 134: return EPROTO;
    case 135: return EPROTONOSUPPORT;
    case 136: return EPROTOTYPE;
    case 137: return ETIME;
    case 138: return ETIMEDOUT;
    case 139: return ETXTBSY;
    case 140: return EAGAIN;  // EWOULDBLOCK
    default: return EINVAL;   // 131 (EOTHER) and unknown values
  }
}

extern "C" int gesher_rmdir(const char* path) {
  std::wstring windows;
  if (!gesher::DiskPath(path, &windows, ENOTDIR)) return -1;  // ENOTDIR: a device or /proc file
  return gesher::PosixDelete(windows, true);
}

namespace {

// glibc's messages, indexed by Linux errno value.
const char* const kMessages[] = {
    "Success",
    "Operation not permitted",
    "No such file or directory",
    "No such process",
    "Interrupted system call",
    "Input/output error",
    "No such device or address",
    "Argument list too long",
    "Exec format error",
    "Bad file descriptor",
    "No child processes",
    "Resource temporarily unavailable",
    "Cannot allocate memory",
    "Permission denied",
    "Bad address",
    "Block device required",
    "Device or resource busy",
    "File exists",
    "Invalid cross-device link",
    "No such device",
    "Not a directory",
    "Is a directory",
    "Invalid argument",
    "Too many open files in system",
    "Too many open files",
    "Inappropriate ioctl for device",
    "Text file busy",
    "File too large",
    "No space left on device",
    "Illegal seek",
    "Read-only file system",
    "Too many links",
    "Broken pipe",
    "Numerical argument out of domain",
    "Numerical result out of range",
    "Resource deadlock avoided",
    "File name too long",
    "No locks available",
    "Function not implemented",
    "Directory not empty",
    "Too many levels of symbolic links",
    "Unknown error 41",
    "No message of desired type",
    "Identifier removed",
    "Channel number out of range",
    "Level 2 not synchronized",
    "Level 3 halted",
    "Level 3 reset",
    "Link number out of range",
    "Protocol driver not attached",
    "No CSI structure available",
    "Level 2 halted",
    "Invalid exchange",
    "Invalid request descriptor",
    "Exchange full",
    "No anode",
    "Invalid request code",
    "Invalid slot",
    "Unknown error 58",
    "Bad font file format",
    "Device not a stream",
    "No data available",
    "Timer expired",
    "Out of streams resources",
    "Machine is not on the network",
    "Package not installed",
    "Object is remote",
    "Link has been severed",
    "Advertise error",
    "Srmount error",
    "Communication error on send",
    "Protocol error",
    "Multihop attempted",
    "RFS specific error",
    "Bad message",
    "Value too large for defined data type",
    "Name not unique on network",
    "File descriptor in bad state",
    "Remote address changed",
    "Can not access a needed shared library",
    "Accessing a corrupted shared library",
    ".lib section in a.out corrupted",
    "Attempting to link in too many shared libraries",
    "Cannot exec a shared library directly",
    "Invalid or incomplete multibyte or wide character",
    "Interrupted system call should be restarted",
    "Streams pipe error",
    "Too many users",
    "Socket operation on non-socket",
    "Destination address required",
    "Message too long",
    "Protocol wrong type for socket",
    "Protocol not available",
    "Protocol not supported",
    "Socket type not supported",
    "Operation not supported",
    "Protocol family not supported",
    "Address family not supported by protocol",
    "Address already in use",
    "Cannot assign requested address",
    "Network is down",
    "Network is unreachable",
    "Network dropped connection on reset",
    "Software caused connection abort",
    "Connection reset by peer",
    "No buffer space available",
    "Transport endpoint is already connected",
    "Transport endpoint is not connected",
    "Cannot send after transport endpoint shutdown",
    "Too many references: cannot splice",
    "Connection timed out",
    "Connection refused",
    "Host is down",
    "No route to host",
    "Operation already in progress",
    "Operation now in progress",
    "Stale file handle",
    "Structure needs cleaning",
    "Not a XENIX named type file",
    "No XENIX semaphores available",
    "Is a named type file",
    "Remote I/O error",
    "Disk quota exceeded",
    "No medium found",
    "Wrong medium type",
    "Operation canceled",
    "Required key not available",
    "Key has expired",
    "Key has been revoked",
    "Key was rejected by service",
    "Owner died",
    "State not recoverable",
    "Operation not possible due to RF-kill",
    "Memory page has hardware error",
};
static_assert(sizeof(kMessages) / sizeof(kMessages[0]) == EHWPOISON + 1);

// Copies the message for errnum into buf; returns EINVAL for unknown values.
int Format(int errnum, char* buf, size_t buflen) {
  bool known = errnum >= 0 && errnum <= EHWPOISON;
  if (known) {
    snprintf(buf, buflen, "%s", kMessages[errnum]);
  } else {
    snprintf(buf, buflen, "Unknown error %d", errnum);
  }
  return known ? 0 : EINVAL;
}

}  // namespace

extern "C" char* gesher_strerror(int errnum) {
  static thread_local char buffer[64];
  if (errnum >= 0 && errnum <= EHWPOISON) return const_cast<char*>(kMessages[errnum]);
  Format(errnum, buffer, sizeof(buffer));
  return buffer;
}

extern "C" int gesher_strerror_s(char* buf, size_t buflen, int errnum) {
  if (buf == nullptr || buflen == 0) return EINVAL;
  Format(errnum, buf, buflen);
  return 0;
}

// GNU strerror_r, which libcore uses; the CRT has none.
extern "C" char* strerror_r(int errnum, char* buf, size_t buflen) {
  if (errnum >= 0 && errnum <= EHWPOISON) return const_cast<char*>(kMessages[errnum]);
  if (buflen > 0) Format(errnum, buf, buflen);
  return buf;
}

// XSI strerror_r, under the name glibc gives it.
extern "C" int __xpg_strerror_r(int errnum, char* buf, size_t buflen) {
  if (buflen == 0) return ERANGE;
  int result = Format(errnum, buf, buflen);
  if (result == 0 && strlen(kMessages[errnum]) >= buflen) return ERANGE;
  return result;
}
