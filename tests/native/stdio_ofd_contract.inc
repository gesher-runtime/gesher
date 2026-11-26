// Public stdio/OFD corpus: compiled as Windows host C++, Android ELF C and an
// independent Linux reference. Android-specific observations are named below.
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#if defined(__BIONIC__) && __ANDROID_API__ < 28
#include <dlfcn.h>
#endif

static int stdio_bytes_equal(int fd, const char* expected) {
  char bytes[128];
  size_t wanted = strlen(expected);
  ssize_t count = pread(fd, bytes, sizeof(bytes), 0);
  return count == (ssize_t)wanted && memcmp(bytes, expected, wanted) == 0;
}
static void* stdio_try_locked(void* argument) {
  FILE* stream = (FILE*)argument;
  int acquired = ftrylockfile(stream) == 0;
  if (acquired) funlockfile(stream);
  // The owner thread is only holding the lock, not accessing the buffer. This
  // call must not acquire that stream lock implicitly.
#if defined(__BIONIC__) && __ANDROID_API__ < 28
  // fwrite_unlocked was added in API 28. Keep the ELF fixture's API 26 floor
  // and require the real runtime export; absence fails the same contract.
  typedef size_t (*write_unlocked_fn)(const void*, size_t, size_t, FILE*);
  write_unlocked_fn write_unlocked = (write_unlocked_fn)dlsym(RTLD_DEFAULT, "fwrite_unlocked");
  int wrote = write_unlocked != NULL && write_unlocked("u", 1, 1, stream) == 1;
#else
  int wrote = fwrite_unlocked("u", 1, 1, stream) == 1;
#endif
  return (void*)(intptr_t)(acquired || !wrote);
}
static void* stdio_flush_all(void* unused) {
  int i;
  (void)unused;
  for (i = 0; i < 1200; ++i) if (fflush(NULL) != 0) return (void*)(intptr_t)1;
  return NULL;
}

