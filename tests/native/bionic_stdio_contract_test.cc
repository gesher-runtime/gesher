// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Run against exported Bionic symbols on Windows or Bionic on Android. The
// Linux/glibc run is only a portable control; Android-specific contracts are
// deliberately not inferred from glibc. Host lock witnesses use the CRT lock.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef _WIN32
#include <windows.h>
#include <gesher/bionic_shim.h>
#include <io.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

#include <cerrno>
#include <chrono>
#include <csignal>
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <future>
#include <string>
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
using GetDelimited = int64_t(SYSV*)(char**, size_t*, int, FILE*);
using GetByteLine = char*(SYSV*)(char*, int, FILE*);
using UngetByte = int(SYSV*)(int, FILE*);
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
  GetDelimited delimited;
  GetByteLine byte_line;
  UngetByte unget_byte;
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
          Load<Error>("__errno"), Load<GetDelimited>("getdelim"),
          Load<GetByteLine>("fgets_unlocked"), Load<UngetByte>("ungetc")};
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
          feof, ferror, clearerr, rewind, NativeErrno, getdelim, fgets_unlocked, ungetc};
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

void TestByteLines(const Api& api) {
  FILE* stream = Stream();
  const std::string data = std::string(8192, 'q') + "\n" + std::string("x\xffy\0z\nnext", 10);
  Seed(stream, data.data(), static_cast<unsigned>(data.size()));
  char* line = nullptr;
  size_t capacity = 0;
  Check("getdelim includes a multi-buffer record and terminator",
        api.delimited(&line, &capacity, '\n', stream) == 8193 && capacity >= 8194 &&
        std::memcmp(line, data.data(), 8193) == 0 && line[8193] == 0);
  Check("getdelim delimiter -1 matches unsigned 255", api.delimited(&line, &capacity, -1, stream) == 2 &&
        line[0] == 'x' && static_cast<unsigned char>(line[1]) == 255 && line[2] == 0);
  Check("getdelim zero delimiter includes input NUL", api.delimited(&line, &capacity, 0, stream) == 2 &&
        line[0] == 'y' && line[1] == 0 && line[2] == 0);
  Check("getdelim pushback retains next descriptor bytes", api.unget_byte('!', stream) == '!' &&
        api.delimited(&line, &capacity, '\n', stream) == 3 && std::memcmp(line, "!z\n", 4) == 0);
  char bounded[12]{};
  Check("unlocked fgets limit preserves subsequent bytes", api.byte_line(bounded, 3, stream) == bounded &&
        std::memcmp(bounded, "ne", 3) == 0);
  Check("unlocked fgets gets final unterminated record", api.byte_line(bounded, sizeof(bounded), stream) == bounded &&
        std::memcmp(bounded, "xt", 3) == 0);
  Check("getdelim after EOF reports -1", api.delimited(&line, &capacity, '\n', stream) == -1 && api.eof(stream));
#if defined(_WIN32) || defined(__BIONIC__)
  api.clear(stream);
  Check("getdelim invalid output marks stream error", api.delimited(nullptr, &capacity, '\n', stream) == -1 &&
        *api.errno_value() == EINVAL && api.error(stream));
  api.clear(stream);
  Check("getdelim invalid capacity marks stream error", api.delimited(&line, nullptr, '\n', stream) == -1 &&
        *api.errno_value() == EINVAL && api.error(stream));
#endif
  std::free(line);
  api.close(stream);
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
                      '|', '%', 'l', 'c', 0};
  const Wide expected[] = {0xe9, '|', ' ', ' ', ' ', 0xe9, '|', 0xe9, '|', 0x1f642, 0};
  const char narrow[] = "\xc3\xa9Q";
  Wide buffer[32]{};
  Check("swprintf precision, padding and count use wide characters",
        api.print_buffer(buffer, 32, fmt, text, narrow, Wint{0x1f642}) == 10 &&
        std::memcmp(buffer, expected, sizeof(expected)) == 0);
  Check("fwprintf counts wide characters and sets orientation",
        api.print(f, fmt, text, narrow, Wint{0x1f642}) == 10 && api.orient(f, 0) > 0);
  constexpr char formatted[] = "\xc3\xa9|   \xc3\xa9|\xc3\xa9|\xf0\x9f\x99\x82";
  ExpectBytes(f, formatted, sizeof(formatted) - 1);
  Check("wide formatted stream closes", api.close(f) == 0);
  const Wide aliases[] = {'%', 'C', '/', '%', 'S', 0};
  const Wide alias_expected[] = {0x3b1, '/', 0xe9, 0x1f642, 0};
  Check("Android wide aliases preserve characters", api.print_buffer(buffer, 32, aliases, Wint{0x3b1}, text) == 4 &&
        std::memcmp(buffer, alias_expected, sizeof(alias_expected)) == 0);
