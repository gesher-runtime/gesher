// Roadmap N04: Virtual procfs & sysfs contracts for Gesher (Issue #56)
// Verifies guest runtime contracts for:
//   /proc/self/auxv (and /proc/<pid>/auxv)
//   /proc/self/maps (and /proc/<pid>/maps)
//   /proc/self/cmdline (and /proc/<pid>/cmdline)
//   /proc/cpuinfo
//   /proc/meminfo
//   /sys/devices/system/cpu/online, present, possible
//   fds open on /proc names: fstat, readlink of /proc/self/fd/<n>, directory
//   fds (getdents64, openat), and /proc/self/map_files

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <windows.h>

#include <set>
#include <string>
#include <vector>

#include <gesher/process_name.h>

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

#define CHECK_MSG(cond, msg)                                                                           \
  do {                                                                                                \
    ++g_checks;                                                                                       \
    if (!(cond)) {                                                                                    \
      fprintf(stderr, "FAIL: %s:%d: assertion failed: %s (%s)\n", __FILE__, __LINE__, #cond, (msg)); \
      ++g_failed;                                                                                     \
    }                                                                                                 \
  } while (0)

static std::string ReadAll(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return "";
  std::string s;
  char buf[4096];
  while (size_t n = fread(buf, 1, sizeof(buf), f)) {
    s.append(buf, n);
  }
  fclose(f);
  return s;
}

static void TestAuxv() {
  printf("Testing /proc/self/auxv and /proc/<pid>/auxv...\n");
  std::string pid = std::to_string(GetCurrentProcessId());
  std::string self_auxv = ReadAll("/proc/self/auxv");
  std::string pid_auxv = ReadAll(("/proc/" + pid + "/auxv").c_str());

  CHECK(!self_auxv.empty());
  CHECK_MSG(self_auxv == pid_auxv, "self/auxv and <pid>/auxv must match identically");
  CHECK_MSG((self_auxv.size() % 16) == 0, "auxv size must be a multiple of 16 bytes (Elf64_auxv_t)");

  const auto* pairs = reinterpret_cast<const uint64_t*>(self_auxv.data());
  size_t count = self_auxv.size() / 16;
  CHECK(count >= 10);

  bool has_pagesz = false;
  bool has_clktck = false;
  bool has_hwcap = false;
  bool has_uid = false;
  bool has_euid = false;
  bool has_gid = false;
  bool has_egid = false;
  bool has_random = false;
  bool has_platform = false;
  bool has_execfn = false;
  bool has_null = false;

  for (size_t i = 0; i < count; ++i) {
    uint64_t type = pairs[2 * i];
    uint64_t val = pairs[2 * i + 1];
    switch (type) {
      case 0:  // AT_NULL
        has_null = true;
        CHECK(val == 0);
        CHECK_MSG(i == count - 1, "AT_NULL must be the final entry in auxv");
        break;
      case 6:  // AT_PAGESZ
        has_pagesz = true;
        CHECK(val == 4096);
        break;
      case 17:  // AT_CLKTCK
        has_clktck = true;
        CHECK(val == 100);
        break;
      case 16:  // AT_HWCAP
        has_hwcap = true;
        break;
      case 11:  // AT_UID
        has_uid = true;
        CHECK(val == static_cast<uint64_t>(getuid()));
        break;
      case 12:  // AT_EUID
        has_euid = true;
        CHECK(val == static_cast<uint64_t>(getuid()));
        break;
      case 13:  // AT_GID
        has_gid = true;
        CHECK(val == static_cast<uint64_t>(getgid()));
        break;
      case 14:  // AT_EGID
        has_egid = true;
        CHECK(val == static_cast<uint64_t>(getgid()));
        break;
      case 25: {  // AT_RANDOM
        has_random = true;
        CHECK(val != 0);
        const auto* rand_ptr = reinterpret_cast<const uint8_t*>(val);
        // AT_RANDOM must point to readable memory with at least some non-zero bytes
        bool any_nonzero = false;
        for (int b = 0; b < 16; ++b) {
          if (rand_ptr[b] != 0) any_nonzero = true;
        }
        CHECK_MSG(any_nonzero, "AT_RANDOM bytes must be initialized by RNG");
        break;
      }
      case 15: {  // AT_PLATFORM
        has_platform = true;
        CHECK(val != 0);
        const char* plat = reinterpret_cast<const char*>(val);
        CHECK(strcmp(plat, "x86_64") == 0 || strcmp(plat, "aarch64") == 0);
        break;
      }
      case 31: {  // AT_EXECFN
        has_execfn = true;
        CHECK(val != 0);
        const char* fn = reinterpret_cast<const char*>(val);
        CHECK(strlen(fn) > 0 && fn[0] == '/');
        break;
      }
      default:
        break;
    }
  }

  CHECK(has_pagesz);
  CHECK(has_clktck);
  CHECK(has_hwcap);
  CHECK(has_uid);
  CHECK(has_euid);
  CHECK(has_gid);
  CHECK(has_egid);
  CHECK(has_random);
  CHECK(has_platform);
  CHECK(has_execfn);
  CHECK(has_null);
}

static void TestMaps() {
  printf("Testing /proc/self/maps and /proc/<pid>/maps...\n");
  std::string pid = std::to_string(GetCurrentProcessId());
  void* allocation = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  CHECK(allocation != nullptr);
  std::string self_maps = ReadAll("/proc/self/maps");
  std::string pid_maps = ReadAll(("/proc/" + pid + "/maps").c_str());

  CHECK(!self_maps.empty());
  // Each open is a fresh snapshot: the first read itself can grow the heap.
  // Both aliases must describe a stable live allocation, without freezing maps.
  for (const auto* snapshot : {&self_maps, &pid_maps}) {
    bool found_allocation = false;
    size_t at = 0;
    while (at < snapshot->size()) {
      unsigned long long lo = 0, hi = 0;
      char perms[5] = {};
      if (sscanf(snapshot->c_str() + at, "%llx-%llx %4s", &lo, &hi, perms) == 3 &&
          lo <= reinterpret_cast<uintptr_t>(allocation) &&
          reinterpret_cast<uintptr_t>(allocation) < hi && strcmp(perms, "rw-p") == 0) {
        found_allocation = true;
      }
      size_t next = snapshot->find('\n', at);
      if (next == std::string::npos) break;
      at = next + 1;
    }
    CHECK_MSG(found_allocation, "both maps aliases describe the actual live allocation");
  }
  if (allocation) VirtualFree(allocation, 0, MEM_RELEASE);

  // Verify parsing of standard Linux maps lines:
  // %llx-%llx %4s %llx %x:%x %llu [pathname]
  FILE* f = fopen("/proc/self/maps", "r");
  CHECK(f != nullptr);
  char line[2048];
  bool found_stack = false;
  bool found_heap = false;
  bool found_file = false;
  size_t line_count = 0;

  while (fgets(line, sizeof(line), f)) {
    ++line_count;
    unsigned long long start = 0, end = 0, offset = 0, inode = 0;
    char perms[16] = {};
    unsigned major = 0, minor = 0;
    char path[1024] = {};
    int parsed = sscanf(line, "%llx-%llx %15s %llx %x:%x %llu %1023[^\r\n]",
                        &start, &end, perms, &offset, &major, &minor, &inode, path);
    CHECK(parsed >= 7);
    CHECK(start < end);
    CHECK(strlen(perms) == 4);
    CHECK(perms[0] == 'r' || perms[0] == '-');
    CHECK(perms[1] == 'w' || perms[1] == '-');
    CHECK(perms[2] == 'x' || perms[2] == '-');
    CHECK(perms[3] == 'p' || perms[3] == 's');

    if (parsed == 8) {
      std::string spath(path);
      if (spath == "[stack]") found_stack = true;
      if (spath == "[heap]") found_heap = true;
      if (spath.rfind("/", 0) == 0) {
        found_file = true;
        // Mapped file entries should have valid device numbers
      }
    }
  }
  fclose(f);

  CHECK(line_count > 5);
  CHECK(found_stack);
  CHECK(found_heap);
  CHECK(found_file);
}

static void TestCmdline() {
  printf("Testing /proc/self/cmdline and /proc/<pid>/cmdline...\n");
  std::string pid = std::to_string(GetCurrentProcessId());
  std::string initial_cmd = ReadAll("/proc/self/cmdline");
  std::string pid_cmd = ReadAll(("/proc/" + pid + "/cmdline").c_str());

  CHECK(!initial_cmd.empty());
  CHECK(initial_cmd == pid_cmd);
  // Null-separated arguments: must end with null
  CHECK(initial_cmd.back() == '\0');

  // Test setArgV0 contract
  gesher_set_process_argv0("com.gesher.test.runtime");
  std::string new_cmd = ReadAll("/proc/self/cmdline");
  std::string new_pid_cmd = ReadAll(("/proc/" + pid + "/cmdline").c_str());
  CHECK(new_cmd == std::string("com.gesher.test.runtime", sizeof("com.gesher.test.runtime")));
  CHECK(new_cmd == new_pid_cmd);

  // Restore
  gesher_set_process_argv0(nullptr);
}

static void TestCpuinfo() {
  printf("Testing /proc/cpuinfo...\n");
  std::string cpuinfo = ReadAll("/proc/cpuinfo");
  CHECK(!cpuinfo.empty());
  CHECK(cpuinfo.find("processor\t: 0\n") != std::string::npos);
#if defined(__x86_64__)
  CHECK(cpuinfo.find("vendor_id\t: ") != std::string::npos);
  CHECK(cpuinfo.find("cpu family\t: ") != std::string::npos);
  CHECK(cpuinfo.find("model\t\t: ") != std::string::npos);
  CHECK(cpuinfo.find("model name\t: ") != std::string::npos);
  CHECK(cpuinfo.find("flags\t\t: ") != std::string::npos);
  CHECK(cpuinfo.find("fpu\t\t: yes\n") != std::string::npos);
#elif defined(__aarch64__)
  CHECK(cpuinfo.find("Features\t: ") != std::string::npos);
#endif
}

static void TestMeminfo() {
  printf("Testing /proc/meminfo...\n");
  std::string meminfo = ReadAll("/proc/meminfo");
  CHECK(!meminfo.empty());
  CHECK(meminfo.rfind("MemTotal:", 0) == 0);
  CHECK(meminfo.find("MemFree:") != std::string::npos);
  CHECK(meminfo.find("MemAvailable:") != std::string::npos);
  CHECK(meminfo.find("Cached:") != std::string::npos);
  CHECK(meminfo.find("SwapTotal:") != std::string::npos);
  CHECK(meminfo.find("SwapFree:") != std::string::npos);
  CHECK(meminfo.find("CommitLimit:") != std::string::npos);
  CHECK(meminfo.find("Committed_AS:") != std::string::npos);
  // Windows has no Linux vmalloc/LRU/slab accounting. Unsupported fields
  // must stay absent rather than reporting invented Linux kernel counters.
  for (const char* unsupported : {"VmallocTotal:", "VmallocUsed:", "Active:",
                                  "Inactive:", "Slab:", "SReclaimable:", "SUnreclaim:"}) {
    CHECK(meminfo.find(unsupported) == std::string::npos);
  }

  // Parse MemTotal and MemAvailable to verify realistic values
  unsigned long long total = 0, avail = 0;
  FILE* f = fopen("/proc/meminfo", "r");
  CHECK(f != nullptr);
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    if (sscanf(line, "MemTotal: %llu kB", &total) == 1) {}
    if (sscanf(line, "MemAvailable: %llu kB", &avail) == 1) {}
  }
  fclose(f);
  CHECK(total > 0);
  CHECK(avail > 0);
  CHECK(avail <= total);
}

