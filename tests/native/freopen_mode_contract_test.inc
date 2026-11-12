// Public stdio contracts shared by Linux, the extracted host/guest adapters,
// and the real Android ELF fixtures. All files belong to this test's cwd.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef FREOPEN_CONTRACT_CALL
#define FREOPEN_CONTRACT_CALL freopen
#endif
#ifndef FREOPEN64_CONTRACT_CALL
#define FREOPEN64_CONTRACT_CALL freopen64
#endif

typedef FILE* (*freopen_contract_fn)(const char*, const char*, FILE*);

static FILE* freopen_contract_source(char* path) {
  int fd = mkstemp(path);
  if (fd < 0) return NULL;
  FILE* stream = fdopen(fd, "w+");
  if (!stream) close(fd);
  return stream;
}

static void test_freopen_exclusive(freopen_contract_fn reopen_call) {
  const char* modes[] = {"wx", "w+x", "wbx", "w+bx", "wex", "w+tex"};
  const char marker[] = "existing destination must survive";
  for (size_t index = 0; index < sizeof(modes) / sizeof(modes[0]); ++index) {
    char destination[] = "gesher-freopen-existing-XXXXXX";
    int fd = mkstemp(destination);
    check("freopen exclusive destination fixture opens", fd >= 0);
    if (fd < 0) continue;
    int prepared = write(fd, marker, sizeof(marker)) == (ssize_t)sizeof(marker);
    prepared = close(fd) == 0 && prepared;
    check("freopen exclusive destination holds original bytes", prepared);
    if (!prepared) { unlink(destination); continue; }
    char source[] = "gesher-freopen-source-XXXXXX";
    FILE* stream = freopen_contract_source(source);
    check("freopen exclusive source fixture opens", stream != NULL);
    if (!stream) { unlink(destination); unlink(source); continue; }
    int original_fd = fileno(stream);
    errno = 0;
    FILE* result = reopen_call(destination, modes[index], stream);
    int error = errno;
    check("freopen exclusive existing destination fails with EEXIST",
          result == NULL && error == EEXIST);
    if (result) {
      fclose(result);
    } else {
      errno = 0;
      check("freopen failed exclusive open closes original stream",
            fcntl(original_fd, F_GETFD) == -1 && errno == EBADF);
    }
    char contents[sizeof(marker) + 1];
    memset(contents, 0, sizeof(contents));
    int witness = open(destination, O_RDONLY);
    ssize_t got = witness >= 0 ? read(witness, contents, sizeof(contents)) : -1;
    check("freopen failed exclusive open preserves all destination bytes",
          got == (ssize_t)sizeof(marker) && memcmp(contents, marker, sizeof(marker)) == 0);
    if (witness >= 0) close(witness);
    unlink(source);
    unlink(destination);
  }
}

static void test_freopen_creation(freopen_contract_fn reopen_call) {
  const char* modes[] = {"w+x", "w+xe", "w+tbxe"};
  const unsigned char payload[] = {'A', '\n', '\r', '\n', 0x1a, 0, 'Z'};
  for (size_t index = 0; index < sizeof(modes) / sizeof(modes[0]); ++index) {
    char destination[] = "gesher-freopen-create-XXXXXX";
    int reserved = mkstemp(destination);
    check("freopen new destination name is reserved", reserved >= 0);
    if (reserved < 0) continue;
    close(reserved);
    if (unlink(destination) != 0) { check("freopen new destination is absent", 0); continue; }
    char source[] = "gesher-freopen-source-XXXXXX";
    FILE* stream = freopen_contract_source(source);
    check("freopen creation source fixture opens", stream != NULL);
    if (!stream) { unlink(source); continue; }
    FILE* result = reopen_call(destination, modes[index], stream);
    check("freopen exclusive absent destination creates a usable stream", result != NULL);
    if (result) {
      int expected = strchr(modes[index], 'e') ? FD_CLOEXEC : 0;
      check("freopen exclusive creation preserves requested CLOEXEC state",
            fcntl(fileno(result), F_GETFD) == expected);
      unsigned char actual[sizeof(payload)];
      memset(actual, 0xff, sizeof(actual));
      int written = fwrite(payload, 1, sizeof(payload), result) == sizeof(payload);
      int positioned = fseek(result, 0, SEEK_SET) == 0;
      size_t got = fread(actual, 1, sizeof(actual), result);
      check("freopen update stream preserves binary bytes including newline and control-Z",
            written && positioned && got == sizeof(payload) &&
            memcmp(actual, payload, sizeof(payload)) == 0);
      check("freopen created stream closes", fclose(result) == 0);
    }
    unlink(source);
    unlink(destination);
  }
}

static void test_freopen_cloexec(freopen_contract_fn reopen_call) {
  const char* modes[] = {"re", "r", "r+e", "r+", "ae", "a", "a+e", "a+",
                         "we", "w", "w+e", "w+"};
  char destination[] = "gesher-freopen-flags-XXXXXX";
  int reserved = mkstemp(destination);
  check("freopen flag destination fixture opens", reserved >= 0);
  if (reserved < 0) return;
  close(reserved);
  char source[] = "gesher-freopen-source-XXXXXX";
  FILE* stream = freopen_contract_source(source);
  check("freopen flag source fixture opens", stream != NULL);
  if (!stream) { unlink(source); unlink(destination); return; }
  for (size_t index = 0; index < sizeof(modes) / sizeof(modes[0]); ++index) {
    stream = reopen_call(destination, modes[index], stream);
    check("freopen ordinary mode opens existing destination", stream != NULL);
    if (!stream) break;
    int expected = strchr(modes[index], 'e') ? FD_CLOEXEC : 0;
    check("freopen adds or removes CLOEXEC as requested by the new mode",
          fcntl(fileno(stream), F_GETFD) == expected);
  }
  if (stream) fclose(stream);
  unlink(source);
  unlink(destination);
}

static void test_freopen_mode_contracts(void) {
  freopen_contract_fn routes[] = {FREOPEN_CONTRACT_CALL, FREOPEN64_CONTRACT_CALL};
  for (size_t index = 0; index < sizeof(routes) / sizeof(routes[0]); ++index) {
    test_freopen_exclusive(routes[index]);
    test_freopen_creation(routes[index]);
    test_freopen_cloexec(routes[index]);
  }
}
