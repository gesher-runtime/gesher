// Public conversion/stream contracts shared by real MinGW and Linux stdio.
// The caller supplies check(); no parser or conversion result is substituted.
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

static FILE* ScanFixture(const void* bytes, size_t size, char* path) {
  strcpy(path, "gesher-stdio-scan-XXXXXX");
  int fd = mkstemp(path);
  check("scan fixture opens", fd >= 0);
  if (fd < 0) return NULL;
  check("scan fixture writes complete input", write(fd, bytes, size) == (ssize_t)size);
  check("scan fixture rewinds", lseek(fd, 0, SEEK_SET) == 0);
  FILE* stream = fdopen(fd, "r");
  check("scan fixture takes descriptor ownership", stream != NULL);
  if (!stream) { close(fd); unlink(path); }
  return stream;
}
static void ScanClose(FILE* stream, const char* path) {
  check("scan stream closes", fclose(stream) == 0);
  check("scan fixture removes", unlink(path) == 0);
}
static void test_stdio_scan_contracts(void) {
  char path[64];
  const char numbers[] = " -17 077 0x2a 184467 1.25 % abc!";
  FILE* stream = ScanFixture(numbers, sizeof(numbers) - 1, path);
  if (stream) {
    int signed_value = 0, octal = 0, hex = 0, count = -1;
    unsigned long long large = 0;
    double real = 0;
    char word[4] = {0};
    check("scan conversions preserve values, counts and literals",
          fscanf(stream, "%d %i %i %llu %lf %% %3[a-z]%n", &signed_value, &octal, &hex,
                 &large, &real, word, &count) == 6 && signed_value == -17 && octal == 63 &&
          hex == 42 && large == 184467 && real == 1.25 && strcmp(word, "abc") == 0 &&
          count == (int)sizeof(numbers) - 2);
    check("scan exact width leaves following input", fgetc(stream) == '!');
    check("scan completion preserves EOF versus I/O error", fgetc(stream) == EOF && feof(stream) && !ferror(stream));
    ScanClose(stream, path);
  }
  stream = ScanFixture("3x", 2, path);
  if (stream) {
    check("scan caller pushback remains ordered", ungetc('2', stream) == '2' && ungetc('1', stream) == '1');
    int value = 0, count = -1;
    check("scan consumes caller pushback and real bytes once", fscanf(stream, "%3d%n", &value, &count) == 1 && value == 123 && count == 3);
    check("scan lookahead after pushback remains readable", fgetc(stream) == 'x');
    ScanClose(stream, path);
  }
  stream = ScanFixture("word:7", 6, path);
  if (stream) {
    int value = 91;
    check("scan mismatch preserves destination and state", fscanf(stream, "%d", &value) == 0 && value == 91 && !feof(stream) && !ferror(stream));
    char word[5] = {0};
    check("scan mismatch preserves the unconsumed byte", fscanf(stream, "%4s:%d", word, &value) == 2 && strcmp(word, "word") == 0 && value == 7);
    value = 92;
    check("scan initial EOF preserves unassigned destination", fscanf(stream, "%d", &value) == EOF && value == 92 && feof(stream) && !ferror(stream));
    check("scan pushback clears EOF for a later conversion", ungetc('8', stream) == '8' && !feof(stream) && fscanf(stream, "%d", &value) == 1 && value == 8);
    ScanClose(stream, path);
  }
  stream = ScanFixture("42", 2, path);
  if (stream) {
    int first = 0, second = 93, count = -1;
    check("scan partial EOF reports assignments already made", fscanf(stream, "%d %d%n", &first, &second, &count) == 1 && first == 42 && second == 93 && count == -1 && feof(stream) && !ferror(stream));
    ScanClose(stream, path);
  }
  {
    const char input[] = {'x', '\0', 'z', '1', '2', '3', '!'};
    stream = ScanFixture(input, sizeof(input), path);
    if (stream) {
      char raw[3] = {0};
      int value = 0, count = -1;
      check("scan character width preserves embedded NUL", fscanf(stream, "%3c%d%n", raw, &value, &count) == 2 && memcmp(raw, input, 3) == 0 && value == 123 && count == 6);
      check("scan character conversion preserves lookahead", fgetc(stream) == '!');
      ScanClose(stream, path);
    }
  }
  stream = ScanFixture("skip 56?", 8, path);
  if (stream) {
    int value = 0;
    check("scan suppressed conversion consumes no destination", fscanf(stream, "%*4c %d", &value) == 1 && value == 56 && fgetc(stream) == '?');
    ScanClose(stream, path);
  }
  {
    char* input = (char*)malloc(4098);
    char* result = (char*)malloc(4098);
    check("scan long-token fixture allocates", input != NULL && result != NULL);
    if (input && result) {
      memset(input, 'v', 4097); input[4097] = '!';
      stream = ScanFixture(input, 4098, path);
      if (stream) {
        int count = -1;
        check("scan token crosses real buffer refills", fscanf(stream, "%4097[a-z]%n", result, &count) == 1 && count == 4097 && result[4097] == 0 && memcmp(result, input, 4097) == 0);
        check("scan long token preserves following input", fgetc(stream) == '!');
        ScanClose(stream, path);
      }
    }
    free(input); free(result);
  }
  {
    int ends[2] = {-1, -1};
    check("scan nonblocking error pipe opens", pipe(ends) == 0);
    if (ends[0] >= 0) {
      check("scan nonblocking mode is real", fcntl(ends[0], F_SETFL, O_NONBLOCK) == 0);
      stream = fdopen(ends[0], "r");
      check("scan nonblocking stream opens", stream != NULL);
      if (stream) {
        check("scan error input is supplied", write(ends[1], "12 ", 3) == 3);
        int first = 0, second = 94;
        errno = 0;
        // MinGW restores entry errno after its whitespace loop. That inherited
        // parser difference is not changed here; the raw-character case below
        // independently requires the callback's actual error number.
        check("scan partial I/O error preserves assigned results", fscanf(stream, "%d %d", &first, &second) == 1 && first == 12 && second == 94 && ferror(stream) && !feof(stream));
        clearerr(stream);
        check("scan retry input is real", write(ends[1], "34#", 3) == 3);
        check("scan cleared error permits a real retry", fscanf(stream, "%d", &second) == 1 && second == 34 && fgetc(stream) == '#' && !ferror(stream));
        char unassigned = '?';
        errno = 0;
        check("scan raw input preserves its actual I/O error", fscanf(stream, "%c", &unassigned) == EOF && unassigned == '?' && ferror(stream) && !feof(stream) && (errno == EAGAIN || errno == EWOULDBLOCK));
        check("scan nonblocking stream closes", fclose(stream) == 0);
      } else close(ends[0]);
      close(ends[1]);
    }
  }
}

