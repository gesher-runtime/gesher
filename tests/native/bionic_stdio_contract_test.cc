// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Run against exported Bionic symbols on Windows and directly against glibc
// on Linux. Host lock witnesses verify that Bionic owns the CRT's actual lock.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef _WIN32
#include <gesher/bionic_shim.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include <cerrno>
#include <chrono>
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <future>
#include <thread>

#if defined(_WIN32) && defined(__x86_64__)
#define SYSV __attribute__((sysv_abi))
#else
#define SYSV
#endif

namespace {
#ifdef _WIN32
using Wide = uint32_t;
using Wint = uint32_t;
#else
using Wide = wchar_t;
using Wint = wint_t;
#endif
constexpr auto kWideEof = static_cast<Wint>(-1);
using Lock = void(SYSV*)(FILE*);
using TryLock = int(SYSV*)(FILE*);
using Orientation = int(SYSV*)(FILE*, int);
using Close = int(SYSV*)(FILE*);
using Reopen = FILE*(SYSV*)(const char*, const char*, FILE*);
using Get = int(SYSV*)(FILE*);
using GetWide = Wint(SYSV*)(FILE*);
using GetWideLine = Wide*(SYSV*)(Wide*, int, FILE*);
using PutWide = Wint(SYSV*)(Wide, FILE*);
using UngetWide = Wint(SYSV*)(Wint, FILE*);
using PutWideLine = int(SYSV*)(const Wide*, FILE*);
using PrintWide = int(SYSV*)(FILE*, const Wide*, ...);
using PrintWideBuffer = int(SYSV*)(Wide*, size_t, const Wide*, ...);
using Status = int(SYSV*)(FILE*);
using Clear = void(SYSV*)(FILE*);
using Position = void(SYSV*)(FILE*);
using Error = int*(SYSV*)();

struct Api {
  Lock lock, unlock;
  TryLock try_lock;
  Orientation orient;
  Close close;
  Reopen reopen;
  Get get;
  GetWide get_wide;
  GetWideLine get_line;
  PutWide put_wide;
  UngetWide unget;
  PutWideLine put_line;
  PrintWide print;
  PrintWideBuffer print_buffer;
  Status eof, error;
  Clear clear;
  Position rewind_stream;
  Error errno_value;
};

int assertions = 0;
int failures = 0;

void Check(const char* name, bool success) {
  ++assertions;
  if (!success) ++failures;
  std::printf("bionic_stdio: %s %s\n", name, success ? "PASS" : "FAIL");
}

#ifdef _WIN32
template <class Fn>
Fn Load(const char* name) {
  auto fn = reinterpret_cast<Fn>(gesher::bionic::LookupBionicSymbol(name));
  if (!fn) {
    std::fprintf(stderr, "missing Bionic export %s\n", name);
    std::abort();
  }
  return fn;
}
Api Resolve() {
  gesher::bionic::InstallBionicShim();
  return {Load<Lock>("flockfile"), Load<Lock>("funlockfile"), Load<TryLock>("ftrylockfile"),
          Load<Orientation>("fwide"), Load<Close>("fclose"), Load<Reopen>("freopen"),
          Load<Get>("fgetc"), Load<GetWide>("fgetwc"), Load<GetWideLine>("fgetws"),
          Load<PutWide>("fputwc"), Load<UngetWide>("ungetwc"), Load<PutWideLine>("fputws"),
          Load<PrintWide>("fwprintf"), Load<PrintWideBuffer>("swprintf"), Load<Status>("feof"),
          Load<Status>("ferror"), Load<Clear>("clearerr"), Load<Position>("rewind"),
          Load<Error>("__errno")};
}
int Fd(FILE* f) { return _fileno(f); }
void NativeLock(FILE* f) { _lock_file(f); }
void NativeUnlock(FILE* f) { _unlock_file(f); }
#else
int* NativeErrno() { return &errno; }
Api Resolve() {
  if (!std::setlocale(LC_CTYPE, "C.UTF-8")) std::abort();
  return {flockfile, funlockfile, ftrylockfile, fwide, fclose, freopen,
          fgetc, fgetwc, fgetws, fputwc, ungetwc, fputws, fwprintf, swprintf,
          feof, ferror, clearerr, rewind, NativeErrno};
}
int Fd(FILE* f) { return fileno(f); }
void NativeLock(FILE* f) { flockfile(f); }
void NativeUnlock(FILE* f) { funlockfile(f); }
#endif

FILE* Stream() {
  FILE* f = std::tmpfile();
  if (!f) {
    std::perror("tmpfile");
    std::abort();
  }
  return f;
}

void Seed(FILE* f, const char* bytes, unsigned size) {
#ifdef _WIN32
  bool written = _write(Fd(f), bytes, size) == static_cast<int>(size);
#else
  bool written = write(Fd(f), bytes, size) == static_cast<ssize_t>(size);
#endif
  if (!written) std::abort();
  std::rewind(f);
}

void ExpectBytes(FILE* f, const char* expected, unsigned size) {
  if (std::fflush(f) != 0) std::abort();
  char actual[64]{};
#ifdef _WIN32
  if (_lseeki64(Fd(f), 0, SEEK_SET) != 0) std::abort();
  int count = _read(Fd(f), actual, sizeof(actual));
#else
  if (lseek(Fd(f), 0, SEEK_SET) != 0) std::abort();
  ssize_t count = read(Fd(f), actual, sizeof(actual));
#endif
  Check("UTF-8 stream content", count >= 0 && static_cast<size_t>(count) == size &&
        std::memcmp(actual, expected, size) == 0);
}

template <class Release>
void TryFromOtherThread(const Api& api, FILE* held, FILE* other,
                        Release release_on_timeout, const char* description) {
  using namespace std::chrono_literals;
  std::promise<bool> result;
  auto done = result.get_future();
  std::thread worker([&] {
    constexpr int kMarker = 71;
    *api.errno_value() = kMarker;
    int busy = api.try_lock(held);
    bool preserved_error = *api.errno_value() == kMarker;
    if (busy == 0) api.unlock(held);
    int acquired = api.try_lock(other);
    if (acquired == 0) api.unlock(other);
#ifdef _WIN32
    bool contention = busy == EBUSY;
#else
    bool contention = busy != 0;
#endif
    result.set_value(contention && preserved_error && acquired == 0);
  });
  bool prompt = done.wait_for(3s) == std::future_status::ready;
  if (!prompt) release_on_timeout(held);
  worker.join();
  Check(description, prompt && done.get());
  if (!prompt) std::abort();
}

void TestLocks(const Api& api) {
  FILE* first = Stream();
  FILE* second = Stream();
  NativeLock(first);
  TryFromOtherThread(api, first, second, NativeUnlock, "prompt busy under CRT ownership");
  NativeUnlock(first);

  api.lock(first);
  Check("recursive try acquires on owning thread", api.try_lock(first) == 0);
  api.unlock(first);
  TryFromOtherThread(api, first, second, api.unlock, "still busy after one recursive release");
  api.unlock(first);

  int acquired = api.try_lock(first);
  Check("available stream acquired after final release", acquired == 0);
  if (acquired != 0) std::abort();
  using namespace std::chrono_literals;
  std::promise<void> started;
  std::promise<int> result;
  auto done = result.get_future();
  std::thread writer([&] {
    started.set_value();
    result.set_value(std::fputc('x', first));
  });
  started.get_future().wait();
  Check("try lock excludes host buffered I/O", done.wait_for(100ms) == std::future_status::timeout);
  api.unlock(first);
  writer.join();
  Check("host buffered I/O completes after unlock", done.get() == 'x');
  Check("close after locks released", api.close(first) == 0 && api.close(second) == 0);
#ifdef _WIN32
  FILE* other = Stream();
  auto legacy = static_cast<unsigned char*>(gesher::bionic::LookupBionicSymbol("__sF"));
  FILE* legacy_stderr = reinterpret_cast<FILE*>(legacy + 2 * 152);
  api.lock(legacy_stderr);
  TryFromOtherThread(api, stderr, other, NativeUnlock, "legacy __sF shares the modern stderr lock");
  api.unlock(legacy_stderr);
  Check("legacy lock witness closes", api.close(other) == 0);
#endif
}

void TestWideInput(const Api& api) {
  FILE* f = Stream();
  constexpr char input[] = "\xc3\xa9\xf0\x9f\x99\x82\nZ";
  Seed(f, input, sizeof(input) - 1);
  Check("fresh stream has no orientation", api.orient(f, 0) == 0);
  Wide line[8]{};
  Check("one-slot wide line is empty without consuming", api.get_line(line, 1, f) == line && line[0] == 0);
  Check("whole wide line preserves supplementary character and newline",
        api.get_line(line, 8, f) == line && line[0] == 0xe9 && line[1] == 0x1f642 &&
        line[2] == '\n' && line[3] == 0);
  Check("orientation remains wide", api.orient(f, -1) > 0 && api.orient(f, 0) > 0);
  Check("last wide character", api.get_wide(f) == 'Z');
  Check("wide read sets EOF", api.get_wide(f) == kWideEof && api.eof(f) != 0 && api.error(f) == 0);
  Check("wide pushback clears EOF", api.unget(0x1f642, f) == 0x1f642 && api.eof(f) == 0);
#ifdef _WIN32
  Check("Android wide pushback has one slot", api.unget('X', f) == kWideEof);
#endif
  Check("pushed code point is returned whole", api.get_wide(f) == 0x1f642);
  Check("WEOF is not pushed back", api.unget(kWideEof, f) == kWideEof);
  Check("reopen clears the stream orientation", api.reopen("/dev/null", "rb", f) == f &&
        api.orient(f, 0) == 0);
  Check("reopened stream closes", api.close(f) == 0);

  f = Stream();
  constexpr char invalid[]{'Q', static_cast<char>(0xff)};
  Seed(f, invalid, sizeof(invalid));
  Check("wide decoding preserves preceding character", api.get_wide(f) == 'Q');
  *api.errno_value() = 0;
  Check("encoding error marks the stream", api.get_wide(f) == kWideEof &&
        *api.errno_value() == 84 && api.error(f) != 0);
  api.rewind_stream(f);
  Check("rewind clears conversion error while keeping orientation", api.error(f) == 0 && api.orient(f, 0) > 0);
  Check("rewind restored input cursor", api.get_wide(f) == 'Q' && api.get_wide(f) == kWideEof && api.error(f) != 0);
  api.clear(f);
  Check("clearerr clears conversion error", api.error(f) == 0);
  Check("malformed input stream closes", api.close(f) == 0);

  f = Stream();
  Seed(f, "x", 1);
  Check("byte read orients the stream", api.get(f) == 'x' && api.orient(f, 1) < 0);
  Check("byte input stream closes", api.close(f) == 0);
}

void TestWideOutput(const Api& api) {
  FILE* f = Stream();
  const Wide text[] = {0xe9, 0x1f642, 0};
  Check("wide NUL is written and counted", api.put_wide(0, f) == 0);
  Check("wide string writes BMP and supplementary characters", api.put_line(text, f) >= 0);
  Check("wide output sets persistent orientation", api.orient(f, 0) > 0);
  constexpr char bytes[] = "\0\xc3\xa9\xf0\x9f\x99\x82";
  ExpectBytes(f, bytes, sizeof(bytes) - 1);
  Check("wide output stream closes", api.close(f) == 0);

#if !defined(_WIN32) || !defined(__aarch64__)
  // The variadic exports run under the x86-64 System V bridge. The ARM64
  // translator has a separate argument adapter unavailable to a native PE.
  f = Stream();
  const Wide fmt[] = {0xe9, '|', '%', '4', '.', '1', 'l', 's', '|', '%', '.', '1', 's',
                      '|', '%', 'l', 'c', '%', 'n', 0};
  const Wide expected[] = {0xe9, '|', ' ', ' ', ' ', 0xe9, '|', 0xe9, '|', 0x1f642, 0};
  const char narrow[] = "\xc3\xa9Q";
  Wide buffer[32]{};
  int count = -1;
  Check("swprintf precision, padding, %n and count use wide characters",
        api.print_buffer(buffer, 32, fmt, text, narrow, Wint{0x1f642}, &count) == 10 && count == 10 &&
        std::memcmp(buffer, expected, sizeof(expected)) == 0);
  count = -1;
  Check("fwprintf counts wide characters and sets orientation",
        api.print(f, fmt, text, narrow, Wint{0x1f642}, &count) == 10 && count == 10 && api.orient(f, 0) > 0);
  constexpr char formatted[] = "\xc3\xa9|   \xc3\xa9|\xc3\xa9|\xf0\x9f\x99\x82";
  ExpectBytes(f, formatted, sizeof(formatted) - 1);
  Check("wide formatted stream closes", api.close(f) == 0);
#endif
}
}  // namespace

int main() {
  const Api api = Resolve();
  TestLocks(api);
  TestWideInput(api);
  TestWideOutput(api);
  std::printf("bionic_stdio: %d assertions, %d failures\n", assertions, failures);
  if (failures == 0) std::puts("bionic_stdio_contract_test all passed");
  return failures == 0 ? 0 : 1;
}
