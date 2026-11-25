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
#include <sys/mman.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gesher/fork.h>
#include <gesher/procfs_fd.h>
#include <gesher/process_name.h>

namespace gesher {
// dirent.cc, for this test: the stream slots' handles, with the fork set held.
void StreamHandlesWhileForkSetHeld(std::vector<HANDLE>* handles);
}  // namespace gesher

// This process's handles with exactly `access` granted
// (SystemExtendedHandleInformation).
static std::vector<HANDLE> HandlesWithAccess(ULONG access) {
  using Query = LONG(NTAPI*)(ULONG, void*, ULONG, ULONG*);
  static const auto query = reinterpret_cast<Query>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation")));
  struct Entry {
    void* object;
    ULONG_PTR pid;
    ULONG_PTR value;
    ULONG granted;
    USHORT creator;
    USHORT type;
    ULONG attributes;
    ULONG reserved;
  };
  std::vector<HANDLE> found;
  std::vector<unsigned char> buffer(1 << 22);
  ULONG needed = 0;
  LONG status;
  while ((status = query(64, buffer.data(), static_cast<ULONG>(buffer.size()), &needed)) ==
         static_cast<LONG>(0xC0000004)) {
    buffer.resize(buffer.size() * 2);
  }
  if (status < 0) return found;
  ULONG_PTR count;
  memcpy(&count, buffer.data(), sizeof(count));
  const auto* entries = reinterpret_cast<const Entry*>(buffer.data() + 2 * sizeof(ULONG_PTR));
  for (ULONG_PTR i = 0; i < count; ++i) {
    if (entries[i].pid == GetCurrentProcessId() && entries[i].granted == access) {
      found.push_back(reinterpret_cast<HANDLE>(entries[i].value));
    }
  }
  return found;
}

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
      if (n != 0) fprintf(stderr, "getdents64 -> %zd errno %d after %zu names\n", n, errno, names.size());
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
  {
    // d_ino is the inode stat and lstat report for each name.
    CHECK(lseek(dir, 0, SEEK_SET) == 0);
    char all[4096];
    const ssize_t got = getdents64(dir, all, sizeof(all));
    int checked = 0, matched = 0;
    for (ssize_t at = 0; at < got;) {
      unsigned short reclen;
      unsigned long long ino;
      memcpy(&reclen, all + at + 16, 2);
      memcpy(&ino, all + at, 8);
      const std::string name = all + at + 19;
      if (name != "..") {
        struct stat st = {};
        ++checked;
        const int r = name == "." ? stat("/proc/self", &st) : lstat(("/proc/self/" + name).c_str(), &st);
        if (r == 0 &&
            static_cast<unsigned long long>(st.st_ino) == ino) {
          ++matched;
        }
      }
      at += reclen;
    }
    CHECK_MSG(checked > 5 && matched == checked, "a /proc entry's d_ino is its st_ino");
    lseek(dir, 0, SEEK_SET);
  }
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

// The first file mapping /proc/self/maps lists: its map_files name and path.
static bool FirstFileMapping(std::string* name, std::string* path) {
  const std::string maps = ReadAll("/proc/self/maps");
  for (size_t at = 0; at < maps.size();) {
    size_t end = maps.find('\n', at);
    if (end == std::string::npos) end = maps.size();
    const std::string line = maps.substr(at, end - at);
    at = end + 1;
    unsigned long long lo = 0, hi = 0;
    char file[1024] = {};
    if (sscanf(line.c_str(), "%llx-%llx %*s %*s %*s %*s %1023[^\n]", &lo, &hi, file) == 3 &&
        file[0] == '/') {
      char text[64];
      snprintf(text, sizeof(text), "%llx-%llx", lo, hi);
      *name = text;
      *path = file;
      return true;
    }
  }
  return false;
}

static std::string TempName(const char* stem) {
  wchar_t temp[MAX_PATH];
  GetTempPathW(MAX_PATH, temp);
  std::wstring w = std::wstring(temp) + L"gesher-" + std::wstring(stem, stem + strlen(stem)) + L"-" +
                   std::to_wstring(GetCurrentProcessId());
  char narrow[MAX_PATH * 3];
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, narrow, sizeof(narrow), nullptr, nullptr);
  return narrow;
}

