// Gesher's ELF loader as /system/bin/linker64 (ADR 0096), through gesher-base's
// production open/stat/access and /proc/self/maps, and exec_spawn's launch
// preparation:
//
//   system_linker_test <empty directory used as $ANDROID_DATA>
//
// It checks what an app's code reads to learn the device's 64-bit machine (an
// Ijiami shell, for one: e_machine of /system/bin/linker or linker64, then
// whether a /proc/self/maps line names "/bin/linker64"), that there is no
// 32-bit /system/bin/linker, that the file cannot be written through the name,
// that `linker64 PROGRAM ARGS` prepares the launch of PROGRAM, and that running
// the file itself fails to link instead of running anything.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <windows.h>

#include <string>
#include <vector>

#include "exec_spawn.h"

namespace ex = gesher::exec;

static int g_failed = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                      \
  do {                                                                                   \
    ++g_checks;                                                                          \
    if (!(cond)) {                                                                       \
      fprintf(stderr, "FAIL: %s:%d: assertion failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failed;                                                                        \
    }                                                                                    \
  } while (0)

static std::string ReadPath(const char* path) {
  std::string s;
  const int fd = open(path, O_RDONLY);
  if (fd < 0) return s;
  char buf[4096];
  for (;;) {
    const int n = static_cast<int>(read(fd, buf, sizeof(buf)));
    if (n <= 0) break;
    s.append(buf, static_cast<size_t>(n));
  }
  close(fd);
  return s;
}

static unsigned U16(const std::string& s, size_t at) {
  return static_cast<unsigned char>(s[at]) | static_cast<unsigned char>(s[at + 1]) << 8;
}

static void TestHeader() {
  printf("the ELF header of /system/bin/linker64\n");
  struct stat st {};
  CHECK(stat("/system/bin/linker64", &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 64);
  CHECK(access("/system/bin/linker64", R_OK) == 0);
  const std::string image = ReadPath("/system/bin/linker64");
  CHECK(image.size() >= 64 && static_cast<long long>(image.size()) == st.st_size);
  if (image.size() < 64) return;
  CHECK(image.compare(0, 4, "\x7f" "ELF") == 0);
  CHECK(image[4] == 2);           // ELFCLASS64
  CHECK(image[5] == 1);           // little-endian
  CHECK(U16(image, 16) == 3);     // ET_DYN, as Android's linker64
  CHECK(U16(image, 18) == 0x3e);  // EM_X86_64: byte 18 is what the shell switches on
  CHECK(image[18] == 0x3e && image[19] == 0);
  // No entry point: nothing runs if it is executed as a program.
  for (int i = 24; i < 32; ++i) CHECK(image[i] == 0);
  // It names itself as Gesher's.
  CHECK(image.find("Gesher ELF loader") != std::string::npos);
  CHECK(image.find(std::string("Gesher\0", 7)) != std::string::npos);
  printf("PASS  /system/bin/linker64 is ELF64 LSB EM_X86_64, Gesher's ELF loader\n");

  // Gesher runs no 32-bit code: no 32-bit linker.
  errno = 0;
  CHECK(access("/system/bin/linker", F_OK) != 0 && errno == ENOENT);
  CHECK(open("/system/bin/linker", O_RDONLY) < 0);
  // Read-only through the name.
  CHECK(open("/system/bin/linker64", O_WRONLY) < 0);
  CHECK(open("/system/bin/linker64", O_RDWR | O_TRUNC) < 0);
  CHECK(ReadPath("/system/bin/linker64") == image);
  printf("PASS  no /system/bin/linker; linker64 cannot be written\n");
}

static void TestMaps() {
  printf("/proc/self/maps\n");
  struct stat st {};
  CHECK(stat("/system/bin/linker64", &st) == 0);
  const std::string maps = ReadPath("/proc/self/maps");
  CHECK(!maps.empty());
  bool contains = false, named = false;
  size_t start = 0;
  while (start < maps.size()) {
    size_t end = maps.find('\n', start);
    if (end == std::string::npos) end = maps.size();
    const std::string line = maps.substr(start, end - start);
    start = end + 1;
    // What an Ijiami shell's il() looks for.
    if (line.find("/bin/linker64") != std::string::npos) contains = true;
    const std::string suffix = " /system/bin/linker64";
    if (line.size() > suffix.size() && line.compare(line.size() - suffix.size(), suffix.size(), suffix) == 0) {
      unsigned long long low = 0, high = 0, offset = 0, inode = 0;
      char perms[5] = {};
      unsigned major = 0, minor = 0;
      if (sscanf(line.c_str(), "%llx-%llx %4s %llx %x:%x %llu", &low, &high, perms, &offset, &major, &minor,
                 &inode) == 7) {
        named = true;
        CHECK(perms[0] == 'r' && perms[1] == '-' && perms[2] == '-');
        CHECK(offset == 0);
        CHECK(inode == static_cast<unsigned long long>(st.st_ino));
        // The mapping holds the file's bytes.
        CHECK(high > low && memcmp(reinterpret_cast<const void*>(low), "\x7f" "ELF", 4) == 0);
        CHECK(reinterpret_cast<const unsigned char*>(low)[18] == 0x3e);
      }
    }
  }
  CHECK(contains);
  CHECK(named);
  // Mapped once, for the process's lifetime.
  const std::string again = ReadPath("/proc/self/maps");
  size_t count = 0;
  for (size_t at = again.find(" /system/bin/linker64\n"); at != std::string::npos;
       at = again.find(" /system/bin/linker64\n", at + 1)) {
    ++count;
  }
  CHECK(count == 1);
  printf("PASS  /proc/self/maps maps /system/bin/linker64 once, from offset 0\n");

  // /proc/self/fd names an open linker64 by its name too.
  const int fd = open("/system/bin/linker64", O_RDONLY);
  CHECK(fd >= 0);
  if (fd >= 0) {
    char target[512] = {};
    const std::string link = "/proc/self/fd/" + std::to_string(fd);
    const ssize_t n = readlink(link.c_str(), target, sizeof(target) - 1);
    CHECK(n > 0 && std::string(target, static_cast<size_t>(n)) == "/system/bin/linker64");
    close(fd);
  }
  printf("PASS  /proc/self/fd names it /system/bin/linker64\n");
}

static std::wstring FindFile(const std::wstring& data) {
  WIN32_FIND_DATAW found;
  HANDLE h = FindFirstFileW((data + L"\\system\\gesher-linker\\*").c_str(), &found);
  if (h == INVALID_HANDLE_VALUE) return L"";
  std::wstring file;
  do {
    if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && found.cFileName[0] != L'.') {
      file = data + L"\\system\\gesher-linker\\" + found.cFileName + L"\\linker64";
    }
  } while (FindNextFileW(h, &found));
  FindClose(h);
  return file;
}

static void TestExec(const std::wstring& data) {
  printf("exec of /system/bin/linker64\n");
  // An ELF x86-64 position-independent executable's header is all
  // PrepareLaunch reads.
  const std::wstring program = data + L"\\program";
  {
    unsigned char header[64] = {0x7f, 'E', 'L', 'F', 2, 1, 1};
    header[16] = 3;     // ET_DYN
    header[18] = 0x3e;  // EM_X86_64
    HANDLE h = CreateFileW(program.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD written = 0;
    CHECK(h != INVALID_HANDLE_VALUE && WriteFile(h, header, sizeof(header), &written, nullptr));
    CloseHandle(h);
  }
  const std::string program_path = ex::Narrow(program);
  ex::Launch direct, via;
  CHECK(ex::PrepareLaunch({program_path, "one", "two"}, "", &direct) == 0);
  CHECK(ex::PrepareLaunch({"/system/bin/linker64", program_path, "one", "two"}, "", &via) == 0);
  CHECK(via.kind == "elf");
  CHECK(via.application == direct.application);
  CHECK(via.command_line == direct.command_line);
  // A tool runs as on a device, where it is an ELF executable.
  CHECK(ex::PrepareLaunch({"/system/bin/linker64", "/system/bin/ls", "-l"}, "", &via) == 0 &&
        via.kind == "tool");
  // Nothing to run, linker64's own options or a program that is not ELF.
  CHECK(ex::PrepareLaunch({"/system/bin/linker64"}, "", &via) == ex::kENOEXEC);
  CHECK(ex::PrepareLaunch({"/system/bin/linker64", "--list", program_path}, "", &via) == ex::kENOEXEC);
  CHECK(ex::PrepareLaunch({"/system/bin/linker64", "/system/bin/sh"}, "", &via) == ex::kENOEXEC);
  CHECK(ex::PrepareLaunch({"/system/bin/linker64", ex::Narrow(data + L"\\missing")}, "", &via) == ex::kENOENT);
  printf("PASS  linker64 PROGRAM ARGS launches PROGRAM; nothing else runs\n");

  // The file itself as a program: the ELF loader finds no entry point and
  // stops, before any code of it could run.
  const std::wstring file = FindFile(data);
  CHECK(!file.empty());
  const std::wstring launcher = ex::LauncherPath();
  if (file.empty() || GetFileAttributesW(launcher.c_str()) == INVALID_FILE_ATTRIBUTES) {
    printf("SKIP  gesher-exec.exe is not next to this test\n");
    return;
  }
  std::wstring command;
  ex::AppendQuoted(&command, launcher);
  command += L" --elf ";
  ex::AppendQuoted(&command, file);
  command += L" linker64";
  SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, TRUE};
  HANDLE read_end = nullptr, write_end = nullptr;
  CHECK(CreatePipe(&read_end, &write_end, &inherit, 0));
  SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = write_end;
  si.hStdError = write_end;
  PROCESS_INFORMATION pi = {};
  const BOOL started = CreateProcessW(launcher.c_str(), command.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(write_end);
  CHECK(started);
  std::string output;
  char buf[1024];
  DWORD got = 0;
  while (ReadFile(read_end, buf, sizeof(buf), &got, nullptr) && got > 0) output.append(buf, got);
  CloseHandle(read_end);
  if (!started) return;
  WaitForSingleObject(pi.hProcess, 30000);
  DWORD status = 0;
  GetExitCodeProcess(pi.hProcess, &status);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  CHECK(status != 0 && status != STILL_ACTIVE);
  CHECK(output.find("no entry point") != std::string::npos);
  printf("PASS  running the file itself fails to link (exit %lu)\n", static_cast<unsigned long>(status));
}

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: system_linker_test <empty directory for $ANDROID_DATA>\n");
    return 2;
  }
  const std::wstring data = ex::Widen(argv[1]);
  CreateDirectoryW(data.c_str(), nullptr);
  SetEnvironmentVariableW(L"ANDROID_DATA", data.c_str());
  TestHeader();
  TestMaps();
  TestExec(data);
  if (g_failed != 0) {
    printf("system_linker_test: %d of %d checks failed\n", g_failed, g_checks);
    return 1;
  }
  printf("system_linker_test: all %d checks passed\n", g_checks);
  return 0;
}
