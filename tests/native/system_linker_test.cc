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
#include <sys/time.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include <gesher/elf_loader.h>

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

static uint32_t ImageHash();

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
  printf("PASS  no /system/bin/linker\n");

  // Other spellings of the name are the same file; a trailing slash is not.
  CHECK(ReadPath("/system//bin/./linker64") == image);
  CHECK(ReadPath("/system/bin/../bin/linker64") == image);
  CHECK(ReadPath("/../system/bin/linker64") == image);
  CHECK(open("/system/bin/linker64/", O_RDONLY) < 0);
  printf("PASS  /system//bin/./linker64 and /system/bin/../bin/linker64 name it\n");
}

// /system is a read-only file system on Android: every change through the
// name is EROFS, and the file stays as it was.
#define CHECK_EROFS(call)        \
  do {                           \
    errno = 0;                   \
    CHECK((call) == -1);         \
    CHECK(errno == EROFS);       \
  } while (0)

static void TestReadOnly(const std::wstring& data) {
  printf("changes through /system/bin/linker64\n");
  const char* name = "/system/bin/linker64";
  const std::string before = ReadPath(name);
  struct stat st_before {};
  CHECK(stat(name, &st_before) == 0);
  CHECK_EROFS(open(name, O_WRONLY));
  CHECK_EROFS(open(name, O_RDWR));
  CHECK_EROFS(open(name, O_RDONLY | O_TRUNC));
  CHECK_EROFS(open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644));
  CHECK_EROFS(open("/system//bin/../bin/linker64", O_WRONLY | O_APPEND));
  CHECK_EROFS(openat(AT_FDCWD, name, O_RDWR));
  CHECK_EROFS(creat(name, 0644));
  CHECK_EROFS(unlink(name));
  CHECK_EROFS(unlinkat(AT_FDCWD, name, 0));
  CHECK_EROFS(remove(name));
  const std::string other = ex::Narrow(data + L"\\other");
  {
    const int fd = open(other.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    if (fd >= 0) close(fd);
  }
  CHECK_EROFS(rename(name, (other + "-moved").c_str()));
  CHECK_EROFS(rename(other.c_str(), name));
  CHECK_EROFS(renameat(AT_FDCWD, other.c_str(), AT_FDCWD, name));
  CHECK_EROFS(link(name, (other + "-link").c_str()));
  CHECK_EROFS(link(other.c_str(), name));
  CHECK_EROFS(symlink(other.c_str(), name));
  CHECK_EROFS(chmod(name, 0777));
  CHECK_EROFS(fchmodat(AT_FDCWD, name, 0600, 0));
  CHECK_EROFS(chown(name, 0, 0));
  CHECK_EROFS(utimes(name, nullptr));
  CHECK_EROFS(utimensat(AT_FDCWD, name, nullptr, 0));
  // Reads still work, and the file is unchanged.
  CHECK(access(name, R_OK) == 0);
  CHECK(ReadPath(name) == before);
  struct stat st_after {};
  CHECK(stat(name, &st_after) == 0 && st_after.st_ino == st_before.st_ino &&
        st_after.st_size == st_before.st_size && st_after.st_mtime == st_before.st_mtime);
  // The file holding it is an ordinary one (not read-only), so cleanups of
  // Gesher's data directory can remove it.
  WIN32_FIND_DATAW found;
  HANDLE find = FindFirstFileW((data + L"\\system\\gesher-linker\\*").c_str(), &found);
  std::wstring file;
  if (find != INVALID_HANDLE_VALUE) {
    do {
      if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && found.cFileName[0] != L'.') {
        const std::wstring candidate = data + L"\\system\\gesher-linker\\" + found.cFileName + L"\\linker64";
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) file = candidate;
      }
    } while (FindNextFileW(find, &found));
    FindClose(find);
  }
  const DWORD attributes = GetFileAttributesW(file.c_str());
  CHECK(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) == 0);
  printf("PASS  open for writing, unlink, rename, link, symlink, chmod, chown, utimes: EROFS\n");
}

