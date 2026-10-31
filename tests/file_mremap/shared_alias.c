/* Slice A ordinary-file fixture. Compile/run in CI only, on Linux and the
 * genuine Gesher host/guest roles. Unimplemented resize families are not PASS. */
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int run_case(const char* id, int writable, size_t offset) {
  char path[] = "file-mremap-a-XXXXXX";
  char other[] = "file-mremap-other-XXXXXX";
  long native_page = sysconf(_SC_PAGESIZE);
  size_t page = native_page > 0 ? (size_t)native_page : 0;
  int fd = -1, reused = -1, replacement = -1, setup = 0, equal = 0;
  int original_number = -1, fd_reused = 0;
  int copied = 0, forward = 0, reverse = 0, source_alive = 0, error = 0;
  int release_alias = -1, release_source = -1, release_peer = -1;
  int close_original = -1, close_reused = -1, close_replacement = -1;
  int remove_original = -1, remove_other = -1;
  unsigned char *source = MAP_FAILED, *alias = MAP_FAILED, *peer = MAP_FAILED;
  int prot = writable ? PROT_READ | PROT_WRITE : PROT_READ;
  size_t i;
  if (!page || offset % page) goto cleanup;
  fd = mkstemp(path);
  if (fd < 0 || ftruncate(fd, (off_t)(offset + page * 2)) != 0) goto cleanup;
  peer = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)offset);
  if (peer == MAP_FAILED) goto cleanup;
  memset(peer, 0x31, page);
  source = mmap(NULL, page, prot, MAP_SHARED, fd, (off_t)offset);
  if (source == MAP_FAILED) goto cleanup;
  equal = mremap(source, page, page, 0) == source;
  original_number = fd;
  close_original = close(fd); fd = -1;
  replacement = mkstemp(other);
  if (replacement < 0 || ftruncate(replacement, (off_t)page) != 0) goto cleanup;
  /* Make descriptor substitution observable without reopening source by path. */
  if (replacement == original_number) {
    reused = dup(replacement);
    fd_reused = reused >= 0;
  } else {
    reused = dup2(replacement, original_number);
    fd_reused = reused == original_number;
  }
  if (reused < 0 || !fd_reused) goto cleanup;
  setup = 1;
  errno = 0;
  alias = mremap(source, 0, page, MREMAP_MAYMOVE);
  error = alias == MAP_FAILED ? errno : 0;
  if (alias == MAP_FAILED) goto cleanup;
  copied = alias != source;
  for (i = 0; i < page; ++i) copied = copied && alias[i] == 0x31;
  source_alive = source[0] == 0x31 && source[page - 1] == 0x31;
  if (writable) { alias[37] = 0x5a; forward = peer[37] == 0x5a && source[37] == 0x5a; }
  else forward = alias[37] == peer[37];
  peer[43] = 0x6b;
  reverse = alias[43] == 0x6b && source[43] == 0x6b;
cleanup:
  if (alias != MAP_FAILED) release_alias = munmap(alias, page);
  if (source != MAP_FAILED) release_source = munmap(source, page);
  if (peer != MAP_FAILED) release_peer = munmap(peer, page);
  if (fd >= 0) close_original = close(fd);
  if (reused >= 0) close_reused = close(reused);
  if (replacement >= 0) close_replacement = close(replacement);
  remove_original = unlink(path);
  remove_other = unlink(other);
  printf("{\"schema\":1,\"case\":\"%s\",\"page\":%zu,\"offset\":%zu,"
         "\"setup\":%d,\"fd_reused\":%d,\"equal\":%d,\"distinct_full_pattern\":%d,\"forward\":%d,"
         "\"reverse\":%d,\"source_alive\":%d,\"errno\":%d,"
         "\"release_alias\":%d,\"release_source\":%d,\"release_peer\":%d,"
         "\"close_original\":%d,\"close_reused\":%d,\"close_replacement\":%d,"
         "\"remove_original\":%d,\"remove_other\":%d}\n", id, page, offset,
         setup, fd_reused, equal, copied, forward, reverse, source_alive, error,
         release_alias, release_source, release_peer, close_original, close_reused,
         close_replacement, remove_original, remove_other);
  return !(setup && fd_reused && equal && copied && forward && reverse && source_alive &&
           release_alias == 0 && release_source == 0 && release_peer == 0 &&
           close_original == 0 && close_reused == 0 && close_replacement == 0 &&
           remove_original == 0 && remove_other == 0);
}
int main(void) {
  long page = sysconf(_SC_PAGESIZE);
  int failed = run_case("FS-ALIAS-RW", 1, 0);
  failed += run_case("FS-ALIAS-RO", 0, 0);
  failed += run_case("FS-ALIAS-OFFSET", 1, page > 0 ? (size_t)page : 0);
  return failed ? 1 : 0;
}