#ifndef GESHER_STDIO_SCAN_NO_WIDE
static void test_stdio_scan_wide_contracts(void) {
  char path[64];
  FILE* stream = ScanFixture(" -19 abc!", 9, path);
  if (stream) {
    int value = 0, count = -1;
    wchar_t text[4] = {0};
    check("wide scan preserves conversions and character count", fwscanf(stream, L"%d %3ls%n", &value, text, &count) == 2 && value == -19 && wcscmp(text, L"abc") == 0 && count == 8);
    check("wide scan keeps wide orientation and lookahead", fwide(stream, 0) > 0 && fgetwc(stream) == L'!');
    check("wide scan EOF preserves unassigned result", fwscanf(stream, L"%d", &value) == EOF && value == -19 && feof(stream) && !ferror(stream));
    check("wide scan pushback clears EOF", ungetwc(L'7', stream) == L'7' && !feof(stream) && fwscanf(stream, L"%d", &value) == 1 && value == 7);
    ScanClose(stream, path);
  }
  const char* current_locale = setlocale(LC_CTYPE, NULL);
  char* previous_locale = current_locale ? strdup(current_locale) : NULL;
#ifdef _WIN32
  const char* selected_locale = setlocale(LC_CTYPE, ".UTF-8");
#else
  const char* selected_locale = setlocale(LC_CTYPE, "C.UTF-8");
#endif
  check("wide scan selects the real UTF-8 locale", selected_locale != NULL);
  if (selected_locale) {
    stream = ScanFixture("\xc3\xa9Z!", 4, path);
    if (stream) {
      wchar_t text[2] = {0};
      int count = -1;
      check("wide scan counts decoded characters across input bytes", fwscanf(stream, L"%2lc%n", text, &count) == 1 && text[0] == 0xe9 && text[1] == L'Z' && count == 2);
      check("wide scan preserves multibyte pushback", ungetwc(0xe9, stream) == 0xe9 && fwscanf(stream, L"%lc", text) == 1 && text[0] == 0xe9 && fgetwc(stream) == L'!');
      ScanClose(stream, path);
    }
    stream = ScanFixture("\xc3", 1, path);
    if (stream) {
      wchar_t value = L'?';
      errno = 0;
      check("wide scan truncated input retains its conversion error", fwscanf(stream, L"%lc", &value) == EOF && value == L'?' && ferror(stream) && errno == EILSEQ);
      ScanClose(stream, path);
    }
  }
  if (previous_locale) {
    check("wide scan restores the previous locale", setlocale(LC_CTYPE, previous_locale) != NULL);
    free(previous_locale);
  }
}
#endif
