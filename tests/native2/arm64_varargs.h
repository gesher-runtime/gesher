// ADR0079/test86: real C va_list consumers, built/executed only in CI.
#pragma once

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <zlib.h>

#include <string>
#include <vector>

namespace arm64_varargs {

struct Result {
  std::vector<std::string> failures;
  void Check(bool ok, const char* what) { if (!ok) failures.emplace_back(what); }
  static const char* Summary() {
    return "literal/few/many GP/FP; ordered shared overflow; va_copy; nested/thread; "
           "wide and binary128 source; gzvprintf retry";
  }
};

inline thread_local const char* expected = nullptr;

__attribute__((noinline, format(printf, 1, 2))) inline bool Format(const char* fmt, ...) {
  va_list ap, first, second;
  va_start(ap, fmt);
  va_copy(first, ap);
  va_copy(second, ap);
  char a[4096], b[4096];
  const int na = vsnprintf(a, sizeof(a), fmt, first);
  const int nb = vsnprintf(b, sizeof(b), fmt, second);
  va_end(first);
  va_end(second);
  va_end(ap);
  return na >= 0 && na == nb && static_cast<size_t>(na) == strlen(expected) &&
         strcmp(a, expected) == 0 && strcmp(b, expected) == 0;
}

// Original Surface logger body, including its independent printf calls.
__attribute__((format(printf, 1, 2), noinline)) inline void Say(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  printf("surfacegl: ");
  vprintf(fmt, ap);
  printf("\n");
  fflush(stdout);
  va_end(ap);
}

inline bool Mixed() {
  expected = "1.0 2.0 3.0 4.0 5.0 6.0 7.0 8.0 9.0 10.0|1 2 3 4 5 6 7 8 9 10";
  return Format("%.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f|%d %d %d %d %d %d %d %d %d %d",
                1., 2., 3., 4., 5., 6., 7., 8., 9., 10., 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
}

inline int Compare(const void* a, const void* b) {
  // Actual host qsort re-enters guest code while its caller's list is live.
  if (!Mixed()) return 0;
  return *static_cast<const int*>(a) - *static_cast<const int*>(b);
}

__attribute__((noinline, format(printf, 1, 2))) inline bool Nested(const char* fmt, ...) {
  va_list ap, copy;
  va_start(ap, fmt);
  va_copy(copy, ap);
  int items[] = {4, 1, 3, 2};
  qsort(items, 4, sizeof(int), Compare);
  char a[64], b[64];
  int na = vsnprintf(a, sizeof(a), fmt, ap);
  int nb = vsnprintf(b, sizeof(b), fmt, copy);
  va_end(copy);
  va_end(ap);
  return items[0] == 1 && items[1] == 2 && items[2] == 3 && items[3] == 4 &&
         na == 17 && nb == na && strcmp(a, "11 22 33 44 55 66") == 0 && strcmp(a, b) == 0;
}

inline void* Thread(void* data) {
  bool* result = static_cast<bool*>(data);
  bool ok = true;
  for (int i = 0; i < 16; ++i) ok = Mixed() && ok;
  *result = ok;
  return nullptr;
}

__attribute__((noinline)) inline int Wide(FILE* file, const wchar_t* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int count = vfwprintf(file, fmt, ap);
  va_end(ap);
  return count;
}

__attribute__((noinline, format(printf, 2, 3))) inline int Gzip(gzFile file, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int count = gzvprintf(file, fmt, ap);
  va_end(ap);
  return count;
}

inline Result Run(const std::string& dir) {
  Result r;
  Say("invalidation: abi arm64-v8a");
  Say("invalidation: ints %d %d %d %d %d %d %d %d", 1, 2, 3, 4, 5, 6, 7, 8);
  expected = "literal-only";
  r.Check(Format("literal-only"), "zero anonymous arguments");
  expected = "7/2.5/text";
  r.Check(Format("%d/%.1f/%s", 7, 2.5, "text"), "few independent GP/FP arguments");
  r.Check(Mixed(), "FP overflow precedes seventh GP and GP overflow; independent va_copy");
  expected = "1 2 3 4 5 6 7 8 9 10|1.0 2.0 3.0 4.0 5.0 6.0 7.0 8.0 9.0 10.0";
  r.Check(Format("%d %d %d %d %d %d %d %d %d %d|%.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f",
                 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 1., 2., 3., 4., 5., 6., 7., 8., 9., 10.),
          "GP overflow precedes FP overflow");
  expected = "1.0 1 2.0 2 3.0 3 4.0 4 5.0 5 6.0 6 7.0 7 8.0 8 9.0 9 10.0 10";
  r.Check(Format("%.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d %.1f %d",
                 1., 1, 2., 2, 3., 3, 4., 4, 5., 5, 6., 6, 7., 7, 8., 8, 9., 9, 10., 10),
          "interleaved GP/FP exhaustion");
  expected = "0012/abc/-1234567890123";
  r.Check(Format("%0*d/%.*s/%lld", 4, 12, 3, "abcdef", -1234567890123LL),
          "width/precision consume ordered GP slots and LP64 values");
  std::string many;
  for (int i = 0; i < 80; ++i) {
    if (i) many += ' ';
    many += std::to_string(i % 10 + 1);
  }
  expected = many.c_str();
#define ARM64_VARARGS_TEN_FMT "%d %d %d %d %d %d %d %d %d %d"
#define ARM64_VARARGS_TEN_ARGS 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
  r.Check(Format(ARM64_VARARGS_TEN_FMT " " ARM64_VARARGS_TEN_FMT " " ARM64_VARARGS_TEN_FMT " " ARM64_VARARGS_TEN_FMT " "
                 ARM64_VARARGS_TEN_FMT " " ARM64_VARARGS_TEN_FMT " " ARM64_VARARGS_TEN_FMT " " ARM64_VARARGS_TEN_FMT,
                 ARM64_VARARGS_TEN_ARGS, ARM64_VARARGS_TEN_ARGS, ARM64_VARARGS_TEN_ARGS, ARM64_VARARGS_TEN_ARGS,
                 ARM64_VARARGS_TEN_ARGS, ARM64_VARARGS_TEN_ARGS, ARM64_VARARGS_TEN_ARGS, ARM64_VARARGS_TEN_ARGS),
          "80 integer arguments exceed the old fixed overflow copy window");
#undef ARM64_VARARGS_TEN_ARGS
#undef ARM64_VARARGS_TEN_FMT
  // Exactly representable binary128 values preserve the formatter's existing
  // narrowing behavior. This is a source-location regression, not full precision.
  expected = "1.5/2.5/7";
  r.Check(Format("%.1Lf/%.1f/%d", 1.5L, 2.5, 7), "binary128 register source retained");
  expected = "1 2 3 4 5 6 7 8|1.5|9";
  r.Check(Format("%.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f|%.1Lf|%d",
                 1., 2., 3., 4., 5., 6., 7., 8., 1.5L, 9), "binary128 aligned stack source retained");
  r.Check(Nested("%d %d %d %d %d %d", 11, 22, 33, 44, 55, 66), "live lists survive nested guest callbacks");
  pthread_t threads[2];
  bool outcomes[2] = {false, false};
  bool started[2];
  for (int i = 0; i < 2; ++i) started[i] = pthread_create(&threads[i], nullptr, Thread, &outcomes[i]) == 0;
  for (int i = 0; i < 2; ++i) {
    r.Check(started[i], "guest pthread starts");
    if (started[i]) r.Check(pthread_join(threads[i], nullptr) == 0 && outcomes[i], "concurrent lists are independent");
  }
  FILE* file = tmpfile();
  r.Check(file != nullptr, "wide formatted-output stream opens");
  if (file) {
    int count = Wide(file, L"%.1f %d %.1f %d", 1.5, 7, 2.5, 8);
    rewind(file);
    char text[64] = {};
    size_t bytes = fread(text, 1, sizeof(text) - 1, file);
    r.Check(count == 11 && bytes == 11 && strcmp(text, "1.5 7 2.5 8") == 0, "wide formatter uses shared bank readers");
    fclose(file);
  }
  const std::string path = dir + "/arm64-varargs.gz";
  gzFile gz = gzopen(path.c_str(), "wb");
  r.Check(gz != nullptr, "gzip output opens");
  std::string large(2048, 'x');
  const std::string want = large + "|1.5|7|2.5|8";
  if (gz) {
    r.Check(Gzip(gz, "%s|%.1f|%d|%.1f|%d", large.c_str(), 1.5, 7, 2.5, 8) == static_cast<int>(want.size()),
            "gzvprintf retries with independent copied list above its 1024-byte buffer");
    r.Check(gzclose(gz) == Z_OK, "gzip output closes");
    gz = gzopen(path.c_str(), "rb");
    r.Check(gz != nullptr, "gzip output reopens");
    if (gz) {
      char text[4096] = {};
      int count = gzread(gz, text, sizeof(text) - 1);
      r.Check(count == static_cast<int>(want.size()) && text == want, "gzip retry preserves exact bytes");
      gzclose(gz);
    }
    remove(path.c_str());
  }
  return r;
}

}  // namespace arm64_varargs
