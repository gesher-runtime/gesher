// Roadmap N04: Virtual procfs & sysfs contracts for Gesher (Issue #56)
// Verifies guest runtime contracts for:
//   /proc/self/auxv (and /proc/<pid>/auxv)
//   /proc/self/maps (and /proc/<pid>/maps)
//   /proc/self/cmdline (and /proc/<pid>/cmdline)
//   /proc/cpuinfo
//   /proc/meminfo
//   /sys/devices/system/cpu/online, present, possible

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <windows.h>

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

int main() {
  TestAuxv();
  TestMaps();
  TestCmdline();
  TestCpuinfo();
  TestMeminfo();
  TestSysfs();
  TestSelfLink();

  if (g_failed == 0) {
    printf("procfs & sysfs contracts: all passed (%d checks)\n", g_checks);
    return 0;
  }
  fprintf(stderr, "FAILED: %d/%d checks failed\n", g_failed, g_checks);
  return 1;
}
