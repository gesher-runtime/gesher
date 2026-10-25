// Gesher (devices track): camera_metadata_compat.h.

#include "camera_metadata_compat.h"

#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

int gesher_camera_metadata_dprintf(int fd, const char* format, ...) {
  char buffer[1024];
  va_list args;
  va_start(args, format);
  int n = vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  if (n <= 0) return n;
  size_t length = (size_t)n < sizeof(buffer) ? (size_t)n : sizeof(buffer) - 1;
  return (int)write(fd, buffer, length);
}
