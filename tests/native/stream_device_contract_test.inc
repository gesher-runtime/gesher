// A CRT fclose/freopen must release the old /dev/zero or /dev/urandom
// identity. Otherwise a reused ordinary FILE is still read as a device.
// Shared by the real ELF tests, an independent Linux run and the host adapter.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef STREAM_DEVICE_FOPEN
#define STREAM_DEVICE_FOPEN fopen
#define STREAM_DEVICE_FCLOSE fclose
#define STREAM_DEVICE_FREOPEN freopen
#define STREAM_DEVICE_FGETC fgetc
#define STREAM_DEVICE_FREAD fread
#endif

static void test_stream_device_contracts(void) {
  char path[] = "gesher-stream-device-XXXXXX";
  int reserved = mkstemp(path);
  check("stream device fixture opens", reserved >= 0);
  if (reserved < 0) return;
  const char contents[] = "17 !";
  int prepared = write(reserved, contents, sizeof(contents) - 1) == (ssize_t)sizeof(contents) - 1;
  prepared = close(reserved) == 0 && prepared;
  check("stream device fixture holds ordinary bytes", prepared);
  if (!prepared) { unlink(path); return; }

  const char* devices[] = {"/dev/zero", "/dev/urandom"};
  for (size_t index = 0; index < sizeof(devices) / sizeof(devices[0]); ++index) {
    for (int operation = 0; operation < 3; ++operation) {
      FILE* stream = STREAM_DEVICE_FOPEN(devices[index], "rb");
      check("stream device source opens", stream != NULL);
      if (!stream) continue;
      int old_fd = fileno(stream);
      if (operation == 0) {
        check("stream device fclose succeeds", STREAM_DEVICE_FCLOSE(stream) == 0);
        stream = STREAM_DEVICE_FOPEN(path, "rb");
      } else if (operation == 1) {
        stream = STREAM_DEVICE_FREOPEN(path, "rb", stream);
      } else {
        char missing[sizeof(path) + 16];
        snprintf(missing, sizeof(missing), "%s/child", path);
        FILE* result = STREAM_DEVICE_FREOPEN(missing, "rb", stream);
        check("stream device failed reopen reports ENOTDIR", result == NULL && errno == ENOTDIR);
        if (result) STREAM_DEVICE_FCLOSE(result);
        stream = STREAM_DEVICE_FOPEN(path, "rb");
      }
      check("stream device close/reopen leaves a reusable ordinary stream", stream != NULL);
      if (!stream) continue;
      check("stream device fixture reuses the descriptor", fileno(stream) == old_fd);
      int value = -1;
      int assigned = fscanf(stream, "%d ", &value);
      int next = STREAM_DEVICE_FGETC(stream);
      check("ordinary stream keeps scanf's unread matching-failure byte",
            assigned == 1 && value == 17 && next == '!');
      char actual[sizeof(contents)] = {0};
      int positioned = fseek(stream, 0, SEEK_SET) == 0;
      size_t count = STREAM_DEVICE_FREAD(actual, 1, sizeof(actual), stream);
      check("ordinary stream fread is not served by the previous device",
            positioned && count == sizeof(contents) - 1 &&
                memcmp(actual, contents, sizeof(contents) - 1) == 0);
      STREAM_DEVICE_FCLOSE(stream);
    }
  }
  unlink(path);
}