#if defined(_WIN32) || defined(__BIONIC__)
  const Wide plain[] = {'a', 'b', 'c', 0};
  *api.errno_value() = 0;
  Check("zero wide buffer reports EINVAL", api.print_buffer(buffer, 0, plain) == -1 && *api.errno_value() == 22);
  *api.errno_value() = 0;
  Check("truncated wide buffer reports EOVERFLOW", api.print_buffer(buffer, 2, plain) == -1 &&
        *api.errno_value() == 75 && buffer[0] == 'a' && buffer[1] == 0);
  const Wide scalar[] = {'%', 'l', 'c', 0};
  for (Wint invalid : {Wint{0xd800}, Wint{0x110000}, Wint{0x200000}}) {
    buffer[0] = 0x7777; *api.errno_value() = 0;
    Check("wide buffer follows Android encode/decode errors", api.print_buffer(buffer, 32, scalar, invalid) == -1 &&
          *api.errno_value() == 84 && buffer[0] == 0x7777);
  }
  const Wide nul[] = {'A', '%', 'l', 'c', 'Z', 0};
  buffer[2] = 0x7777;
  Check("wide buffer retains Bionic embedded-NUL conversion boundary", api.print_buffer(buffer, 32, nul, Wint{0}) == 3 &&
        buffer[0] == 'A' && buffer[1] == 0 && buffer[2] == 0x7777);
  const Wide escaped[] = {'%', '%', 'n', 0};
  Check("escaped percent-n remains literal", api.print_buffer(buffer, 32, escaped) == 2 &&
        buffer[0] == '%' && buffer[1] == 'n' && buffer[2] == 0);
  f = Stream();
  const Wide partial[] = {'A', '%', 's', 'Z', 0};
  *api.errno_value() = 0;
  Check("wide stream conversion error retains earlier output", api.print(f, partial, "\xff") == -1 &&
        *api.errno_value() == 84 && api.error(f));
  ExpectBytes(f, "A", 1);
  api.close(f);
  f = Stream();
  const Wide invalid_byte[] = {'A', '%', '3', 'c', 'Z', 0};
  *api.errno_value() = 0;
  Check("wide percent-c preserves padding before Bionic btowc failure", api.print(f, invalid_byte, 0x80) == -1 &&
        *api.errno_value() == 84 && api.error(f));
  ExpectBytes(f, "A  ", 3);
  api.close(f);
  f = Stream();
  Check("wide percent-c follows Bionic single-byte narrowing", api.print(f, invalid_byte, 0x100) == 5);
  ExpectBytes(f, "A  \0Z", 5);
  api.close(f);
  const Wide overflow[] = {'A', '%', '2', '1', '4', '7', '4', '8', '3', '6', '4', '8', 'd', 0};
  buffer[0] = 0x7777; *api.errno_value() = 0;
  Check("wide format-size overflow uses Bionic ENOMEM", api.print_buffer(buffer, 32, overflow, 1) == -1 &&
        *api.errno_value() == 12 && buffer[0] == 0x7777);
  f = Stream(); *api.errno_value() = 0;
  Check("format-size overflow preserves prefix without marking an I/O error", api.print(f, overflow, 1) == -1 &&
        *api.errno_value() == 12 && !api.error(f));
  ExpectBytes(f, "A", 1);
  api.close(f);
#endif
#endif
}

#if (defined(_WIN32) && !defined(__aarch64__)) || defined(__BIONIC__)
int FatalFormatChild(const Api& api, bool narrow, bool positional = false) {
  int count = -1;
  if (narrow) {
    using Print = int(SYSV*)(char*, size_t, const char*, ...);
#ifdef _WIN32
    Print print = Load<Print>("snprintf");
#else
    Print print = snprintf;
#endif
    char buffer[8]; print(buffer, sizeof(buffer), positional ? "ab%1$ln" : "ab%n", &count);
  } else {
    const Wide format[] = {'a', 'b', '%', 'n', 0};
    const Wide numbered[] = {'a', 'b', '%', '1', '$', 'l', 'n', 0};
    Wide buffer[8]; api.print_buffer(buffer, 8, positional ? numbered : format, &count);
  }
  return 77;  // Reaching this sentinel means Android's fatal rule was lost.
}