// map_files links: what following one does everywhere (access, realpath,
// statfs's lookup, O_PATH | O_NOFOLLOW), names in either case, permissions,
// and a lookup that does not scan the address space.
static void TestMapFilesLinks() {
  printf("Testing /proc/self/map_files links (follow rule, modes, lookup cost)...\n");
  std::string name, file;
  CHECK(FirstFileMapping(&name, &file));
  const std::string entry = "/proc/self/map_files/" + name;
  struct stat st = {};

  // Following one is EPERM wherever a call follows it.
  errno = 0;
  CHECK_MSG(access(entry.c_str(), F_OK) < 0 && errno == EPERM, "access(F_OK) follows: EPERM");
  errno = 0;
  CHECK_MSG(access(entry.c_str(), R_OK) < 0 && errno == EPERM, "access(R_OK) follows: EPERM");
  CHECK_MSG(faccessat(AT_FDCWD, entry.c_str(), F_OK, AT_SYMLINK_NOFOLLOW) == 0,
            "faccessat AT_SYMLINK_NOFOLLOW checks the link itself");
  errno = 0;
  char* real = realpath(entry.c_str(), nullptr);
  CHECK_MSG(real == nullptr && errno == EPERM, "realpath follows: EPERM");
  free(real);
  std::string linux_path, target;
  int link_fd = 0;
  errno = 0;
  CHECK_MSG(gesher::ProcfsFollow(entry.c_str(), &linux_path, &link_fd, &target) < 0 && errno == EPERM,
            "statfs follows: EPERM");

  // O_PATH | O_NOFOLLOW opens the link itself; O_NOFOLLOW alone is ELOOP.
  errno = 0;
  CHECK(open(entry.c_str(), O_RDONLY | O_NOFOLLOW) < 0 && errno == ELOOP);
  int fd = open(entry.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
  CHECK_MSG(fd >= 0, "O_PATH | O_NOFOLLOW opens a map_files link");
  if (fd >= 0) {
    struct stat by_name = {};
    CHECK(fstat(fd, &st) == 0 && lstat(entry.c_str(), &by_name) == 0);
    CHECK_MSG(S_ISLNK(st.st_mode) && st.st_mode == by_name.st_mode && st.st_ino == by_name.st_ino &&
                  st.st_dev == by_name.st_dev,
              "fstat of the O_PATH fd is lstat of the link");
    char link[512];
    const ssize_t n = readlink(("/proc/self/fd/" + std::to_string(fd)).c_str(), link, sizeof(link));
    CHECK_MSG(n > 0 && std::string(link, n) ==
                           "/proc/" + std::to_string(GetCurrentProcessId()) + "/map_files/" + name,
              "readlink of its /proc/self/fd entry names the link");
    errno = 0;
    CHECK_MSG(openat(fd, "x", O_RDONLY) < 0 && errno == ENOTDIR, "a link fd is no directory");
    close(fd);
  }
  errno = 0;
  CHECK(open(entry.c_str(), O_PATH | O_NOFOLLOW | O_DIRECTORY) < 0 && errno == ENOTDIR);
  fd = open("/proc/self/exe", O_PATH | O_NOFOLLOW);
  CHECK(fd >= 0 && fstat(fd, &st) == 0 && S_ISLNK(st.st_mode) && (st.st_mode & 0777) == 0777);
  if (fd >= 0) close(fd);
  fd = open("/proc/self", O_PATH | O_NOFOLLOW);
  CHECK_MSG(fd >= 0 && fstat(fd, &st) == 0 && S_ISLNK(st.st_mode), "/proc/self itself is a link");
  if (fd >= 0) {
    char self[64];
    const ssize_t got = readlinkat(fd, "", self, sizeof(self));
    CHECK_MSG(got > 0 && std::string(self, got) == std::to_string(GetCurrentProcessId()),
              "readlinkat(fd of /proc/self, \"\") gives the pid");
    close(fd);
  }
  fd = open("/proc/self/exe", O_PATH | O_NOFOLLOW);
  if (fd >= 0) {
    char by_fd[512], by_name[512];
    const ssize_t a = readlinkat(fd, "", by_fd, sizeof(by_fd));
    const ssize_t b = readlink("/proc/self/exe", by_name, sizeof(by_name));
    CHECK_MSG(a > 0 && a == b && memcmp(by_fd, by_name, a) == 0, "readlinkat(fd of exe, \"\") is readlink");
    close(fd);
  }
  fd = open("/proc/self/maps", O_RDONLY);
  errno = 0;
  char none[8];
  CHECK_MSG(readlinkat(fd, "", none, sizeof(none)) < 0 && errno == ENOENT,
            "readlinkat(fd, \"\") of a file that is no link: ENOENT");
  if (fd >= 0) close(fd);
  {
    // A symbolic link on disk, opened O_PATH | O_NOFOLLOW.
    const std::string base = TempName("readlinkat-empty");
    const std::string name = base + "-link";
    if (symlink("some/target", name.c_str()) == 0) {
      fd = open(name.c_str(), O_PATH | O_NOFOLLOW);
      char text[64];
      const ssize_t got = fd >= 0 ? readlinkat(fd, "", text, sizeof(text)) : -1;
      CHECK_MSG(got > 0 && std::string(text, got) == "some/target",
                "readlinkat(fd of a disk symlink, \"\") gives its target");
      if (fd >= 0) close(fd);
      unlink(name.c_str());
    } else {
      printf("  (no symbolic links here: errno %d; disk readlinkat(fd, \"\") not checked)\n", errno);
    }
  }
  errno = 0;
  CHECK(open("/proc/self", O_RDONLY | O_NOFOLLOW) < 0 && errno == ELOOP);
  fd = open("/proc/self/exe", O_PATH);
  CHECK_MSG(fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode), "O_PATH alone follows the link");
  if (fd >= 0) close(fd);

  // The kernel parses the name with _parse_integer: either case.
  std::string upper = name;
  for (char& c : upper) {
    if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
  }
  char link[512];
  ssize_t n = readlink(("/proc/self/map_files/" + upper).c_str(), link, sizeof(link));
  CHECK_MSG(n > 0 && std::string(link, n) == file, "an uppercase map_files name is the same link");

  // dr-x------, by name and by fd.
  CHECK(stat("/proc/self/map_files", &st) == 0 && st.st_mode == (S_IFDIR | 0500));
  fd = open("/proc/self/map_files", O_RDONLY | O_DIRECTORY);
  CHECK_MSG(fd >= 0 && fstat(fd, &st) == 0 && st.st_mode == (S_IFDIR | 0500), "map_files is dr-x------");
  if (fd >= 0) close(fd);

  // A link's permissions are its file's open mode: 0400 read-only, 0600 for
  // a file open for writing.
  const std::string data = TempName("map-files-mode");
  int rw = open(data.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  CHECK(rw >= 0 && ftruncate(rw, 65536) == 0);
  int ro = open(data.c_str(), O_RDONLY);
  CHECK(ro >= 0);
  void* by_rw = mmap(nullptr, 4096, PROT_READ, MAP_PRIVATE, rw, 0);
  void* by_ro = mmap(nullptr, 4096, PROT_READ, MAP_PRIVATE, ro, 0);
  CHECK(by_rw != MAP_FAILED && by_ro != MAP_FAILED);
  auto link_mode = [](void* at) -> unsigned {
    MEMORY_BASIC_INFORMATION info;
    if (VirtualQuery(at, &info, sizeof(info)) != sizeof(info)) return 0;
    char text[96];
    snprintf(text, sizeof(text), "/proc/self/map_files/%llx-%llx",
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(info.BaseAddress)),
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize));
    struct stat l = {};
    return lstat(text, &l) == 0 && S_ISLNK(l.st_mode) ? (l.st_mode & 0777) : 0;
  };
  if (by_rw != MAP_FAILED) {
    CHECK_MSG(link_mode(by_rw) == 0600, "a file mapped from an O_RDWR fd: 0600");
    // An O_PATH fd on the link keeps the mode it was opened with, and
    // readlinkat(fd, "") reads the link, also after the munmap.
    char text[96];
    snprintf(text, sizeof(text), "/proc/self/map_files/%llx-%llx",
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(by_rw)),
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(by_rw) + 4096));
    const int link_fd = open(text, O_PATH | O_NOFOLLOW | O_CLOEXEC);
    char target[512];
    ssize_t got = link_fd >= 0 ? readlinkat(link_fd, "", target, sizeof(target)) : -1;
    struct stat before = {};
    CHECK(link_fd >= 0 && fstat(link_fd, &before) == 0 && before.st_mode == (S_IFLNK | 0600));
    CHECK_MSG(got > 0 && std::string(target, got).find("map-files-mode") != std::string::npos,
              "readlinkat(link fd, \"\") gives the mapped file");
    munmap(by_rw, 4096);
    struct stat after = {};
    CHECK_MSG(link_fd >= 0 && fstat(link_fd, &after) == 0 && after.st_mode == (S_IFLNK | 0600),
              "the link fd keeps its mode after the munmap");
    if (link_fd >= 0) close(link_fd);
  }
  if (by_ro != MAP_FAILED) {
    CHECK_MSG(link_mode(by_ro) == 0400, "a file mapped from an O_RDONLY fd: 0400");
    munmap(by_ro, 4096);
  }

  // The lookup's cost does not grow with the number of mappings: readlink
  // of each of ~1500 entries (Linux: microseconds) once took 70 ms each.
  std::vector<void*> views;
  for (int i = 0; i < 1500 && ro >= 0; ++i) {
    void* v = mmap(nullptr, 4096, PROT_READ, MAP_PRIVATE, ro, 0);
    if (v == MAP_FAILED) break;
    views.push_back(v);
  }
  CHECK(views.size() == 1500);
  const auto t0 = std::chrono::steady_clock::now();
  size_t resolved = 0;
  for (void* v : views) {
    char text[96];
    snprintf(text, sizeof(text), "/proc/self/map_files/%llx-%llx",
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(v)),
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(v) + 4096));
    struct stat l = {};
    if (readlink(text, link, sizeof(link)) > 0 && lstat(text, &l) == 0) ++resolved;
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  printf("  readlink+lstat of %zu map_files entries: %.3f s\n", views.size(), seconds);
  CHECK(resolved == views.size());
  CHECK_MSG(seconds < 5.0, "map_files lookups do not scan the address space");
  for (void* v : views) munmap(v, 4096);
  if (rw >= 0) close(rw);
  if (ro >= 0) close(ro);
  unlink(data.c_str());

  // statfs follows the other links: /proc/self/exe to its file, an fd link
  // to the fd; a name that is no link is procfs's.
  CHECK(gesher::ProcfsFollow("/proc/self/exe", &linux_path, &link_fd, &target) == 1 &&
        link_fd == -1 && !target.empty() && target.rfind("/proc", 0) != 0);
  fd = open(".", O_RDONLY | O_DIRECTORY);
  CHECK(gesher::ProcfsFollow(("/proc/self/fd/" + std::to_string(fd)).c_str(), &linux_path, &link_fd,
                             &target) == 1 &&
        link_fd == fd);
  close(fd);
  CHECK(gesher::ProcfsFollow("/proc/self/maps", &linux_path, &link_fd, &target) == 1 && link_fd == -1 &&
        target.empty() && linux_path == "/proc/" + std::to_string(GetCurrentProcessId()) + "/maps");
  CHECK(gesher::ProcfsFollow("/proc/self", &linux_path, &link_fd, &target) == 1 && target.empty());
  CHECK(gesher::ProcfsFollow("/data", &linux_path, &link_fd, &target) == 0);
}

