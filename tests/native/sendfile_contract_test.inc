// Public sendfile behavior shared by Linux, the extracted production bridge,
// and the Android ELF libc fixture.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef SENDFILE_CONTRACT_CALL
#define SENDFILE_CONTRACT_CALL sendfile
#endif
static ssize_t (*volatile sendfile_contract_call)(int, int, off_t*, size_t) = SENDFILE_CONTRACT_CALL;

static int sendfile_contract_fill(int fd, const unsigned char* bytes, size_t count) {
  size_t done = 0;
  while (done < count) {
    ssize_t wrote = write(fd, bytes + done, count - done);
    if (wrote <= 0) return 0;
    done += (size_t)wrote;
  }
  return 1;
}
static int sendfile_contract_contents(int fd, off_t start, const unsigned char* bytes, size_t count) {
  unsigned char buffer[257];
  size_t done = 0;
  while (done < count) {
    size_t want = count - done < sizeof(buffer) ? count - done : sizeof(buffer);
    ssize_t got = pread(fd, buffer, want, start + (off_t)done);
    if (got <= 0 || memcmp(buffer, bytes + done, (size_t)got) != 0) return 0;
    done += (size_t)got;
  }
  return 1;
}
static int64_t sendfile_contract_now_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
  return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static int sendfile_contract_socket_pair(int pair[2]) {
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return 0;
  int capacity = 4096;
  for (int side = 0; side < 2; ++side) {
    int flags = fcntl(pair[side], F_GETFL);
    if (flags < 0 || fcntl(pair[side], F_SETFL, flags | O_NONBLOCK) != 0 ||
        setsockopt(pair[side], SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity)) != 0 ||
        setsockopt(pair[side], SOL_SOCKET, SO_RCVBUF, &capacity, sizeof(capacity)) != 0) {
      close(pair[0]); close(pair[1]); return 0;
    }
  }
  return 1;
}
static void test_sendfile_blocked(int input, int explicit_offset) {
  int pair[2];
  int opened = sendfile_contract_socket_pair(pair);
  check("sendfile blocked socket fixture opens", opened);
  if (!opened) return;
  unsigned char filler[4096];
  memset(filler, 0xd3, sizeof(filler));
  int blocked = 0, state_ok = 1;
  size_t filled = 0;
  int64_t start = sendfile_contract_now_ms();
  // Loopback transport can move queued bytes into the peer without a user
  // read. Saturate both queues before requiring a no-progress EAGAIN result.
  for (int attempt = 0; attempt < 4096 && !blocked && state_ok; ++attempt) {
    int64_t now = sendfile_contract_now_ms();
    if (start < 0 || now < 0 || now - start > 3000 || filled >= 8u * 1024u * 1024u) break;
    ssize_t wrote = write(pair[0], filler, sizeof(filler));
    if (wrote > 0) { filled += (size_t)wrote; continue; }
    if (wrote == -1 && errno == EINTR) continue;
    if (wrote != -1 || (errno != EAGAIN && errno != EWOULDBLOCK)) { state_ok = 0; break; }
    off_t offset = 23;
    off_t cursor = explicit_offset ? 7 : offset;
    if (lseek(input, cursor, SEEK_SET) != cursor) { state_ok = 0; break; }
    errno = 0;
    ssize_t sent = sendfile_contract_call(pair[0], input, explicit_offset ? &offset : NULL, 65536);
    int error = errno;
    if (sent == -1 && (error == EAGAIN || error == EWOULDBLOCK)) {
      blocked = 1;
      state_ok = offset == 23 && lseek(input, 0, SEEK_CUR) == cursor;
    } else if (sent > 0 && sent <= 65536) {
      state_ok = explicit_offset
          ? offset == 23 + sent && lseek(input, 0, SEEK_CUR) == cursor
          : offset == 23 && lseek(input, 0, SEEK_CUR) == cursor + sent;
      filled += (size_t)sent;
    } else {
      state_ok = 0;
    }
  }
  check(explicit_offset ? "sendfile initial EAGAIN preserves explicit offset and source cursor"
                        : "sendfile initial EAGAIN consumes no implicit source bytes",
        blocked && state_ok);
  close(pair[0]); close(pair[1]);
}
static void test_sendfile_nonblocking_payload(int input, const unsigned char* payload,
                                             size_t length, int explicit_offset) {
  int pair[2];
  int opened = sendfile_contract_socket_pair(pair);
  check("sendfile nonblocking payload fixture opens", opened);
  if (!opened) return;
  const off_t first = 23;
  const off_t cursor = explicit_offset ? 7 : first;
  const size_t wanted = length - (size_t)first;
  off_t offset = first;
  size_t committed = 0, received = 0;
  int calls = 0, valid = lseek(input, cursor, SEEK_SET) == cursor;
  unsigned char received_bytes[4096];
  int64_t start = sendfile_contract_now_ms();
  for (int attempt = 0; attempt < 2048 && valid && received < wanted; ++attempt) {
    int64_t now = sendfile_contract_now_ms();
    if (start < 0 || now < 0 || now - start > 5000) break;
    if (committed < wanted) {
      size_t remaining = wanted - committed;
      size_t count = remaining < 65536 ? remaining : 65536;
      errno = 0;
      ssize_t sent = sendfile_contract_call(pair[0], input, explicit_offset ? &offset : NULL, count);
      int error = errno;
      ++calls;
      if (sent > 0 && (size_t)sent <= count) committed += (size_t)sent;
      else if (sent != -1 || (error != EAGAIN && error != EWOULDBLOCK && error != EINTR)) valid = 0;
      if (explicit_offset) {
        valid = valid && offset == first + (off_t)committed && lseek(input, 0, SEEK_CUR) == cursor;
      } else {
        valid = valid && lseek(input, 0, SEEK_CUR) == first + (off_t)committed;
      }
    }
    // Verify received bytes, not just the return value or source position.
    // Every retry must continue the same prefix without omissions or repeats.
    for (int drain = 0; drain < 64 && valid; ++drain) {
      ssize_t got = read(pair[1], received_bytes, sizeof(received_bytes));
      if (got > 0) {
        if ((size_t)got > committed - received ||
            memcmp(received_bytes, payload + (size_t)first + received, (size_t)got) != 0) {
          valid = 0; break;
        }
        received += (size_t)got;
      } else if (got == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        break;
      } else if (got != -1 || errno != EINTR) {
        valid = 0; break;
      }
    }
    if (valid && received < wanted) {
      struct pollfd readiness = {committed > received ? pair[1] : pair[0],
                                 (short)(committed > received ? POLLIN : POLLOUT), 0};
      if (poll(&readiness, 1, 10) < 0 && errno != EINTR) valid = 0;
    }
  }
  check(explicit_offset ? "sendfile nonblocking explicit retries deliver the exact payload once"
                        : "sendfile nonblocking implicit retries deliver the exact payload once",
        valid && calls > 1 && committed == wanted && received == wanted);
  close(pair[0]); close(pair[1]);
}
static void test_sendfile_contracts(void) {
  const size_t length = 131089;
  unsigned char* payload = (unsigned char*)malloc(length);
  char input_path[] = "gesher-sendfile-in-XXXXXX";
  char output_path[] = "gesher-sendfile-out-XXXXXX";
  int input = -1, output = -1, readonly_output = -1, alias = -1;
  off_t offset = 0;
  ssize_t result = 0;
  if (payload == NULL) { check("sendfile payload allocation", 0); return; }
  for (size_t i = 0; i < length; ++i)
    payload[i] = (unsigned char)((i * 37u + (i >> 8) * 13u + (i >> 16) * 83u) & 255);
  input = mkstemp(input_path);
  output = mkstemp(output_path);
  check("sendfile ordinary file fixture opens", input >= 0 && output >= 0);
  if (input < 0 || output < 0) goto cleanup;
  check("sendfile ordinary file fixture fills", sendfile_contract_fill(input, payload, length));
  check("sendfile implicit source position is established", lseek(input, 13, SEEK_SET) == 13);
  result = sendfile_contract_call(output, input, NULL, length - 29);
  check("sendfile implicit copy reports and advances only transferred bytes",
        result == (ssize_t)(length - 29) && lseek(input, 0, SEEK_CUR) == 13 + result);
  check("sendfile multi-chunk implicit payload is complete",
        result == (ssize_t)(length - 29) &&
        sendfile_contract_contents(output, 0, payload + 13, (size_t)result));

  check("sendfile explicit fixture resets", ftruncate(output, 0) == 0 &&
        lseek(output, 0, SEEK_SET) == 0 && lseek(input, 7, SEEK_SET) == 7);
  offset = 19;
  result = sendfile_contract_call(output, input, &offset, length - 31);
  check("sendfile explicit offset advances while ordinary input position stays unchanged",
        result == (ssize_t)(length - 31) && offset == 19 + result &&
        lseek(input, 0, SEEK_CUR) == 7);
  check("sendfile multi-chunk explicit payload is complete",
        result == (ssize_t)(length - 31) &&
        sendfile_contract_contents(output, 0, payload + 19, (size_t)result));

  readonly_output = open(output_path, O_RDONLY);
  check("sendfile write-error fixture opens read-only destination", readonly_output >= 0);
  if (readonly_output >= 0) {
    lseek(input, 17, SEEK_SET); errno = 0;
    result = sendfile_contract_call(readonly_output, input, NULL, 65536);
    check("sendfile failed output write consumes no implicit input",
          result == -1 && errno == EBADF && lseek(input, 0, SEEK_CUR) == 17);
    offset = 31; errno = 0;
    result = sendfile_contract_call(readonly_output, input, &offset, 65536);
    check("sendfile failed output write preserves explicit offset and input position",
          result == -1 && errno == EBADF && offset == 31 && lseek(input, 0, SEEK_CUR) == 17);
  }
  offset = -1; errno = 0;
  result = sendfile_contract_call(output, input, &offset, 1);
  check("sendfile negative explicit offset fails without mutation",
        result == -1 && errno == EINVAL && offset == -1 && lseek(input, 0, SEEK_CUR) == 17);
  {
    const struct { off_t start; size_t count; } invalid[] = {
      {0, SIZE_MAX}, {0, (size_t)INT64_MAX + 1}, {1, (size_t)INT64_MAX},
      {INT64_MAX - 0x7ffff000, 0x7ffff001},
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      offset = invalid[i].start; errno = 0;
      result = sendfile_contract_call(output, input, &offset, invalid[i].count);
      check("sendfile validates original count/range before transfer-size clamping",
            result == -1 && errno == EINVAL && offset == invalid[i].start &&
            lseek(input, 0, SEEK_CUR) == 17);
    }
    offset = INT64_MAX - 0x7ffff000;
    check("sendfile representable exact-cap EOF succeeds",
          sendfile_contract_call(output, input, &offset, 0x7ffff000) == 0 &&
          offset == INT64_MAX - 0x7ffff000);
    offset = 0;
    check("sendfile valid signed-maximum count retains available progress",
          lseek(output, 0, SEEK_SET) == 0 &&
          sendfile_contract_call(output, input, &offset, (size_t)INT64_MAX) == (ssize_t)length &&
          offset == (off_t)length && lseek(input, 0, SEEK_CUR) == 17 &&
          sendfile_contract_contents(output, 0, payload, length));
  }
  // A high EOF seek allocates no giant file, including on Windows.
  offset = ((off_t)1 << 33) + 17;
  check("sendfile source cursor remains 64-bit above four GiB",
        lseek(input, offset, SEEK_SET) == offset &&
        sendfile_contract_call(output, input, NULL, 31) == 0 &&
        lseek(input, 0, SEEK_CUR) == offset);
  lseek(input, 7, SEEK_SET);
  check("sendfile high explicit EOF preserves both positions",
        sendfile_contract_call(output, input, &offset, 31) == 0 &&
        offset == ((off_t)1 << 33) + 17 && lseek(input, 0, SEEK_CUR) == 7);

  for (int explicit_offset = 0; explicit_offset < 2; ++explicit_offset) {
    test_sendfile_blocked(input, explicit_offset);
    test_sendfile_nonblocking_payload(input, payload, length, explicit_offset);
  }

  // Output writes legitimately move a shared cursor even with explicit input offset.
  alias = dup(input);
  check("sendfile alias fixture duplicates input", alias >= 0);
  if (alias >= 0) {
    lseek(input, 5, SEEK_SET);
    result = sendfile_contract_call(alias, input, NULL, 7);
    check("sendfile implicit duplicate uses one shared cursor advance",
          result == 7 && lseek(input, 0, SEEK_CUR) == 12 && lseek(alias, 0, SEEK_CUR) == 12);
    check("sendfile implicit alias preserves the copied bytes",
          sendfile_contract_contents(input, 5, payload + 5, 7));
    lseek(input, 5, SEEK_SET); offset = 19;
    result = sendfile_contract_call(alias, input, &offset, 7);
    check("sendfile explicit alias advances output cursor and supplied offset separately",
          result == 7 && offset == 26 && lseek(input, 0, SEEK_CUR) == 12);
    check("sendfile explicit alias copies the requested source bytes",
          sendfile_contract_contents(input, 5, payload + 19, 7));
  }
cleanup:
  if (alias >= 0) close(alias);
  if (readonly_output >= 0) close(readonly_output);
  if (output >= 0) { close(output); unlink(output_path); }
  if (input >= 0) { close(input); unlink(input_path); }
  free(payload);
}