static void TestRealpath() {
  printf("realpath\n");
  char resolved[4096];
  CHECK(realpath("/system/bin/linker64", resolved) != nullptr &&
        strcmp(resolved, "/system/bin/linker64") == 0);
  CHECK(realpath("/system//bin/../bin/./linker64", resolved) != nullptr &&
        strcmp(resolved, "/system/bin/linker64") == 0);
  const int fd = open("/system/bin/linker64", O_RDONLY);
  CHECK(fd >= 0);
  if (fd >= 0) {
    const std::string link = "/proc/self/fd/" + std::to_string(fd);
    CHECK(realpath(link.c_str(), resolved) != nullptr && strcmp(resolved, "/system/bin/linker64") == 0);
    close(fd);
  }
  printf("PASS  realpath names it /system/bin/linker64\n");
}

static void TestDlopen() {
  printf("loading it as a library\n");
  std::string error;
  gesher::elf::Library* lib = gesher::elf::Load("/system/bin/linker64", &error);
  CHECK(lib == nullptr);
  CHECK(error.find("Gesher's ELF loader") != std::string::npos);
  // Nothing was mapped for it: maps still names it once.
  const std::string maps = ReadPath("/proc/self/maps");
  size_t count = 0;
  for (size_t at = maps.find(" /system/bin/linker64\n"); at != std::string::npos;
       at = maps.find(" /system/bin/linker64\n", at + 1)) {
    ++count;
  }
  CHECK(count == 1);
  printf("PASS  the ELF loader refuses to load it as a library\n");
}

static uint32_t ImageHash() {
  const std::string image = ReadPath("/system/bin/linker64");
  uint32_t fnv = 2166136261u;
  for (unsigned char c : image) fnv = (fnv ^ c) * 16777619u;
  return fnv;
}