static void TestSysfs() {
  printf("Testing /sys/devices/system/cpu/{online,present,possible}...\n");
  for (const char* leaf : {"online", "present", "possible"}) {
    std::string path = std::string("/sys/devices/system/cpu/") + leaf;
    std::string content = ReadAll(path.c_str());
    CHECK_MSG(!content.empty(), path.c_str());
    CHECK_MSG(content.back() == '\n', "sysfs cpu lists must terminate with newline");
    CHECK_MSG(content.rfind("0", 0) == 0, "sysfs cpu lists must start with CPU 0");

    struct stat st;
    CHECK(stat(path.c_str(), &st) == 0);
    CHECK(S_ISREG(st.st_mode));
  }

  // Nonexistent sysfs path must fail
  CHECK(open("/sys/devices/system/cpu/unknown", O_RDONLY) == -1 && errno == ENOENT);
  CHECK(open("/sys/other", O_RDONLY) == -1 && errno == ENOENT);
}

static void TestSelfLink() {
  printf("Testing /proc/self link and lstat semantics...\n");
  struct stat st;
  CHECK(lstat("/proc/self", &st) == 0);
  CHECK_MSG(S_ISLNK(st.st_mode), "lstat(/proc/self) must report S_IFLNK");

  CHECK(stat("/proc/self", &st) == 0);
  CHECK_MSG(S_ISDIR(st.st_mode), "stat(/proc/self) following symlink must report S_IFDIR");

  char target[256] = {};
  ssize_t n = readlink("/proc/self", target, sizeof(target) - 1);
  CHECK(n > 0);
  std::string expected_pid = std::to_string(GetCurrentProcessId());
  CHECK(std::string(target, n) == expected_pid);
}