static void test_stdio_ofd_contracts(void) {
  char path[] = "gesher-stdio-ofd-XXXXXX";
  int fd = mkstemp(path);
  int alias;
  FILE* stream;
  char buffer[128];
  char text[32];
  check("stdio fixture opens", fd >= 0);
  if (fd < 0) return;
  check("stdio initial content", write(fd, "base", 4) == 4 && lseek(fd, 0, SEEK_SET) == 0);
  alias = dup(fd);
  check("stdio duplicate descriptor", alias >= 0);
  stream = fdopen(fd, "r+");
  check("stdio fdopen transfers the existing descriptor", stream != NULL && fileno(stream) == fd);
  if (!stream) { close(fd); close(alias); unlink(path); return; }
  check("stdio installs a real caller buffer", setvbuf(stream, buffer, _IOFBF, sizeof(buffer)) == 0);
  check("stdio buffers pending output", fwrite("tail", 1, 4, stream) == 4);
  check("stdio pending output is not yet written", stdio_bytes_equal(alias, "base"));
  check("stdio alias changes append before flush", fcntl(alias, F_SETFL, O_APPEND) == 0);
  check("stdio flush consults current OFD append", fflush(stream) == 0 && stdio_bytes_equal(alias, "basetail"));
  check("stdio current position includes atomic append", ftello(stream) == 8);
  check("stdio clears append through an alias", fcntl(alias, F_SETFL, 0) == 0);
  check("stdio positions through the shared description", fseeko(stream, 0, SEEK_SET) == 0);
  check("stdio fputs and putc use the same output state", fputs("XY", stream) >= 0 && putc('Z', stream) == 'Z');
  check("stdio cleared append overwrites at the cursor", fflush(stream) == 0 && stdio_bytes_equal(alias, "XYZetail"));
  check("stdio printf family output", fprintf(stream, "%s:%d", "p", 7) == 3 && fflush(stream) == 0);
  check("stdio printf reaches the descriptor", stdio_bytes_equal(alias, "XYZp:7il"));
  check("stdio prepares buffered input", fseeko(stream, 0, SEEK_SET) == 0 && fread(text, 1, 2, stream) == 2);
  check("stdio read-ahead logical offset", memcmp(text, "XY", 2) == 0 && ftello(stream) == 2);
  check("stdio ungetc participates in offset", ungetc('q', stream) == 'q' && ftello(stream) == 1);
  check("stdio ungetc precedes old read-ahead", getc(stream) == 'q' && getc(stream) == 'Z');
  check("stdio seek-current accounts for read-ahead", fseeko(stream, 1, SEEK_CUR) == 0 && ftello(stream) == 4);
  check("stdio write after a positioning barrier", fputc('!', stream) == '!' && fflush(stream) == 0);
  check("stdio read/write transition writes at logical position", stdio_bytes_equal(alias, "XYZp!7il"));
  check("stdio seek supports a large file offset", fseeko(stream, INT64_C(5) * 1024 * 1024 * 1024, SEEK_SET) == 0);
  check("stdio tell preserves all offset bits", ftello(stream) == INT64_C(5) * 1024 * 1024 * 1024);
  check("stdio close retires stream descriptor", fclose(stream) == 0 && fcntl(fd, F_GETFD) == -1 && errno == EBADF);
  check("stdio close preserves dup lifetime", stdio_bytes_equal(alias, "XYZp!7il"));
  close(alias);

  fd = open(path, O_RDWR);
  check("stdio opens append fixture", fd >= 0 && lseek(fd, 2, SEEK_SET) == 2);
  stream = fdopen(fd, "a+");
  check("stdio fdopen append sets the real status bit", stream != NULL && (fcntl(fd, F_GETFL) & O_APPEND));
  if (stream) {
    check("stdio fdopen append preserves the old cursor", lseek(fd, 0, SEEK_CUR) == 2);
    check("stdio append stream can clear shared append", fcntl(fd, F_SETFL, 0) == 0);
    check("stdio fdopen has no hidden CRT append decision", fputc('a', stream) == 'a' && fflush(stream) == 0 && stdio_bytes_equal(fd, "XYap!7il"));
    fclose(stream);
  } else close(fd);

#if defined(__BIONIC__) || defined(_WIN32) || defined(GESHER_STDIO_HOST_ENVELOPE)
  stream = fopen(path, "a+");
  check("Android fopen append-update starts reads at EOF", stream != NULL && ftello(stream) == 8);
  if (stream) {
    check("Android append-update initial input reports EOF", fgetc(stream) == EOF && feof(stream));
    check("Android append-update can position its input", fseeko(stream, 0, SEEK_SET) == 0 && fgetc(stream) == 'X');
    fclose(stream);
  }
#endif
#if defined(__BIONIC__) || defined(_WIN32)
  stream = fopen(path, "r");
  if (stream) stream = freopen(path, "a+", stream);
  check("Android freopen append-update also starts reads at EOF", stream != NULL && ftello(stream) == 8);
  if (stream) fclose(stream);
#endif

  fd = open(path, O_RDWR);
  stream = fdopen(fd, "r");
  check("stdio can narrow stream mode on an update fd", stream != NULL);
  if (stream) {
    errno = 0;
    check("stdio readonly stream rejects writes on writable fd", fputc('x', stream) == EOF && errno == EBADF && ferror(stream));
    clearerr(stream);
    check("stdio clearerr clears its error flag", !ferror(stream) && !feof(stream));
#if defined(__BIONIC__) || defined(_WIN32) || defined(GESHER_STDIO_HOST_ENVELOPE)
    errno = 0;
    check("Android readonly fflush reports EBADF", fflush(stream) == EOF && errno == EBADF && !ferror(stream));
#endif
    fclose(stream);
  } else close(fd);
  fd = open(path, O_RDONLY);
  errno = 0;
  check("stdio incompatible fdopen fails without consuming fd", fdopen(fd, "w") == NULL && errno == EINVAL && fcntl(fd, F_GETFD) >= 0);
  close(fd);

  stream = fopen(path, "w+");
  check("stdio opens formatted-input fixture", stream != NULL);
  if (stream) {
    int number = -1;
    char word[16] = {0};
    check("stdio writes scanner fixture", fputs("123 word!", stream) >= 0 && fseeko(stream, 0, SEEK_SET) == 0);
    check("stdio formatted input shares the read buffer", fscanf(stream, "%d %4s", &number, word) == 2 && number == 123 && strcmp(word, "word") == 0);
    check("stdio scanning leaves unconsumed input", fgetc(stream) == '!');
    check("stdio EOF is distinct from errors", fgetc(stream) == EOF && feof(stream) && !ferror(stream));
    check("stdio ungetc clears EOF", ungetc('k', stream) == 'k' && !feof(stream) && fgetc(stream) == 'k');
    rewind(stream);
    check("stdio rewind clears errors and EOF", !feof(stream) && !ferror(stream) && fgetc(stream) == '1');
    fclose(stream);
  }

  fd = open(path, O_RDWR | O_TRUNC);
  check("wide stdio initializes an existing descriptor", fd >= 0 && write(fd, "base", 4) == 4 && lseek(fd, 0, SEEK_SET) == 0);
  stream = fdopen(fd, "r+");
  check("wide stdio wraps the same descriptor", stream != NULL);
  if (stream) {
    alias = dup(fd);
    check("wide stdio starts unoriented", fwide(stream, 0) == 0);
    check("wide stdio selects and retains wide orientation", fputwc(L'A', stream) == L'A' && fwide(stream, 0) > 0 && fwide(stream, -1) > 0);
    check("wide stdio shares buffered output", fputws(L"B", stream) >= 0 && fwprintf(stream, L"C%d", 9) == 2);
    check("wide stdio changes append through duplicate", alias >= 0 && fcntl(alias, F_SETFL, O_APPEND) == 0);
    check("wide stdio flush uses current append status", fflush(stream) == 0 && stdio_bytes_equal(alias, "baseABC9"));
    check("wide stdio positions its input", fseeko(stream, 0, SEEK_SET) == 0 && fgetwc(stream) == L'b');
    check("wide stdio pushback clears on positioning", ungetwc(L'Q', stream) == L'Q' && fgetwc(stream) == L'Q' && ungetwc(L'R', stream) == L'R' && fseeko(stream, 0, SEEK_SET) == 0 && fgetwc(stream) == L'b');
    close(alias);
    fclose(stream);
  } else close(fd);

  fd = open(path, O_RDWR | O_TRUNC);
  check("wide formatting initializes an existing descriptor", fd >= 0 && write(fd, "base", 4) == 4 && lseek(fd, 0, SEEK_SET) == 0);
  stream = fdopen(fd, "r+");
  check("wide formatting wraps the same descriptor", stream != NULL);
  if (stream) {
    alias = dup(fd);
#if defined(__BIONIC__)
    // Android deliberately rejects printf %n. Keep the mixed-format output
    // and return-count contract here; native hosts also check its side effect.
    check("wide formatting combines literal, string, padding and character output",
          fwprintf(stream, L"[%4.2ls|%-4.2s|%lc]", L"wxyz", "abcd", L'!') == 13);
#else
    int written = -1;
    check("wide formatting combines literal, string, padding and character output",
          fwprintf(stream, L"[%4.2ls|%-4.2s|%lc]%n", L"wxyz", "abcd", L'!', &written) == 13 && written == 13);
#endif
    check("wide formatting changes append through duplicate", alias >= 0 && fcntl(alias, F_SETFL, O_APPEND) == 0);
    check("all wide formatting output shares the current append status",
          fflush(stream) == 0 && stdio_bytes_equal(alias, "base[  wx|ab  |!]"));
    close(alias);
    fclose(stream);
  } else close(fd);

  stream = fopen(path, "w+");
  check("stdio opens recursive-lock fixture", stream != NULL);
  if (stream) {
    pthread_t worker;
    void* result = NULL;
    flockfile(stream);
    flockfile(stream);
    int started = pthread_create(&worker, NULL, stdio_try_locked, stream) == 0;
    check("stdio starts trylock worker", started);
    if (started) check("stdio trylock is nonblocking and unlocked output stays unlocked", pthread_join(worker, &result) == 0 && result == NULL);
    funlockfile(stream);
    funlockfile(stream);
    check("stdio recursive stream locks release completely", ftrylockfile(stream) == 0);
    funlockfile(stream);
    check("stdio unlocked output uses the shared buffer", fflush(stream) == 0 && stdio_bytes_equal(fileno(stream), "u"));
    fclose(stream);
  }
  {
    pthread_t worker;
    void* result = NULL;
    int i;
    int opened = 1;
    int started = pthread_create(&worker, NULL, stdio_flush_all, NULL) == 0;
    check("stdio starts concurrent flush-all worker", started);
    for (i = 0; i < 300; ++i) {
      stream = fopen(path, "w");
      if (stream == NULL) { opened = 0; break; }
      if (fwrite("closed", 1, 6, stream) != 6 || fclose(stream) != 0) { opened = 0; break; }
    }
    check("stdio close/reuse survives concurrent flush-all", opened);
    if (started) check("stdio flush-all never uses a retired FILE", pthread_join(worker, &result) == 0 && result == NULL);
  }
  {
    int pipes[2];
    int made = pipe2(pipes, O_NONBLOCK | O_CLOEXEC) == 0;
    check("stdio opens real nonblocking pipe", made);
    if (made) {
      stream = fdopen(pipes[0], "r");
      check("stdio wraps pipe input", stream != NULL);
      if (stream) {
        errno = 0;
        check("stdio nonblocking read reports EAGAIN, not EOF", fgetc(stream) == EOF && errno == EAGAIN && ferror(stream) && !feof(stream));
        clearerr(stream);
        check("stdio supplies real pipe data", write(pipes[1], "pipe\n", 5) == 5);
        check("stdio fgets uses descriptor I/O", fgets(text, sizeof(text), stream) == text && strcmp(text, "pipe\n") == 0);
        close(pipes[1]);
        check("stdio closed writer is EOF", fgetc(stream) == EOF && feof(stream) && !ferror(stream));
        fclose(stream);
      } else { close(pipes[0]); close(pipes[1]); }
    }
  }
  unlink(path);
}