// Leftovers placed before the first use of the name, checked after it.
static void PreparePrune(const std::wstring& data) {
  const std::wstring root = data + L"\\system\\gesher-linker";
  CreateDirectoryW((data + L"\\system").c_str(), nullptr);
  CreateDirectoryW(root.c_str(), nullptr);
  auto make = [](const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  };
  for (const wchar_t* name : {L"deadbeef", L"cafef00d"}) {
    CreateDirectoryW((root + L"\\" + name).c_str(), nullptr);
    make(root + L"\\" + name + L"\\linker64");
  }
  // deadbeef: unused for eight days.
  HANDLE dir = CreateFileW((root + L"\\deadbeef").c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ |
                           FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  ULARGE_INTEGER t;
  t.LowPart = now.dwLowDateTime;
  t.HighPart = now.dwHighDateTime;
  t.QuadPart -= 8ull * 24 * 3600 * 10000000ull;
  FILETIME old = {t.LowPart, t.HighPart};
  SetFileTime(dir, nullptr, nullptr, &old);
  CloseHandle(dir);
}

static void TestPrune(const std::wstring& data) {
  printf("leftovers of earlier images\n");
  const std::wstring root = data + L"\\system\\gesher-linker";
  CHECK(GetFileAttributesW((root + L"\\deadbeef").c_str()) == INVALID_FILE_ATTRIBUTES);
  CHECK(GetFileAttributesW((root + L"\\cafef00d\\linker64").c_str()) != INVALID_FILE_ATTRIBUTES);
  printf("PASS  an image directory unused for a week is removed, a recent one kept\n");
}

static void TestTemporaryPrune(const std::wstring& data) {
  // A dead process's temporary file in the current image's directory goes
  // the next time a process starts using the name; this process's stays.
  wchar_t hash[16];
  swprintf(hash, 16, L"%08x", ImageHash());
  const std::wstring dir = data + L"\\system\\gesher-linker\\" + hash;
  const std::wstring dead = dir + L"\\linker64.0.tmp";
  const std::wstring mine = dir + L"\\linker64." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
  for (const std::wstring& path : {dead, mine}) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  }
  // Another process (this test again, with --use) uses the name.
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  std::wstring command;
  ex::AppendQuoted(&command, self);
  command += L" --use ";
  ex::AppendQuoted(&command, data);
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  const BOOL started = CreateProcessW(self, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                      nullptr, nullptr, &si, &pi);
  CHECK(started);
  if (!started) return;
  WaitForSingleObject(pi.hProcess, 30000);
  DWORD status = 1;
  GetExitCodeProcess(pi.hProcess, &status);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  CHECK(status == 0);
  CHECK(GetFileAttributesW(dead.c_str()) == INVALID_FILE_ATTRIBUTES);
  CHECK(GetFileAttributesW(mine.c_str()) != INVALID_FILE_ATTRIBUTES);
  DeleteFileW(mine.c_str());
  printf("PASS  a dead process's temporary file is removed, a running one's kept\n");
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

// The file holding the current image.
static std::wstring FindFile(const std::wstring& data) {
  wchar_t hash[16];
  swprintf(hash, 16, L"%08x", ImageHash());
  const std::wstring file = data + L"\\system\\gesher-linker\\" + hash + L"\\linker64";
  return GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES ? file : L"";
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

  // The file itself as a program: the ELF loader refuses it (it has no entry
  // point either), before any code of it could run.
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
  CHECK(output.find("CANNOT LINK EXECUTABLE") != std::string::npos &&
        output.find("Gesher's ELF loader") != std::string::npos);
  printf("PASS  running the file itself fails to link (exit %lu)\n", static_cast<unsigned long>(status));
}

// Other names and descriptors that reach the file: a symbolic link to the
// name (Linux allows it; writes through it are EROFS), fds opened read-only
// through the name, extended attributes.
static void TestOtherNames(const std::wstring& data) {
  printf("changes through other names and descriptors\n");
  const char* name = "/system/bin/linker64";
  const std::string before = ReadPath(name);
  const std::string link = ex::Narrow(data + L"\\to-linker64");
  if (symlink(name, link.c_str()) == 0) {
    CHECK_EROFS(open(link.c_str(), O_WRONLY));
    CHECK_EROFS(open(link.c_str(), O_RDWR | O_TRUNC));
    CHECK_EROFS(open(link.c_str(), O_WRONLY | O_APPEND));
    CHECK(ReadPath(link.c_str()) == before);
    char target[4096] = {};
    const ssize_t n = readlink(link.c_str(), target, sizeof(target) - 1);
    CHECK(n > 0 && std::string(target, static_cast<size_t>(n)) == name);
    if (n <= 0 || std::string(target, static_cast<size_t>(n)) != name) {
      fprintf(stderr, "readlink: n=%d errno=%d %s\n", static_cast<int>(n), errno, target);
    }
    char resolved[4096];
    CHECK(realpath(link.c_str(), resolved) != nullptr && strcmp(resolved, name) == 0);
    CHECK_EROFS(setxattr(link.c_str(), "user.gesher", "v", 1, 0));
    printf("PASS  a symbolic link to it: readable, readlink and realpath name it, writes EROFS\n");
  } else {
    printf("SKIP  symbolic links need developer mode or a privilege here (errno %d)\n", errno);
  }
  CHECK_EROFS(setxattr(name, "user.gesher", "v", 1, 0));
  CHECK_EROFS(lsetxattr(name, "user.gesher", "v", 1, 0));
  CHECK_EROFS(removexattr(name, "user.gesher"));
  CHECK_EROFS(lremovexattr(name, "user.gesher"));
  const int fd = open(name, O_RDONLY);
  CHECK(fd >= 0);
  if (fd >= 0) {
    CHECK_EROFS(fchmod(fd, 0644));
    CHECK_EROFS(futimens(fd, nullptr));
    CHECK_EROFS(utimensat(fd, "", nullptr, AT_EMPTY_PATH));
    CHECK_EROFS(fsetxattr(fd, "user.gesher", "v", 1, 0));
    CHECK_EROFS(fremovexattr(fd, "user.gesher"));
    close(fd);
  }
  CHECK(ReadPath(name) == before);
  const DWORD attributes = GetFileAttributesW(FindFile(data).c_str());
  CHECK(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) == 0);
  // Names Linux would not resolve to the file.
  CHECK(open("/system/bin/linker64/.", O_RDONLY) < 0);
  CHECK(open("/system/bin/missing/../linker64", O_RDONLY) < 0);
  printf("PASS  xattrs, fchmod, futimens, utimensat(AT_EMPTY_PATH) through the name or an fd: EROFS\n");
}