// A random (version 4, RFC 4122 variant) UUID, lowercase, then a newline.
static bool IsUuidLine(const std::string& s) {
  if (s.size() != 37 || s.back() != '\n') return false;
  for (size_t i = 0; i < 36; ++i) {
    bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash ? s[i] != '-' : !((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) {
      return false;
    }
  }
  return s[14] == '4' && (s[19] == '8' || s[19] == '9' || s[19] == 'a' || s[19] == 'b');
}

// This executable run with --boot-id: the boot_id another process of the same
// device reads.
static std::string ChildBootId() {
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring command = L"\"" + std::wstring(exe) + L"\" --boot-id";
  SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
  HANDLE read_end = nullptr, write_end = nullptr;
  if (!CreatePipe(&read_end, &write_end, &sa, 0)) return "";
  SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = write_end;
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION pi = {};
  std::string out;
  if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
    CloseHandle(write_end);
    write_end = nullptr;
    char buf[256];
    DWORD n = 0;
    while (ReadFile(read_end, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
  if (write_end != nullptr) CloseHandle(write_end);
  CloseHandle(read_end);
  return out;
}

static void TestRandom() {
  printf("Testing /proc/sys/kernel/random/boot_id...\n");
  CHECK(open("/proc/sys/kernel/random/uuid", O_RDONLY) == -1 && errno == ENOENT);

  struct stat st;
  CHECK(stat("/proc/sys/kernel/random/boot_id", &st) == 0 && S_ISREG(st.st_mode));
  CHECK(stat("/proc/sys/kernel/random", &st) == 0 && S_ISDIR(st.st_mode));
  CHECK(stat("/proc/sys/kernel", &st) == 0 && S_ISDIR(st.st_mode));
  CHECK(stat("/proc/sys", &st) == 0 && S_ISDIR(st.st_mode));
  CHECK(open("/proc/sys/kernel/random/entropy_avail", O_RDONLY) == -1 && errno == ENOENT);
  CHECK(open("/proc/sys/kernel/osrelease", O_RDONLY) == -1 && errno == ENOENT);
  CHECK(open("/proc/sys/kernel/random/boot_id", O_WRONLY) == -1 && errno == EACCES);

  // Outside a device: one id for this process.
  SetEnvironmentVariableW(L"GESHER_BINDER", nullptr);
  std::string own = ReadAll("/proc/sys/kernel/random/boot_id");
  CHECK_MSG(IsUuidLine(own), own.c_str());
  CHECK(ReadAll("/proc/sys/kernel/random/boot_id") == own);

  // A device: every process of it reads the same id, kept in its data directory;
  // a device restart (a new $GESHER_BINDER) is a new boot with a new id.
  wchar_t temp[MAX_PATH];
  GetTempPathW(MAX_PATH, temp);
  std::wstring data = std::wstring(temp) + L"gesher-boot-id-" + std::to_wstring(GetCurrentProcessId());
  CreateDirectoryW(data.c_str(), nullptr);
  SetEnvironmentVariableW(L"ANDROID_DATA", data.c_str());
  SetEnvironmentVariableW(L"GESHER_PROPERTY_ROOT_SESSION", nullptr);
  std::wstring first = L"gesher-boot-test-a-" + std::to_wstring(GetCurrentProcessId());
  SetEnvironmentVariableW(L"GESHER_BINDER", first.c_str());
  std::string boot1 = ReadAll("/proc/sys/kernel/random/boot_id");
  CHECK_MSG(IsUuidLine(boot1), boot1.c_str());
  CHECK(boot1 != own);
  CHECK(ReadAll("/proc/sys/kernel/random/boot_id") == boot1);
  std::string child = ChildBootId();
  CHECK_MSG(child == boot1, child.c_str());

  std::wstring second = L"gesher-boot-test-b-" + std::to_wstring(GetCurrentProcessId());
  SetEnvironmentVariableW(L"GESHER_BINDER", second.c_str());
  std::string boot2 = ReadAll("/proc/sys/kernel/random/boot_id");
  CHECK_MSG(IsUuidLine(boot2), boot2.c_str());
  CHECK(boot2 != boot1);
  CHECK(ChildBootId() == boot2);
  SetEnvironmentVariableW(L"GESHER_BINDER", first.c_str());
  CHECK(ChildBootId() == boot1);
  CHECK(ReadAll("/proc/sys/kernel/random/boot_id") == boot1);

  // A device cannot claim boot identity from an unwriteable data root. An
  // ordinary file obstructs its system directory even on an elevated runner.
  std::wstring blocked = data + L"\\blocked";
  HANDLE obstruction = CreateFileW(blocked.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  CHECK(obstruction != INVALID_HANDLE_VALUE);
  if (obstruction != INVALID_HANDLE_VALUE) {
    CloseHandle(obstruction);
    SetEnvironmentVariableW(L"ANDROID_DATA", blocked.c_str());
    std::wstring third = L"gesher-boot-test-c-" + std::to_wstring(GetCurrentProcessId());
    SetEnvironmentVariableW(L"GESHER_BINDER", third.c_str());
    errno = 0;
    int unreadable = open("/proc/sys/kernel/random/boot_id", O_RDONLY);
    CHECK(unreadable == -1 && errno == EIO);
    if (unreadable >= 0) close(unreadable);
    SetEnvironmentVariableW(L"ANDROID_DATA", data.c_str());
    std::string boot3 = ReadAll("/proc/sys/kernel/random/boot_id");
    CHECK_MSG(IsUuidLine(boot3), boot3.c_str());
    CHECK(ChildBootId() == boot3);
    DeleteFileW(blocked.c_str());
  }

  SetEnvironmentVariableW(L"GESHER_BINDER", nullptr);
  CHECK(ReadAll("/proc/sys/kernel/random/boot_id") == own);
  std::wstring system = data + L"\\system";
  WIN32_FIND_DATAW entry;
  HANDLE find = FindFirstFileW((system + L"\\gesher-boot-id-*").c_str(), &entry);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      DeleteFileW((system + L"\\" + entry.cFileName).c_str());
    } while (FindNextFileW(find, &entry));
    FindClose(find);
  }
  RemoveDirectoryW(system.c_str());
  RemoveDirectoryW(data.c_str());
}

// The names getdents64 gives for the directory open as fd, read with a
// buffer of `size` bytes (a small one takes several calls).
static std::vector<std::string> Dents(int fd, size_t size) {
  std::vector<std::string> names;
  std::vector<char> buffer(size);
  for (int calls = 0; calls < 100000; ++calls) {
    ssize_t n = getdents64(fd, buffer.data(), buffer.size());
    if (n <= 0) {
      CHECK_MSG(n == 0, "getdents64 ends with 0");
      break;
    }
    for (ssize_t at = 0; at < n;) {
      unsigned short reclen;
      memcpy(&reclen, buffer.data() + at + 16, 2);
      CHECK(reclen >= 24 && reclen % 8 == 0 && at + reclen <= n);
      if (reclen < 24 || at + reclen > n) return names;
      unsigned long long ino;
      memcpy(&ino, buffer.data() + at, 8);
      CHECK_MSG(ino != 0, "getdents64 entries have an inode number");
      names.push_back(buffer.data() + at + 19);
      at += reclen;
    }
  }
  return names;
}

static bool Has(const std::vector<std::string>& names, const char* name) {
  for (const std::string& n : names) {
    if (n == name) return true;
  }
  return false;
}

static void TestProcfsDescriptors() {
  printf("Testing fds of /proc names (fstat, readlink, getdents64, map_files)...\n");
  const std::string pid = std::to_string(GetCurrentProcessId());

  // An fd open on /proc/self/maps is a /proc file: what stat of the name says.
  int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
  CHECK(fd >= 0);
  struct stat by_fd = {}, by_name = {};
  CHECK(fstat(fd, &by_fd) == 0);
  CHECK(stat("/proc/self/maps", &by_name) == 0);
  CHECK_MSG(S_ISREG(by_fd.st_mode) && (by_fd.st_mode & 0777) == 0444, "fstat: a read-only file");
  CHECK_MSG(by_fd.st_size == 0, "fstat: size 0, as Linux's /proc files");
  CHECK_MSG(by_fd.st_dev == by_name.st_dev && by_fd.st_ino == by_name.st_ino && by_fd.st_ino != 0,
            "fstat of the fd and stat of the name agree");
  char link[512];
  const std::string fd_link = "/proc/self/fd/" + std::to_string(fd);
  ssize_t n = readlink(fd_link.c_str(), link, sizeof(link));
  CHECK_MSG(n > 0 && std::string(link, n) == "/proc/" + pid + "/maps",
            "readlink of /proc/self/fd/<n> names /proc/<pid>/maps");
  char head[32];
  CHECK_MSG(read(fd, head, sizeof(head)) > 0, "the fd still reads the content");
  close(fd);

  // A /proc directory opens as a directory fd.
  int dir = open("/proc/self", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  CHECK(dir >= 0);
  CHECK(fstat(dir, &by_fd) == 0 && S_ISDIR(by_fd.st_mode));
  n = readlink(("/proc/self/fd/" + std::to_string(dir)).c_str(), link, sizeof(link));
  CHECK(n > 0 && std::string(link, n) == "/proc/" + pid);
  errno = 0;
  CHECK_MSG(open("/proc/self", O_WRONLY) < 0 && errno == EISDIR, "a directory opens read-only");
  errno = 0;
  CHECK(open("/proc/self/maps", O_RDONLY | O_DIRECTORY) < 0 && errno == ENOTDIR);
  int status = openat(dir, "status", O_RDONLY | O_CLOEXEC);
  CHECK_MSG(status >= 0, "openat relative to a /proc directory fd");
  if (status >= 0) {
    std::string text;
    char buf[256];
    while ((n = read(status, buf, sizeof(buf))) > 0) text.append(buf, static_cast<size_t>(n));
    CHECK(text.rfind("Name:", 0) == 0);
    close(status);
  }
  std::vector<std::string> names = Dents(dir, 64);  // a record or two per call
  CHECK(Has(names, ".") && Has(names, "..") && Has(names, "maps") && Has(names, "status") &&
        Has(names, "map_files") && Has(names, "fd"));
  CHECK_MSG(lseek(dir, 0, SEEK_SET) == 0 && Dents(dir, 4096) == names,
            "lseek to 0 rewinds; the buffer size does not change the listing");
  char tiny[16];
  errno = 0;
  lseek(dir, 0, SEEK_SET);
  CHECK_MSG(getdents64(dir, tiny, sizeof(tiny)) < 0 && errno == EINVAL,
            "a buffer too small for an entry is EINVAL");
  close(dir);
  fd = open("/proc/self/maps", O_RDONLY);
  errno = 0;
  CHECK(getdents64(fd, tiny, sizeof(tiny)) < 0 && errno == ENOTDIR);
  close(fd);

  // /proc/self/map_files: a link per file mapping, named "%lx-%lx".
  dir = open("/proc/self/map_files", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  CHECK(dir >= 0);
  std::vector<std::string> links = Dents(dir, 4096);
  close(dir);
  std::string maps = ReadAll("/proc/self/maps");
  std::set<std::string> file_ranges;
  std::vector<std::pair<std::string, std::string>> files;  // name, path
  for (size_t at = 0; at < maps.size();) {
    size_t end = maps.find('\n', at);
    if (end == std::string::npos) end = maps.size();
    const std::string line = maps.substr(at, end - at);
    at = end + 1;
    unsigned long long lo = 0, hi = 0;
    char path[1024] = {};
    if (sscanf(line.c_str(), "%llx-%llx %*s %*s %*s %*s %1023[^\n]", &lo, &hi, path) == 3 &&
        path[0] == '/') {
      char name[64];
      snprintf(name, sizeof(name), "%llx-%llx", lo, hi);
      file_ranges.insert(name);
      files.push_back({name, path});
    }
  }
  CHECK_MSG(!files.empty(), "maps has file mappings (this executable)");
  size_t listed = 0;
  for (const std::string& l : links) {
    if (l == "." || l == "..") continue;
    ++listed;
    CHECK_MSG(file_ranges.count(l) == 1, "each map_files entry is a file mapping of maps");
  }
  CHECK_MSG(listed == file_ranges.size(), "map_files lists every file mapping of maps");
  if (!files.empty()) {
    const std::string entry = "/proc/self/map_files/" + files[0].first;
    n = readlink(entry.c_str(), link, sizeof(link));
    CHECK_MSG(n > 0 && std::string(link, n) == files[0].second, "readlink names the mapped file");
    struct stat st = {};
    CHECK(lstat(entry.c_str(), &st) == 0 && S_ISLNK(st.st_mode) && (st.st_mode & 0400) != 0);
    errno = 0;
    CHECK_MSG(stat(entry.c_str(), &st) < 0 && errno == EPERM, "following a map_files link is EPERM");
    errno = 0;
    CHECK_MSG(open(entry.c_str(), O_RDONLY) < 0 && errno == EPERM, "opening one is EPERM");
    errno = 0;
    CHECK(lstat(("/proc/self/map_files/0" + files[0].first).c_str(), &st) < 0 && errno == ENOENT);
  }
  struct stat st = {};
  errno = 0;
  CHECK(lstat("/proc/self/map_files/1-2", &st) < 0 && errno == ENOENT);
}

// getdents64 on a directory on disk.
static void TestDiskGetdents() {
  printf("Testing getdents64 on a directory on disk...\n");
  wchar_t temp[MAX_PATH];
  GetTempPathW(MAX_PATH, temp);
  std::wstring root = std::wstring(temp) + L"gesher-getdents-" + std::to_wstring(GetCurrentProcessId());
  CreateDirectoryW(root.c_str(), nullptr);
  for (const wchar_t* f : {L"\\alpha", L"\\beta", L"\\gamma"}) {
    HANDLE h = CreateFileW((root + f).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  }
  CreateDirectoryW((root + L"\\sub").c_str(), nullptr);
  char narrow[MAX_PATH * 3];
  WideCharToMultiByte(CP_UTF8, 0, root.c_str(), -1, narrow, sizeof(narrow), nullptr, nullptr);
  int dir = open(narrow, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  CHECK(dir >= 0);
  std::vector<std::string> names = Dents(dir, 48);
  CHECK(names.size() == 6 && Has(names, ".") && Has(names, "..") && Has(names, "alpha") &&
        Has(names, "beta") && Has(names, "gamma") && Has(names, "sub"));
  CHECK(lseek(dir, 0, SEEK_SET) == 0 && Dents(dir, 4096) == names);
  close(dir);
  for (const wchar_t* f : {L"\\alpha", L"\\beta", L"\\gamma"}) DeleteFileW((root + f).c_str());
  RemoveDirectoryW((root + L"\\sub").c_str());
  RemoveDirectoryW(root.c_str());
}

int main(int argc, char** argv) {
  if (argc == 2 && strcmp(argv[1], "--boot-id") == 0) {
    std::string id = ReadAll("/proc/sys/kernel/random/boot_id");
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), id.data(), static_cast<DWORD>(id.size()), &written, nullptr);
    return id.empty() ? 1 : 0;
  }
  TestAuxv();
  TestMaps();
  TestCmdline();
  TestCpuinfo();
  TestMeminfo();
  TestSysfs();
  TestSelfLink();
  TestRandom();
  TestProcfsDescriptors();
  TestDiskGetdents();

  if (g_failed == 0) {
    printf("procfs & sysfs contracts: all passed (%d checks)\n", g_checks);
    return 0;
  }
  fprintf(stderr, "FAILED: %d/%d checks failed\n", g_failed, g_checks);
  return 1;
}