bool FatalFormat(bool narrow, bool positional = false) {
#ifdef _WIN32
  wchar_t executable[32768];
  DWORD length = GetModuleFileNameW(nullptr, executable, sizeof(executable) / sizeof(*executable));
  if (length == 0 || length >= sizeof(executable) / sizeof(*executable)) return false;
  std::wstring command = std::wstring(L"\"") + executable + L"\" --fatal-" + (narrow ? L"narrow" : L"wide");
  if (positional) command += L"-positional";
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
  HANDLE reader = nullptr, writer = nullptr;
  if (!CreatePipe(&reader, &writer, &attributes, 0)) return false;
  SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW start{}; start.cb = sizeof(start); start.dwFlags = STARTF_USESTDHANDLES;
  start.hStdInput = GetStdHandle(STD_INPUT_HANDLE); start.hStdOutput = writer; start.hStdError = writer;
  PROCESS_INFORMATION process{};
  bool created = CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                nullptr, nullptr, &start, &process);
  CloseHandle(writer);
  if (!created) { CloseHandle(reader); return false; }
  auto output = std::async(std::launch::async, [reader] {
    std::string text; char bytes[1024]; DWORD size;
    while (ReadFile(reader, bytes, sizeof(bytes), &size, nullptr) && size) {
      if (text.size() < 16384) text.append(bytes, size);
    }
    CloseHandle(reader); return text;
  });
  bool finished = WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0;
  if (!finished) { TerminateProcess(process.hProcess, 78); WaitForSingleObject(process.hProcess, 5000); }
  DWORD status = 0; GetExitCodeProcess(process.hProcess, &status);
  CloseHandle(process.hThread); CloseHandle(process.hProcess);
  return finished && status != 0 && status != 77 &&
      output.get().find("FORTIFY: %n not allowed on Android") != std::string::npos;
#else
  int fds[2]; if (pipe(fds)) return false;
  pid_t child = fork();
  if (child == 0) {
    close(fds[0]); dup2(fds[1], STDERR_FILENO); close(fds[1]);
    alarm(10);
    _exit(FatalFormatChild(Resolve(), narrow, positional));
  }
  close(fds[1]);
  if (child < 0) { close(fds[0]); return false; }
  std::string text; char bytes[1024]; ssize_t size;
  while ((size = read(fds[0], bytes, sizeof(bytes))) > 0) {
    if (text.size() < 16384) text.append(bytes, size);
  }
  close(fds[0]); int status = 0;
  return waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT &&
      text.find("FORTIFY: %n not allowed on Android") != std::string::npos;
#endif
}
#endif
}  // namespace

int main(int argc, char** argv) {
  const Api api = Resolve();
#if (defined(_WIN32) && !defined(__aarch64__)) || defined(__BIONIC__)
#ifdef _WIN32
  SetErrorMode(SEM_NOGPFAULTERRORBOX);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
  if (argc == 2 && std::strcmp(argv[1], "--fatal-narrow") == 0) return FatalFormatChild(api, true);
  if (argc == 2 && std::strcmp(argv[1], "--fatal-wide") == 0) return FatalFormatChild(api, false);
  if (argc == 2 && std::strcmp(argv[1], "--fatal-narrow-positional") == 0) return FatalFormatChild(api, true, true);
  if (argc == 2 && std::strcmp(argv[1], "--fatal-wide-positional") == 0) return FatalFormatChild(api, false, true);
  Check("narrow printf %n is fatal with Bionic diagnostic", FatalFormat(true));
  Check("wide printf %n is fatal with Bionic diagnostic", FatalFormat(false));
  Check("positional narrow printf %n is fatal", FatalFormat(true, true));
  Check("positional wide printf %n is fatal", FatalFormat(false, true));
#else
  (void)argc; (void)argv;
#endif
  TestLocks(api);
  TestWideInput(api);
  TestWideOutput(api);
  TestByteLines(api);
  std::printf("bionic_stdio: %d assertions, %d failures\n", assertions, failures);
  if (failures == 0) std::puts("bionic_stdio_contract_test all passed");
  return failures == 0 ? 0 : 1;
}