// Without a file (its directory cannot be made), changing opens of the name
// are still EROFS, and reading it finds nothing.
static int NoFileChild(const char* data) {
  const std::wstring blocker = ex::Widen(data) + L"\\blocker";
  HANDLE h = CreateFileW(blocker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  SetEnvironmentVariableW(L"ANDROID_DATA", blocker.c_str());
  struct stat st {};
  errno = 0;
  if (stat("/system/bin/linker64", &st) == 0 || errno != ENOENT) return 1;
  errno = 0;
  if (open("/system/bin/linker64", O_WRONLY | O_CREAT, 0644) != -1 || errno != EROFS) return 2;
  return 0;
}

static void TestNoFile(const std::wstring& data) {
  printf("no file\n");
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  std::wstring command;
  ex::AppendQuoted(&command, self);
  command += L" --no-file ";
  ex::AppendQuoted(&command, data);
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  const BOOL started = CreateProcessW(self, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                      nullptr, nullptr, &si, &pi);
  CHECK(started);
  if (!started) return;
  WaitForSingleObject(pi.hProcess, 30000);
  DWORD status = 99;
  GetExitCodeProcess(pi.hProcess, &status);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  CHECK(status == 0);
  printf("PASS  without a file: ENOENT to read, EROFS to create (status %lu)\n",
         static_cast<unsigned long>(status));
}

// chmod and utimes through a symbolic or a hard link to the file, and
// readlink of a chain of links.
static void TestLinks(const std::wstring& data) {
  printf("links to the file\n");
  const char* name = "/system/bin/linker64";
  const std::string before = ReadPath(name);
  struct stat st_before {};
  CHECK(stat(name, &st_before) == 0);
  // A hard link made with Win32 (link() through the name is EROFS).
  const std::wstring hard = data + L"\\hard-linker64";
  if (CreateHardLinkW(hard.c_str(), FindFile(data).c_str(), nullptr)) {
    const std::string h = ex::Narrow(hard);
    CHECK_EROFS(chmod(h.c_str(), 0777));
    CHECK_EROFS(utimes(h.c_str(), nullptr));
    CHECK_EROFS(utimensat(AT_FDCWD, h.c_str(), nullptr, 0));
    CHECK_EROFS(utimensat(AT_FDCWD, h.c_str(), nullptr, AT_SYMLINK_NOFOLLOW));
    CHECK_EROFS(open(h.c_str(), O_WRONLY));
    printf("PASS  a hard link: chmod, utimes, utimensat, open for writing: EROFS\n");
    DeleteFileW(hard.c_str());
  } else {
    printf("SKIP  no hard link (error %lu)\n", GetLastError());
  }
  const std::string b = ex::Narrow(data + L"\\link-b");
  const std::string a = ex::Narrow(data + L"\\link-a");
  if (symlink(name, b.c_str()) == 0 && symlink(b.c_str(), a.c_str()) == 0) {
    CHECK_EROFS(chmod(b.c_str(), 0777));
    CHECK_EROFS(chmod(a.c_str(), 0777));
    CHECK_EROFS(utimes(a.c_str(), nullptr));
    CHECK_EROFS(utimensat(AT_FDCWD, b.c_str(), nullptr, 0));
    // The links themselves are not the file.
    CHECK(utimensat(AT_FDCWD, a.c_str(), nullptr, AT_SYMLINK_NOFOLLOW) == 0);
    char target[4096] = {};
    ssize_t n = readlink(b.c_str(), target, sizeof(target) - 1);
    CHECK(n > 0 && std::string(target, static_cast<size_t>(n)) == name);
    // A's target is B, not the file B leads to.
    n = readlink(a.c_str(), target, sizeof(target) - 1);
    const std::string got = n > 0 ? std::string(target, static_cast<size_t>(n)) : std::string();
    CHECK(got != name && got.size() >= 7 && got.compare(got.size() - 7, 7, "/link-b") == 0);
    printf("PASS  symbolic links: chmod and utimes EROFS; readlink of A -> B -> file gives B\n");
  } else {
    printf("SKIP  symbolic links need developer mode or a privilege here (errno %d)\n", errno);
  }
  CHECK(ReadPath(name) == before);
  struct stat st_after {};
  CHECK(stat(name, &st_after) == 0 && st_after.st_mtime == st_before.st_mtime);
  const DWORD attributes = GetFileAttributesW(FindFile(data).c_str());
  CHECK(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) == 0);
}

static bool WaitForFile(const std::wstring& path) {
  for (int i = 0; i < 1000; ++i) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    Sleep(10);
  }
  return false;
}

static void Touch(const std::wstring& path) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

// --hold <data>: uses the name (making and holding the file), links to it,
// then waits for "done".
static int HoldChild(const char* data_arg) {
  const std::wstring data = ex::Widen(data_arg);
  SetEnvironmentVariableW(L"ANDROID_DATA", data.c_str());
  struct stat st {};
  if (stat("/system/bin/linker64", &st) != 0) return 1;
  if (symlink("/system/bin/linker64", ex::Narrow(data + L"\\late-link").c_str()) != 0) return 77;
  Touch(data + L"\\go");
  return WaitForFile(data + L"\\done") ? 0 : 2;
}