// fstat of an fd open on a /proc name gives the times stat of the name does.
static void TestProcfsTimes() {
  printf("Testing /proc times (stat of a name, fstat of an fd on it)...\n");
  for (const char* name : {"/proc/self/maps", "/proc/self", "/proc/self/task", "/proc/meminfo",
                           "/sys/devices/system/cpu/online"}) {
    struct stat by_name = {}, by_fd = {};
    const int fd = open(name, O_RDONLY | O_CLOEXEC);
    CHECK_MSG(fd >= 0, name);
    if (fd < 0) continue;
    CHECK(stat(name, &by_name) == 0 && fstat(fd, &by_fd) == 0);
    CHECK_MSG(by_name.st_mtim.tv_sec == by_fd.st_mtim.tv_sec && by_name.st_mtim.tv_nsec == by_fd.st_mtim.tv_nsec &&
                  by_name.st_atim.tv_sec == by_fd.st_atim.tv_sec && by_name.st_ctim.tv_sec == by_fd.st_ctim.tv_sec,
              name);
    CHECK_MSG(by_name.st_mtim.tv_sec > 0 && by_name.st_mtim.tv_sec <= time(nullptr), name);
    close(fd);
  }
}

// The fork set never waits for a getdents64: threads reading /proc
// directories (whose snapshots take the mman and fd-table locks) do not keep
// a fork from taking every lock of its set promptly.
static void TestForkWhileProcReaders() {
  printf("Testing the fork set while threads read /proc directories...\n");
  std::atomic<bool> stop{false};
  std::vector<std::thread> readers;
  for (const char* name : {"/proc/self/map_files", "/proc/self/fd", "/proc/self/task",
                           "/proc/self/map_files", "/proc/self/fd", "/proc/self/task"}) {
    readers.emplace_back([&stop, name]() {
      const int fd = open(name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      char part[512];
      while (fd >= 0 && !stop.load()) {
        if (getdents64(fd, part, sizeof(part)) <= 0) lseek(fd, 0, SEEK_SET);
      }
      if (fd >= 0) close(fd);
    });
  }
  Sleep(50);
  int taken = 0;
  double worst = 0;
  for (int i = 0; i < 20; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    if (gesher::AcquireForkLocks(nullptr, nullptr, 5000) == 0) {
      ++taken;
      gesher::ReleaseForkLocks(false);
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (ms > worst) worst = ms;
    Sleep(5);
  }
  stop = true;
  for (std::thread& t : readers) t.join();
  printf("  fork set taken %d/20 with 6 /proc readers, worst %.1f ms\n", taken, worst);
  CHECK_MSG(taken == 20, "the fork set is taken while threads read /proc directories");
  CHECK_MSG(worst < 1000.0, "and promptly");
}

// Every directory handle of a getdents64 stream is in its registered slot
// whenever a fork holds its lock set (a forked child closes those it
// inherited): threads reopening streams all the while (lseek to 0 and read)
// never have one open outside a slot.
static void TestForkSeesStreamHandles() {
  printf("Testing that a fork sees every stream handle...\n");
  wchar_t temp[MAX_PATH];
  GetTempPathW(MAX_PATH, temp);
  char narrow[MAX_PATH * 3];
  WideCharToMultiByte(CP_UTF8, 0, temp, -1, narrow, sizeof(narrow), nullptr, nullptr);
  // Handles with that access before any stream: not the streams' (the
  // process's own, such as a directory the loader keeps open).
  constexpr ULONG kStreamAccess = FILE_LIST_DIRECTORY | SYNCHRONIZE;
  const std::vector<HANDLE> before = HandlesWithAccess(kStreamAccess);
  std::atomic<bool> stop{false};
  std::vector<std::thread> readers;
  for (int i = 0; i < 8; ++i) {
    readers.emplace_back([&stop, &narrow]() {
      const int fd = open(narrow, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      char part[256];
      while (fd >= 0 && !stop.load()) {
        lseek(fd, 0, SEEK_SET);
        getdents64(fd, part, sizeof(part));
      }
      if (fd >= 0) close(fd);
    });
  }
  Sleep(50);
  int outside = 0, seen = 0;
  for (int round = 0; round < 100; ++round) {
    if (gesher::AcquireForkLocks(nullptr, nullptr, 5000) != 0) continue;
    std::vector<HANDLE> registered;
    gesher::StreamHandlesWhileForkSetHeld(&registered);
    const std::vector<HANDLE> open = HandlesWithAccess(kStreamAccess);
    gesher::ReleaseForkLocks(false);
    seen += static_cast<int>(registered.size());
    for (HANDLE h : open) {
      if (std::find(registered.begin(), registered.end(), h) == registered.end() &&
          std::find(before.begin(), before.end(), h) == before.end()) {
        ++outside;
      }
    }
  }
  stop = true;
  for (std::thread& t : readers) t.join();
  printf("  100 fork-set rounds: %d stream handles in slots, %d directory handles outside\n", seen,
         outside);
  CHECK_MSG(seen > 0, "the readers' streams were open during the rounds");
  CHECK_MSG(outside <= 1, "a fork finds every stream handle in its slot");
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
  {
    // d_ino is the inode stat reports.
    CHECK(lseek(dir, 0, SEEK_SET) == 0);
    char all[4096];
    const ssize_t got = getdents64(dir, all, sizeof(all));
    struct stat st = {};
    bool matched = false;
    for (ssize_t at = 0; at < got;) {
      unsigned short reclen;
      unsigned long long ino;
      memcpy(&reclen, all + at + 16, 2);
      memcpy(&ino, all + at, 8);
      if (strcmp(all + at + 19, "alpha") == 0) {
        matched = stat((std::string(narrow) + "/alpha").c_str(), &st) == 0 &&
                  static_cast<unsigned long long>(st.st_ino) == ino;
      }
      at += reclen;
    }
    CHECK_MSG(matched, "getdents64's d_ino is stat's st_ino");
  }
  // A dup shares the position, as Linux's open file description does.
  CHECK(lseek(dir, 0, SEEK_SET) == 0);
  char record[24];  // "." alone
  CHECK(getdents64(dir, record, sizeof(record)) > 0);
  const int copy = dup(dir);
  std::vector<std::string> rest = Dents(copy, 4096);
  CHECK_MSG(rest.size() + 1 == names.size() && rest.back() == names.back(), "dup shares the position");
  close(copy);
  close(dir);
  for (const wchar_t* f : {L"\\alpha", L"\\beta", L"\\gamma"}) DeleteFileW((root + f).c_str());
  RemoveDirectoryW((root + L"\\sub").c_str());
  CHECK_MSG(RemoveDirectoryW(root.c_str()), "a closed listing holds no handle on the directory");
  {
    // A directory removed while an fd is open on it lists as empty, as
    // Linux's does (getdents64 returns 0).
    CreateDirectoryW(root.c_str(), nullptr);
    const int gone = open(narrow, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(gone >= 0 && RemoveDirectoryW(root.c_str()));
    char part[256];
    errno = 0;
    const ssize_t got = gone >= 0 ? getdents64(gone, part, sizeof(part)) : -1;
    CHECK_MSG(got == 0, "getdents64 of a removed directory: 0 (end of listing)");
    if (got != 0) printf("  removed directory: getdents64 %zd errno %d\n", got, errno);
    if (gone >= 0) close(gone);
  }

  // Many entries: one enumeration serves the calls that continue it, and
  // two threads reading one fd share the entries out (Linux serializes
  // them on the description's f_pos_lock), each entry once.
  root += L"-big";
  CreateDirectoryW(root.c_str(), nullptr);
  constexpr int kFiles = 3000;
  for (int i = 0; i < kFiles; ++i) {
    HANDLE h = CreateFileW((root + L"\\file-with-a-longer-name-" + std::to_wstring(i)).c_str(), GENERIC_WRITE,
                           0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  }
  WideCharToMultiByte(CP_UTF8, 0, root.c_str(), -1, narrow, sizeof(narrow), nullptr, nullptr);
  dir = open(narrow, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  CHECK(dir >= 0);
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<std::string> all = Dents(dir, 4096);
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  printf("  getdents64 of %d entries with a 4 KiB buffer: %.3f s\n", kFiles, seconds);
  CHECK(all.size() == kFiles + 2);
  CHECK_MSG(std::set<std::string>(all.begin(), all.end()).size() == all.size(), "each entry once");
  CHECK(lseek(dir, 0, SEEK_SET) == 0);
  std::mutex lock;
  std::vector<std::string> shared;
  auto reader = [&]() {
    std::vector<char> buffer(256);
    for (;;) {
      ssize_t got = getdents64(dir, buffer.data(), buffer.size());
      if (got <= 0) break;
      std::lock_guard<std::mutex> guard(lock);
      for (ssize_t at = 0; at < got;) {
        unsigned short reclen;
        memcpy(&reclen, buffer.data() + at + 16, 2);
        shared.push_back(buffer.data() + at + 19);
        at += reclen;
      }
    }
  };
  std::thread a(reader), b(reader);
  a.join();
  b.join();
  CHECK_MSG(shared.size() == all.size() &&
                std::set<std::string>(shared.begin(), shared.end()).size() == shared.size(),
            "two threads on one fd read each entry once between them");
  close(dir);

  // Streams hold handles only while their descriptions live: 300 fds each
  // part way through the listing, then closed, leave no handle behind.
  DWORD handles_before = 0, handles_open = 0, handles_after = 0;
  GetProcessHandleCount(GetCurrentProcess(), &handles_before);
  std::vector<int> many;
  for (int i = 0; i < 300; ++i) {
    const int d = open(narrow, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char part[512];
    if (d >= 0 && getdents64(d, part, sizeof(part)) > 0) many.push_back(d);
  }
  GetProcessHandleCount(GetCurrentProcess(), &handles_open);
  for (int d : many) close(d);
  GetProcessHandleCount(GetCurrentProcess(), &handles_after);
  printf("  handles: %lu before, %lu with 300 streams, %lu after closing them\n", handles_before,
         handles_open, handles_after);
  CHECK(many.size() == 300);
  CHECK_MSG(handles_after <= handles_before + 4, "a closed description's stream closes its handle");

  // The fd is resolved once per call: while one thread reads a directory fd
  // with getdents64, another puts a file in its place (dup2) and back. The
  // directory's position must never land in the file's description.
  {
    const std::string data = TempName("getdents-race");
    const int file = open(data.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    const int target = open(narrow, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    const int directory = open(narrow, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(file >= 0 && target >= 0 && directory >= 0);
    std::atomic<bool> stop{false};
    std::thread reader([&]() {
      char part[256];
      while (!stop.load()) {
        if (getdents64(target, part, sizeof(part)) == 0) lseek(target, 0, SEEK_SET);
      }
    });
    int moved = 0;
    for (int i = 0; i < 20000; ++i) {
      if (dup2(file, target) < 0) break;
      if (lseek(file, 0, SEEK_CUR) != 0) {
        ++moved;
        lseek(file, 0, SEEK_SET);
      }
      if (dup2(directory, target) < 0) break;
      if (lseek(file, 0, SEEK_CUR) != 0) {
        ++moved;
        lseek(file, 0, SEEK_SET);
      }
    }
    stop = true;
    reader.join();
    CHECK_MSG(moved == 0, "getdents64 never moves the position of a file that replaced its fd");
    close(file);
    close(target);
    close(directory);
    unlink(data.c_str());
  }
  for (int i = 0; i < kFiles; ++i) {
    DeleteFileW((root + L"\\file-with-a-longer-name-" + std::to_wstring(i)).c_str());
  }
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
  TestMapFilesLinks();
  TestProcfsTimes();
  TestDiskGetdents();
  TestForkWhileProcReaders();
  TestForkSeesStreamHandles();

  if (g_failed == 0) {
    printf("procfs & sysfs contracts: all passed (%d checks)\n", g_checks);
    return 0;
  }
  fprintf(stderr, "FAILED: %d/%d checks failed\n", g_failed, g_checks);
  return 1;
}
