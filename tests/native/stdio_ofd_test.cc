// Runs the same public stdio corpus as the Android ELF libc test.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <io.h>
static int checks;
static int failures;
static void check(const char* name, int ok) {
  ++checks;
  if (!ok) { ++failures; fprintf(stderr, "FAIL stdio: %s\n", name); }
}
#include "stdio_ofd_contract.inc"
#include "stdio_virtual_contract.inc"
#include "stdio_cancel_contract.inc"
#include "stdio_tls_exit_contract.inc"
static FILE* exit_stream;
static void ExitOutput() { fputs("late", exit_stream); }
// Exercise the wide real parser too: the shared corpus already executes the
// narrow parser, but wide output alone cannot qualify vfwscanf's final link.
static void WideScanner() {
  char path[] = "gesher-stdio-wide-scan-XXXXXX";
  int fd = mkstemp(path);
  check("wide scanner fixture opens", fd >= 0);
  if (fd < 0) return;
  bool supplied = write(fd, "42 done!", 8) == 8 && lseek(fd, 0, SEEK_SET) == 0;
  FILE* stream = fdopen(fd, "r");
  check("wide scanner wraps the supplied descriptor", stream != nullptr);
  if (stream) {
    int value = -1;
    wchar_t word[5] = {};
    check("wide scanner reads real formatted input", supplied &&
          fwscanf(stream, L"%d %4ls", &value, word) == 2 && value == 42 && wcscmp(word, L"done") == 0);
    check("wide scanner leaves unconsumed input", fgetwc(stream) == L'!');
    check("wide scanner EOF is not an I/O error", fgetwc(stream) == WEOF && feof(stream) && !ferror(stream));
    check("wide scanner closes its descriptor", fclose(stream) == 0 && fcntl(fd, F_GETFD) == -1 && errno == EBADF);
  } else close(fd);
  unlink(path);
}
// Native CRT FILE tokens reach the lazy stream-registration path. A real
// external pipe has usable access rights even though its unmediated wait mode
// cannot be truthfully returned by F_GETFL.
static void ExternalPipeStreams() {
  HANDLE reader = INVALID_HANDLE_VALUE, writer = INVALID_HANDLE_VALUE;
  if (!CreatePipe(&reader, &writer, nullptr, 4096)) {
    check("native stdio pipe creation", false);
    return;
  }
  int fd = _open_osfhandle(reinterpret_cast<intptr_t>(writer), _O_WRONLY | _O_BINARY);
  if (fd < 0) { CloseHandle(reader); CloseHandle(writer); check("native stdio writer descriptor", false); return; }
  FILE* output = _fdopen(fd, "wb");
  if (!output) { close(fd); CloseHandle(reader); check("native CRT output token", false); return; }
  check("external writer status remains unknown", fcntl(fd, F_GETFL) == -1 && errno == EOPNOTSUPP);
  check("lazy external stream writes actual data", fwrite("io!", 1, 3, output) == 3 && fflush(output) == 0);
  DWORD available = 0, received = 0;
  char bytes[3] = {};
  bool ready = PeekNamedPipe(reader, nullptr, 0, nullptr, &available, nullptr) && available >= sizeof(bytes);
  check("native peer observes stream bytes", ready && ReadFile(reader, bytes, sizeof(bytes), &received, nullptr) &&
        received == sizeof(bytes) && memcmp(bytes, "io!", sizeof(bytes)) == 0);
  check("native output token closes", fclose(output) == 0);
  CloseHandle(reader);

  if (!CreatePipe(&reader, &writer, nullptr, 4096)) { check("native input pipe creation", false); return; }
  fd = _open_osfhandle(reinterpret_cast<intptr_t>(reader), _O_RDONLY | _O_BINARY);
  if (fd < 0) { CloseHandle(reader); CloseHandle(writer); check("native stdio reader descriptor", false); return; }
  FILE* input = _fdopen(fd, "rb");
  if (!input) { close(fd); CloseHandle(writer); check("native CRT input token", false); return; }
  DWORD sent = 0;
  bool supplied = WriteFile(writer, "in!", 3, &sent, nullptr) && sent == 3;
  CloseHandle(writer);
  check("external reader status remains unknown", fcntl(fd, F_GETFL) == -1 && errno == EOPNOTSUPP);
  memset(bytes, 0, sizeof(bytes));
  check("lazy external stream reads actual data", supplied && fread(bytes, 1, 3, input) == 3 &&
        memcmp(bytes, "in!", sizeof(bytes)) == 0);
  check("native input token closes", fclose(input) == 0);
}
int main(int argc, char** argv) {
  if (argc == 2 && strcmp(argv[1], "--tls-exit") == 0) return StdioTlsExitContract();
  if (argc == 3 && strcmp(argv[1], "--exit-output") == 0) {
    atexit(ExitOutput);
    exit_stream = fopen(argv[2], "w");
    if (!exit_stream || fputs("early", exit_stream) < 0) return 2;
    return 0;
  }
  test_stdio_ofd_contracts();
  WideScanner();
  test_stdio_virtual_contracts();
  test_stdio_cancel_contracts();
  ExternalPipeStreams();
  printf("stdio OFD contracts: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