// --late <data>: looks for the file before it exists (a write open), then,
// once another process made and holds it, writes through a link to it.
static int LateChild(const char* data_arg) {
  const std::wstring data = ex::Widen(data_arg);
  SetEnvironmentVariableW(L"ANDROID_DATA", data.c_str());
  const std::string scratch = ex::Narrow(data + L"\\scratch");
  const int fd = open(scratch.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return 1;
  close(fd);
  Touch(data + L"\\probed");
  if (!WaitForFile(data + L"\\go")) return 2;
  const ULONGLONG start = GetTickCount64();
  errno = 0;
  const int w = open(ex::Narrow(data + L"\\late-link").c_str(), O_WRONLY);
  const ULONGLONG elapsed = GetTickCount64() - start;
  if (w >= 0) {
    close(w);
    return 3;
  }
  if (errno != EROFS) return 4;
  printf("late open refused in %llu ms\n", static_cast<unsigned long long>(elapsed));
  return 0;
}

static HANDLE StartSelf(const wchar_t* mode, const std::wstring& data) {
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  std::wstring command;
  ex::AppendQuoted(&command, self);
  command += L" ";
  command += mode;
  command += L" ";
  ex::AppendQuoted(&command, data);
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessW(self, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      nullptr, &si, &pi)) {
    return nullptr;
  }
  CloseHandle(pi.hThread);
  return pi.hProcess;
}

static DWORD Finish(HANDLE process) {
  DWORD status = 99;
  if (process == nullptr) return status;
  WaitForSingleObject(process, 30000);
  GetExitCodeProcess(process, &status);
  CloseHandle(process);
  return status;
}

static void TestLateProbe(const std::wstring& base) {
  printf("a process that looked before the file existed\n");
  const std::wstring data = base + L"\\late";
  CreateDirectoryW(data.c_str(), nullptr);
  HANDLE late = StartSelf(L"--late", data);
  CHECK(late != nullptr);
  CHECK(WaitForFile(data + L"\\probed"));
  HANDLE hold = StartSelf(L"--hold", data);
  CHECK(hold != nullptr);
  const DWORD late_status = Finish(late);
  Touch(data + L"\\done");
  const DWORD hold_status = Finish(hold);
  if (hold_status == 77) {
    printf("SKIP  symbolic links need developer mode or a privilege here\n");
    return;
  }
  CHECK(hold_status == 0);
  CHECK(late_status == 0);
  printf("PASS  it looks again when a write is refused for sharing: EROFS (status %lu)\n",
         static_cast<unsigned long>(late_status));
}

int main(int argc, char** argv) {
  if (argc == 3 && strcmp(argv[1], "--no-file") == 0) return NoFileChild(argv[2]);
  if (argc == 3 && strcmp(argv[1], "--hold") == 0) return HoldChild(argv[2]);
  if (argc == 3 && strcmp(argv[1], "--late") == 0) return LateChild(argv[2]);
  if (argc == 3 && strcmp(argv[1], "--use") == 0) {
    // A second process using the name (TestTemporaryPrune).
    SetEnvironmentVariableW(L"ANDROID_DATA", ex::Widen(argv[2]).c_str());
    struct stat st {};
    return stat("/system/bin/linker64", &st) == 0 ? 0 : 1;
  }
  if (argc != 2) {
    fprintf(stderr, "usage: system_linker_test <empty directory for $ANDROID_DATA>\n");
    return 2;
  }
  const std::wstring data = ex::Widen(argv[1]);
  CreateDirectoryW(data.c_str(), nullptr);
  SetEnvironmentVariableW(L"ANDROID_DATA", data.c_str());
  PreparePrune(data);
  TestHeader();
  TestPrune(data);
  TestReadOnly(data);
  TestMaps();
  TestRealpath();
  TestDlopen();
  TestExec(data);
  TestTemporaryPrune(data);
  TestOtherNames(data);
  TestNoFile(data);
  TestLinks(data);
  TestLateProbe(data);
  if (g_failed != 0) {
    printf("system_linker_test: %d of %d checks failed\n", g_failed, g_checks);
    return 1;
  }
  printf("system_linker_test: all %d checks passed\n", g_checks);
  return 0;
}
