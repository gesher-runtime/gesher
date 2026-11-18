// gesher-base: Linux's /proc for this process, and /etc/timezone
// (docs/decisions/0045-posix-paths-tz-procfs.md).
//
// The names are virtual (path.cc's kVirtual): open reads a snapshot of the
// file, made when it is opened, from an anonymous file (memfd_create); stat
// reports size 0, as Linux does for /proc files; opendir lists the virtual
// directories (dirent.cc); readlink reads the links. /proc/<pid> is this
// process's own pid only; other processes have no /proc entries.
//
//   /proc/self/{auxv,cmdline,comm,environ,status,stat,statm,maps,mounts}
//   /proc/self/fd/<n>            links to the file open as fd n
//   /proc/self/exe, cwd          links to the executable, the current directory
//   /proc/self/task/<tid>/{comm,stat,status}
//   /proc/{meminfo,cpuinfo,uptime,mounts}
//   /etc/timezone                the IANA id of Windows' time zone (libcore)
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <windows.h>
// bcrypt.h needs windows.h first.
#include <bcrypt.h>
#if defined(__x86_64__)
#include <intrin.h>
#endif
#include <psapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <dirent.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "internal.h"
#include "ofd_internal.h"
#include "revocable_internal.h"
#include "record_lock_descriptors.h"

#include <gesher/process_name.h>

namespace gesher {
namespace procfs {
namespace {

// The name android.os.Process.setArgV0 gave this process (gesher/process_name.h),
// or empty: /proc/self/cmdline is then the Windows command line.
std::mutex g_argv0_mutex;
std::string g_argv0;

std::string Argv0() {
  std::lock_guard<std::mutex> lock(g_argv0_mutex);
  return g_argv0;
}

constexpr unsigned kNone = 0;
constexpr unsigned kFile = S_IFREG;
constexpr unsigned kDir = S_IFDIR;
constexpr unsigned kLink = S_IFLNK;

std::string Format(const char* format, ...) __attribute__((format(printf, 1, 2)));
std::string Format(const char* format, ...) {
  char buf[512];
  va_list args;
  va_start(args, format);
  int n = vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  return std::string(buf, n < 0 ? 0 : std::min<size_t>(n, sizeof(buf) - 1));
}

std::vector<std::string> Split(const std::wstring& name) {
  std::string narrow = Narrow(name.c_str());
  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= narrow.size()) {
    size_t end = narrow.find('/', start);
    if (end == std::string::npos) end = narrow.size();
    if (end > start) parts.push_back(narrow.substr(start, end - start));
    start = end + 1;
  }
  return parts;
}

bool ParseNumber(const std::string& s, unsigned long* value) {
  if (s.empty() || s.size() > 10 || s.find_first_not_of("0123456789") != std::string::npos) {
    return false;
  }
  *value = strtoul(s.c_str(), nullptr, 10);
  return true;
}

// The open fds: CRT fds with a handle (sockets, eventfds and epolls are CRT
// fds on NUL too).
std::vector<int> OpenFds() {
  std::vector<int> fds;
  for (int fd = 0; fd < 8192; ++fd) {
    if (_get_osfhandle(fd) != -1) fds.push_back(fd);
  }
  return fds;
}

std::vector<DWORD> ThreadIds() {
  std::vector<DWORD> tids;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return tids;
  THREADENTRY32 entry;
  memset(&entry, 0, sizeof(entry));
  entry.dwSize = sizeof(entry);
  DWORD pid = GetCurrentProcessId();
  for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID == pid) tids.push_back(entry.th32ThreadID);
  }
  CloseHandle(snapshot);
  std::sort(tids.begin(), tids.end());
  return tids;
}

bool IsOwnThread(unsigned long tid) {
  if (tid == GetCurrentThreadId()) return true;
  // A targeted task lookup must not enumerate every thread on the machine.
  // An exited thread can still have a handle and an owner, so also check its
  // signalled state. Holding this handle prevents reuse during both queries.
  HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, tid);
  if (thread == nullptr) {
    // A custom thread DACL may deny even limited queries. Keep the previous
    // directory-visibility contract for that uncommon case.
    if (GetLastError() != ERROR_ACCESS_DENIED) return false;
    const auto tids = ThreadIds();
    return std::binary_search(tids.begin(), tids.end(), tid);
  }
  bool own = GetProcessIdOfThread(thread) == GetCurrentProcessId() &&
             WaitForSingleObject(thread, 0) == WAIT_TIMEOUT;
  CloseHandle(thread);
  return own;
}

// The executable's name without its extension, at most 15 bytes (Linux's
// TASK_COMM_LEN).
std::string ProcessComm() {
  // An app process: the name setArgV0 gave it, as Android's SetThreadName
  // shortens it (its last 15 bytes when it has a '.' and no '@').
  const std::string argv0 = Argv0();
  if (!argv0.empty()) {
    if (argv0.size() < 15 || argv0.find('@') != std::string::npos ||
        argv0.find('.') == std::string::npos) {
      return argv0.substr(0, 15);
    }
    return argv0.substr(argv0.size() - 15);
  }
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring name(path, n);
  size_t slash = name.find_last_of(L"\\/");
  if (slash != std::wstring::npos) name = name.substr(slash + 1);
  size_t dot = name.rfind(L'.');
  if (dot != std::wstring::npos) name.resize(dot);
  std::string comm = Narrow(name.c_str());
  size_t end = std::min<size_t>(comm.size(), 15);
  if (end < comm.size()) {
    while (end && (static_cast<unsigned char>(comm[end]) & 0xc0) == 0x80) --end;
  }
  return comm.substr(0, end);
}

// A thread's name (prctl(PR_SET_NAME) sets it, misc.cc), or the process's.
std::string ThreadComm(DWORD tid) {
  using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static auto get = reinterpret_cast<GetThreadDescriptionFn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
  std::string comm;
  HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
  if (h != nullptr && GetProcessIdOfThread(h) == GetCurrentProcessId() && get != nullptr) {
    PWSTR description = nullptr;
    if (SUCCEEDED(get(h, &description)) && description != nullptr) {
      comm = Narrow(description);
      LocalFree(description);
    }
  }
  if (h != nullptr) CloseHandle(h);
  if (comm.empty()) comm = ProcessComm();
  size_t end = std::min<size_t>(comm.size(), 15);
  if (end < comm.size()) {
    while (end && (static_cast<unsigned char>(comm[end]) & 0xc0) == 0x80) --end;
  }
  return comm.substr(0, end);
}

unsigned long long Ticks(FILETIME t) {  // 100 ns units to USER_HZ (100) ticks
  return ((static_cast<unsigned long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime) / 100000;
}

// Clock ticks since boot of a creation time.
unsigned long long StartTicks(FILETIME created) {
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  unsigned long long now_ticks = Ticks(now);
  unsigned long long uptime_ticks = GetTickCount64() / 10;
  unsigned long long boot = now_ticks > uptime_ticks ? now_ticks - uptime_ticks : 0;
  unsigned long long start = Ticks(created);
  return start > boot ? start - boot : 0;
}

struct Memory {
  unsigned long long vsize = 0;  // reserved and committed address space
  unsigned long long rss = 0, peak_rss = 0, private_bytes = 0;
};

Memory ProcessMemory() {
  Memory m;
  PROCESS_MEMORY_COUNTERS_EX counters = {};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(),
                           reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
    m.rss = counters.WorkingSetSize;
    m.peak_rss = counters.PeakWorkingSetSize;
    m.private_bytes = counters.PrivateUsage;
  }
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  auto* address = static_cast<char*>(si.lpMinimumApplicationAddress);
  MEMORY_BASIC_INFORMATION info;
  while (address < si.lpMaximumApplicationAddress &&
         VirtualQuery(address, &info, sizeof(info)) == sizeof(info)) {
    if (info.State != MEM_FREE) m.vsize += info.RegionSize;
    address = static_cast<char*>(info.BaseAddress) + info.RegionSize;
  }
  return m;
}

std::string Cmdline() {
  // An app process: its name alone, as the zygote leaves argv on a device.
  const std::string argv0 = Argv0();
  if (!argv0.empty()) return argv0 + std::string(1, '\0');
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::string content;
  for (int i = 0; argv != nullptr && i < argc; ++i) {
    content += Narrow(argv[i]);
    content += '\0';
  }
  if (argv != nullptr) LocalFree(argv);
  return content;
}

std::string Environ() {
  std::string content;
  wchar_t* block = GetEnvironmentStringsW();
  for (const wchar_t* p = block; p != nullptr && *p != L'\0'; p += wcslen(p) + 1) {
    if (*p == L'=') continue;  // the hidden per-drive current directories
    content += Narrow(p);
    content += '\0';
  }
  if (block != nullptr) FreeEnvironmentStringsW(block);
  return content;
}

// Linux's single-letter states: R for the calling thread, S for others.
char ThreadState(DWORD tid) { return tid == GetCurrentThreadId() ? 'R' : 'S'; }

std::string Status(DWORD tid) {
  bool process = tid == 0;
  DWORD pid = GetCurrentProcessId();
  Memory m = ProcessMemory();
  std::vector<DWORD> tids = ThreadIds();
  uid_t uid = getuid();
  gid_t gid = getgid();
  DWORD cpus = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  unsigned long long mask = cpus >= 64 ? ~0ULL : (1ULL << cpus) - 1;
  ULONG_PTR low = 0, high = 0;
  GetCurrentThreadStackLimits(&low, &high);
  std::string s;
  s += "Name:\t" + (process ? ProcessComm() : ThreadComm(tid)) + "\n";
  s += "Umask:\t0022\n";
  s += Format("State:\t%c (%s)\n", process ? 'R' : ThreadState(tid),
              (process || ThreadState(tid) == 'R') ? "running" : "sleeping");
  s += Format("Tgid:\t%lu\nNgid:\t0\nPid:\t%lu\nPPid:\t%d\nTracerPid:\t0\n",
              static_cast<unsigned long>(pid), static_cast<unsigned long>(process ? pid : tid),
              static_cast<int>(getppid()));
  s += Format("Uid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\n", uid, uid, uid, uid, gid, gid, gid, gid);
  s += Format("FDSize:\t%llu\nGroups:\t\n",
              static_cast<unsigned long long>(std::max<size_t>(64, OpenFds().size())));
  s += Format("VmPeak:\t%8llu kB\nVmSize:\t%8llu kB\nVmLck:\t%8d kB\nVmPin:\t%8d kB\n", m.vsize / 1024,
              m.vsize / 1024, 0, 0);
  s += Format("VmHWM:\t%8llu kB\nVmRSS:\t%8llu kB\n", m.peak_rss / 1024, m.rss / 1024);
  s += Format("RssAnon:\t%8llu kB\nRssFile:\t%8d kB\nRssShmem:\t%8d kB\n",
              std::min(m.rss, m.private_bytes) / 1024, 0, 0);
  s += Format("VmData:\t%8llu kB\nVmStk:\t%8llu kB\nVmExe:\t%8d kB\nVmLib:\t%8d kB\n",
              m.private_bytes / 1024, static_cast<unsigned long long>(high - low) / 1024, 0, 0);
  s += Format("VmPTE:\t%8d kB\nVmSwap:\t%8d kB\n", 0, 0);
  s += Format("Threads:\t%llu\n", static_cast<unsigned long long>(tids.size()));
  s += "SigQ:\t0/63438\n";
  s += "SigPnd:\t0000000000000000\nShdPnd:\t0000000000000000\n";
  s += "SigBlk:\t0000000000000000\nSigIgn:\t0000000000000000\nSigCgt:\t0000000000000000\n";
  s += "CapInh:\t0000000000000000\nCapPrm:\t0000000000000000\nCapEff:\t0000000000000000\n";
  s += "CapBnd:\t0000000000000000\nCapAmb:\t0000000000000000\n";
  s += "NoNewPrivs:\t0\nSeccomp:\t0\n";
  s += Format("Cpus_allowed:\t%llx\nCpus_allowed_list:\t0-%lu\n", mask,
              static_cast<unsigned long>(cpus > 0 ? cpus - 1 : 0));
  s += "voluntary_ctxt_switches:\t0\nnonvoluntary_ctxt_switches:\t0\n";
  return s;
}

// /proc/<pid>/stat and /proc/<pid>/task/<tid>/stat (proc(5)'s 52 fields).
std::string Stat(DWORD tid) {
  bool process = tid == 0;
  DWORD pid = GetCurrentProcessId();
  FILETIME created = {}, exited, kernel = {}, user = {};
  if (process) {
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
  } else {
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (h != nullptr) {
      GetThreadTimes(h, &created, &exited, &kernel, &user);
      CloseHandle(h);
    }
  }
  Memory m = ProcessMemory();
  PROCESS_MEMORY_COUNTERS counters = {};
  counters.cb = sizeof(counters);
  GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters));
  int processor = tid == GetCurrentThreadId() ? static_cast<int>(GetCurrentProcessorNumber()) : 0;
  std::string comm = process ? ProcessComm() : ThreadComm(tid);
  return Format("%lu (%s) %c %d %lu %lu 0 -1 4194304 %lu 0 0 0 %llu %llu 0 0 20 0 %llu 0 %llu %llu "
                "%llu 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 %d 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
                static_cast<unsigned long>(process ? pid : tid), comm.c_str(),
                process ? 'R' : ThreadState(tid), static_cast<int>(getppid()),
                static_cast<unsigned long>(pid), static_cast<unsigned long>(pid),
                static_cast<unsigned long>(counters.PageFaultCount), Ticks(user), Ticks(kernel),
                static_cast<unsigned long long>(ThreadIds().size()), StartTicks(created), m.vsize, m.rss / 4096, processor);
}

}  // namespace
}  // namespace procfs
}  // namespace gesher

// AT_RANDOM's 16 bytes, filled once from the system RNG: /proc/self/auxv
// and the bionic shim's getauxval(AT_RANDOM) both point here.
extern "C" const unsigned char* gesher_auxv_random(void) {
  alignas(16) static unsigned char bytes[16];
  static const bool filled = [] {
    return BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
  }();
  (void)filled;
  return bytes;
}

extern "C" const char* gesher_auxv_platform(void) {
#if defined(__aarch64__)
  static const char kPlatform[] = "aarch64";
#else
  static const char kPlatform[] = "x86_64";
#endif
  return kPlatform;
}

extern "C" uint64_t gesher_auxv_hwcap(void) {
#if defined(__x86_64__)
  int regs[4];
  __cpuid(regs, 1);
  return static_cast<uint32_t>(regs[3]);
#elif defined(__aarch64__)
  // Windows ARM64 guarantees FP/ASIMD; optional instructions require OS evidence.
  uint64_t caps = 3;  // HWCAP_FP | HWCAP_ASIMD
  if (IsProcessorFeaturePresent(PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE)) caps |= 0x78;
  if (IsProcessorFeaturePresent(PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE)) caps |= 0x80;
  return caps;
#else
  return 0;
#endif
}

extern "C" uint64_t gesher_auxv_hwcap2(void) {
#if defined(__x86_64__)
  // Linux HWCAP2_FSGSBASE means the OS enables the instructions, not merely CPUID.
  return IsProcessorFeaturePresent(PF_RDWRFSGSBASE_AVAILABLE) ? 2 : 0;
#else
  return 0;
#endif
}

extern "C" const char* gesher_auxv_execfn(void) {
  static const std::string kExecfn = [] {
    wchar_t buf[MAX_PATH * 4];
    DWORD length = GetModuleFileNameW(nullptr, buf, MAX_PATH * 4);
    if (length == 0 || length >= MAX_PATH * 4) return std::string();
    return gesher::LinuxPath(buf);
  }();
  return kExecfn.c_str();
}

namespace gesher {
namespace procfs {
namespace {

// /proc/self/auxv: the auxiliary vector as the kernel gave it, 64-bit
// (type, value) pairs ending with AT_NULL; the values getauxval reports
// (compat/bionic/posix_bionic.cc). Code that reads the file instead of
// calling getauxval exists: UPX's unpacker takes AT_PAGESZ from it, and stops
// on an int3 if it cannot be opened.
std::string Auxv() {
  const unsigned char* random_bytes = gesher_auxv_random();
  const char* platform = gesher_auxv_platform();
  const char* execfn = gesher_auxv_execfn();
  const uint64_t pairs[][2] = {
      {6, 4096},                                              // AT_PAGESZ
      {17, 100},                                              // AT_CLKTCK
      {16, gesher_auxv_hwcap()},                              // AT_HWCAP
      {11, static_cast<uint64_t>(getuid())},                  // AT_UID
      {12, static_cast<uint64_t>(getuid())},                  // AT_EUID
      {13, static_cast<uint64_t>(getgid())},                  // AT_GID
      {14, static_cast<uint64_t>(getgid())},                  // AT_EGID
      {23, 0},                                                // AT_SECURE
      {25, reinterpret_cast<uint64_t>(random_bytes)},         // AT_RANDOM
      {26, gesher_auxv_hwcap2()},                             // AT_HWCAP2
      {15, reinterpret_cast<uint64_t>(platform)},             // AT_PLATFORM
      {31, reinterpret_cast<uint64_t>(execfn)},               // AT_EXECFN
      {0, 0},                                                 // AT_NULL
  };
  return std::string(reinterpret_cast<const char*>(pairs), sizeof(pairs));
}

std::string Statm() {
  Memory m = ProcessMemory();
  return Format("%llu %llu 0 0 0 %llu 0\n", m.vsize / 4096, m.rss / 4096, m.private_bytes / 4096);
}

// The path of the file mapped at `address`, as a Linux path; empty if none.
std::string MappedFile(void* address) {
  wchar_t device_path[MAX_PATH * 4];
  DWORD n = GetMappedFileNameW(GetCurrentProcess(), address, device_path, MAX_PATH * 4);
  if (n == 0) return "";
  std::wstring path(device_path, n);
  // \Device\HarddiskVolumeN\... to a drive letter.
  struct DriveMapping {
    std::wstring target;
    std::wstring drive;
  };
  static const std::vector<DriveMapping> drives = [] {
    std::vector<DriveMapping> result;
    wchar_t drive_strings[512];
    DWORD length = GetLogicalDriveStringsW(512, drive_strings);
    for (const wchar_t* d = drive_strings; length != 0 && *d != L'\0'; d += wcslen(d) + 1) {
      wchar_t drive[3] = {d[0], L':', L'\0'};
      wchar_t target[MAX_PATH];
      if (QueryDosDeviceW(drive, target, MAX_PATH) == 0) continue;
      result.push_back({target, drive});
    }
    return result;
  }();
  for (const auto& dm : drives) {
    size_t t = dm.target.size();
    if (path.size() > t && _wcsnicmp(path.c_str(), dm.target.c_str(), t) == 0 && path[t] == L'\\') {
      return LinuxPath((dm.drive + path.substr(t)).c_str());
    }
  }
  return Narrow(path.c_str());
}

// /proc/self/maps: one line per run of pages with the same state, as
// VirtualQuery reports them; reserved address space is "---p", as Linux shows
// a PROT_NONE mapping.
std::string Maps() {
  std::string s;
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  auto* address = static_cast<char*>(si.lpMinimumApplicationAddress);
  MEMORY_BASIC_INFORMATION info;
  ULONG_PTR stack_low = 0, stack_high = 0;
  GetCurrentThreadStackLimits(&stack_low, &stack_high);
  HANDLE default_heap = GetProcessHeap();
  MEMORY_BASIC_INFORMATION heap_info = {};
  if (default_heap != nullptr) {
    VirtualQuery(default_heap, &heap_info, sizeof(heap_info));
  }
  while (address < si.lpMaximumApplicationAddress &&
         VirtualQuery(address, &info, sizeof(info)) == sizeof(info)) {
    char* base = static_cast<char*>(info.BaseAddress);
    address = base + info.RegionSize;
    if (info.State == MEM_FREE) continue;
    char perms[5] = "---p";
    if (info.State == MEM_COMMIT && (info.Protect & PAGE_GUARD) == 0) {
      DWORD p = info.Protect & 0xff;
      bool r = p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
               p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
      bool w = p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE ||
               p == PAGE_EXECUTE_WRITECOPY;
      bool x = p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE ||
               p == PAGE_EXECUTE_WRITECOPY;
      perms[0] = r ? 'r' : '-';
      perms[1] = w ? 'w' : '-';
      perms[2] = x ? 'x' : '-';
    }
    if (info.Type == MEM_MAPPED && (info.Protect & 0xff) != PAGE_WRITECOPY) perms[3] = 's';
    std::string name;
    unsigned long long offset = 0;
    unsigned major = 0, minor = 0;
    unsigned long long inode = 0;
    if (info.Type == MEM_IMAGE || info.Type == MEM_MAPPED) {
      name = MappedFile(base);
      offset = static_cast<unsigned long long>(base - static_cast<char*>(info.AllocationBase));
      if (!name.empty() && name[0] == '/') {
        struct stat st;
        if (stat(name.c_str(), &st) == 0) {
          major = static_cast<unsigned>((st.st_dev >> 8) & 0xff);
          minor = static_cast<unsigned>(st.st_dev & 0xff);
          inode = static_cast<unsigned long long>(st.st_ino);
        }
      }
    } else if (reinterpret_cast<ULONG_PTR>(base) >= stack_low &&
               reinterpret_cast<ULONG_PTR>(base) < stack_high) {
      name = "[stack]";
    } else if (heap_info.AllocationBase != nullptr &&
               info.AllocationBase == heap_info.AllocationBase) {
      name = "[heap]";
    }
    s += Format("%08llx-%08llx %s %08llx %02x:%02x %llu", reinterpret_cast<unsigned long long>(base),
                reinterpret_cast<unsigned long long>(address), perms, offset, major, minor, inode);
    if (!name.empty()) s += "                          " + name;
    s += '\n';
  }
  return s;
}

std::string Meminfo() {
  MEMORYSTATUSEX status = {};
  status.dwLength = sizeof(status);
  GlobalMemoryStatusEx(&status);
  PERFORMANCE_INFORMATION perf = {};
  perf.cb = sizeof(perf);
  GetPerformanceInfo(&perf, sizeof(perf));
  unsigned long long cached = static_cast<unsigned long long>(perf.SystemCache) * perf.PageSize;
  unsigned long long swap_total =
      status.ullTotalPageFile > status.ullTotalPhys ? status.ullTotalPageFile - status.ullTotalPhys : 0;
  unsigned long long swap_free =
      status.ullAvailPageFile > status.ullAvailPhys ? status.ullAvailPageFile - status.ullAvailPhys : 0;
  if (swap_free > swap_total) swap_free = swap_total;
  auto line = [](const char* name, unsigned long long bytes) {
    return Format("%-16s%8llu kB\n", name, bytes / 1024);
  };
  std::string s;
  s += line("MemTotal:", status.ullTotalPhys);
  s += line("MemFree:", status.ullAvailPhys > cached ? status.ullAvailPhys - cached : 0);
  s += line("MemAvailable:", status.ullAvailPhys);
  s += line("Buffers:", 0);
  s += line("Cached:", cached);
  s += line("SwapCached:", 0);
  s += line("SwapTotal:", swap_total);
  s += line("SwapFree:", swap_free);
  s += line("CommitLimit:", status.ullTotalPageFile);
  s += line("Committed_AS:", status.ullTotalPageFile - status.ullAvailPageFile);
  // Linux LRU, slab and vmalloc counters have no faithful public Windows
  // equivalent here. Omit unavailable metrics rather than invent measurements.
  return s;
}

std::string Cpuinfo() {
#if defined(__x86_64__)
  int regs[4];
  __cpuid(regs, 0);
  int max_cpuid = regs[0];
  char vendor[13] = {};
  memcpy(vendor, &regs[1], 4);
  memcpy(vendor + 4, &regs[3], 4);
  memcpy(vendor + 8, &regs[2], 4);
  __cpuid(regs, 1);
  int eax = regs[0], ecx = regs[2], edx = regs[3];
  int family = (eax >> 8) & 0xf, model = (eax >> 4) & 0xf, stepping = eax & 0xf;
  if (family == 0xf) family += (eax >> 20) & 0xff;
  if (family == 0x6 || family >= 0xf) model += ((eax >> 16) & 0xf) << 4;
  char brand[49] = {};
  __cpuid(regs, 0x80000000);
  if (static_cast<unsigned>(regs[0]) >= 0x80000004) {
    for (int i = 0; i < 3; ++i) {
      __cpuid(regs, 0x80000002 + i);
      memcpy(brand + 16 * i, regs, 16);
    }
  }
  std::string model_name = brand;
  model_name.erase(0, model_name.find_first_not_of(' '));
  __cpuid(regs, 7);
  int ebx7 = regs[1];
  std::string flags = "fpu";
  struct Flag {
    int reg;  // 0: edx(1), 1: ecx(1), 2: ebx(7)
    int bit;
    const char* name;
  };
  static const Flag kFlags[] = {
      {0, 4, "tsc"},    {0, 8, "cx8"},      {0, 15, "cmov"},   {0, 23, "mmx"},
      {0, 24, "fxsr"},  {0, 25, "sse"},     {0, 26, "sse2"},   {0, 28, "ht"},
      {1, 0, "pni"},    {1, 1, "pclmulqdq"}, {1, 9, "ssse3"},  {1, 12, "fma"},
      {1, 13, "cx16"},  {1, 19, "sse4_1"},  {1, 20, "sse4_2"}, {1, 22, "movbe"},
      {1, 23, "popcnt"}, {1, 25, "aes"},    {1, 28, "avx"},    {1, 29, "f16c"},
      {1, 30, "rdrand"}, {2, 3, "bmi1"},    {2, 5, "avx2"},    {2, 8, "bmi2"},
      {2, 16, "avx512f"}, {2, 18, "rdseed"}, {2, 19, "adx"},   {2, 29, "sha_ni"},
  };
  for (const Flag& f : kFlags) {
    int reg = f.reg == 0 ? edx : f.reg == 1 ? ecx : ebx7;
    if ((reg >> f.bit) & 1) flags += std::string(" ") + f.name;
  }
  __cpuid(regs, 0x80000000);
  if (static_cast<unsigned>(regs[0]) >= 0x80000001) {
    __cpuid(regs, 0x80000001);
    int ecx_ext = regs[2], edx_ext = regs[3];
    if ((edx_ext >> 11) & 1) flags += " syscall";
    if ((edx_ext >> 20) & 1) flags += " nx";
    if ((edx_ext >> 29) & 1) flags += " lm";
    if ((ecx_ext >> 0) & 1) flags += " lahf_lm";
    if ((ecx_ext >> 5) & 1) flags += " abm";
  }
  DWORD cpus = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  std::string s;
  for (DWORD i = 0; i < cpus; ++i) {
    s += Format("processor\t: %lu\nvendor_id\t: %s\ncpu family\t: %d\nmodel\t\t: %d\n",
                static_cast<unsigned long>(i), vendor, family, model);
    s += "model name\t: " + model_name + "\n";
    s += Format("stepping\t: %d\nphysical id\t: 0\nsiblings\t: %lu\ncore id\t\t: %lu\n", stepping,
                static_cast<unsigned long>(cpus), static_cast<unsigned long>(i));
    s += Format("cpu cores\t: %lu\napicid\t\t: %lu\nfpu\t\t: yes\nfpu_exception\t: yes\n",
                static_cast<unsigned long>(cpus), static_cast<unsigned long>(i));
    s += Format("cpuid level\t: %d\nwp\t\t: yes\n", max_cpuid);
    s += "flags\t\t: " + flags + "\n";
    s += "\n";
  }
  return s;
#elif defined(__aarch64__)
  DWORD cpus = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  std::string s;
  for (DWORD i = 0; i < cpus; ++i) {
    s += Format("processor\t: %lu\n", static_cast<unsigned long>(i));
    const uint64_t caps = gesher_auxv_hwcap();
    std::string features = "fp asimd";
    if (caps & (1 << 3)) features += " aes";
    if (caps & (1 << 4)) features += " pmull";
    if (caps & (1 << 5)) features += " sha1";
    if (caps & (1 << 6)) features += " sha2";
    if (caps & (1 << 7)) features += " crc32";
    s += "Features\t: " + features + "\nCPU architecture: 8\n\n";
  }
  return s;
#else
  return "processor\t: 0\n\n";
#endif
}

std::string Uptime() {
  ULONGLONG ms = GetTickCount64();
  return Format("%llu.%02llu 0.00\n", ms / 1000, (ms % 1000) / 10);
}

// What a name is. `file` names a generated file's content, `tid` a task
// directory's thread, `fd` a /proc/self/fd link's fd.
struct Node {
  unsigned type = kNone;
  std::string file;  // "cmdline", "status", ... ; "timezone"
  DWORD tid = 0;
  int fd = -1;
  std::string link;  // "exe", "cwd", "fd"
  std::vector<procfs::Entry> entries;  // directories: filled by Lookup(.., true)
};

bool IsSelf(const std::string& part) {
  unsigned long pid;
  return part == "self" || (ParseNumber(part, &pid) && pid == GetCurrentProcessId());
}

void Add(std::vector<procfs::Entry>* entries, const std::string& name, unsigned char type) {
  entries->push_back({name, type});
}

Node Lookup(const std::wstring& name, bool list) {
  std::vector<std::string> p = Split(name);
  Node n;
  if (p.size() == 2 && p[0] == "etc" && p[1] == "timezone") {
    n.type = kFile;
    n.file = "timezone";
    return n;
  }
  if (p.size() == 5 && p[0] == "sys" && p[1] == "devices" && p[2] == "system" &&
      p[3] == "cpu" && (p[4] == "possible" || p[4] == "present" || p[4] == "online")) {
    n.type = kFile;
    n.file = "cpu_list";
    return n;
  }
  if (p.empty() || p[0] != "proc") return n;
  if (p.size() == 1) {
    n.type = kDir;
    if (list) {
      Add(&n.entries, "self", DT_LNK);
      Add(&n.entries, std::to_string(GetCurrentProcessId()), DT_DIR);
      for (const char* f : {"cpuinfo", "meminfo", "mounts", "uptime"}) Add(&n.entries, f, DT_REG);
    }
    return n;
  }
  if (p.size() == 2 && (p[1] == "meminfo" || p[1] == "cpuinfo" || p[1] == "uptime" ||
                        p[1] == "mounts")) {
    n.type = kFile;
    n.file = p[1];
    return n;
  }
  if (p.size() == 2 && p[1] == "self") {
    // A link to /proc/<pid>, which is a directory to everyone that follows it.
    n.type = kDir;
  } else if (!IsSelf(p[1])) {
    return n;
  }
  static const char* const kProcessFiles[] = {"auxv", "cmdline", "comm",  "environ", "maps",
                                              "mounts", "stat",   "statm", "status"};
  if (p.size() == 2) {
    n.type = kDir;
    if (list) {
      for (const char* f : kProcessFiles) Add(&n.entries, f, DT_REG);
      Add(&n.entries, "fd", DT_DIR);
      Add(&n.entries, "task", DT_DIR);
      Add(&n.entries, "exe", DT_LNK);
      Add(&n.entries, "cwd", DT_LNK);
    }
    return n;
  }
  if (p.size() == 3) {
    for (const char* f : kProcessFiles) {
      if (p[2] == f) {
        n.type = kFile;
        n.file = f;
        return n;
      }
    }
    if (p[2] == "exe" || p[2] == "cwd") {
      n.type = kLink;
      n.link = p[2];
      return n;
    }
    if (p[2] == "fd") {
      n.type = kDir;
      if (list) {
        for (int fd : OpenFds()) Add(&n.entries, std::to_string(fd), DT_LNK);
      }
      return n;
    }
    if (p[2] == "task") {
      n.type = kDir;
      if (list) {
        for (DWORD tid : ThreadIds()) Add(&n.entries, std::to_string(tid), DT_DIR);
      }
      return n;
    }
    return n;
  }
  unsigned long number;
  if (p[2] == "fd" && p.size() == 4 && ParseNumber(p[3], &number)) {
    if (_get_osfhandle(static_cast<int>(number)) == -1) return n;
    n.type = kLink;
    n.link = "fd";
    n.fd = static_cast<int>(number);
    return n;
  }
  if (p[2] == "task" && ParseNumber(p[3], &number) && IsOwnThread(number)) {
    n.tid = static_cast<DWORD>(number);
    if (p.size() == 4) {
      n.type = kDir;
      if (list) {
        for (const char* f : {"comm", "stat", "status"}) Add(&n.entries, f, DT_REG);
      }
      return n;
    }
    if (p.size() == 5 && (p[4] == "comm" || p[4] == "stat" || p[4] == "status")) {
      n.type = kFile;
      n.file = p[4];
    }
    return n;
  }
  return n;
}

std::string Content(const Node& n) {
  const std::string& f = n.file;
  if (f == "timezone") return WindowsZoneId();  // no newline: libcore reads it whole
  if (f == "cmdline") return Cmdline();
  if (f == "environ") return Environ();
  if (f == "comm") return (n.tid != 0 ? ThreadComm(n.tid) : ProcessComm()) + "\n";
  if (f == "status") return Status(n.tid);
  if (f == "stat") return Stat(n.tid);
  if (f == "statm") return Statm();
  if (f == "auxv") return Auxv();
  if (f == "maps") return Maps();
  if (f == "meminfo") return Meminfo();
  if (f == "cpuinfo") return Cpuinfo();
  if (f == "cpu_list") {
    // /sys/devices/system/cpu/{possible,present,online}: "0-<n-1>".
    const DWORD cpus = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return cpus > 1 ? Format("0-%lu\n", cpus - 1) : std::string("0\n");
  }
  if (f == "uptime") return Uptime();
  return "";  // mounts: no mount table (misc.cc's setmntent agrees)
}

// The file open as fd: its path, or Linux's pipe:[n] / socket:[n] names.
bool FdTarget(int fd, std::string* target) {
  HANDLE h = HandleFromFd(fd);
  if (h == INVALID_HANDLE_VALUE) return errno = ENOENT, false;
  switch (GetFileType(h)) {
    case FILE_TYPE_DISK: {
      wchar_t buf[32768];
      DWORD n = GetFinalPathNameByHandleW(h, buf, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
      if (n == 0) return FailWithLastError(), false;
      if (n >= 32768) return errno = ENAMETOOLONG, false;
      *target = LinuxPath(buf);
      return true;
    }
    case FILE_TYPE_PIPE:
      *target = Format("pipe:[%llu]", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h)));
      return true;
    default:
      // Sockets, eventfds and epolls are fds on NUL; /dev/null itself too.
      *target = "/dev/null";
      return true;
  }
}

}  // namespace

unsigned Type(const std::wstring& name) { return Lookup(name, false).type; }

int FdOfLink(const std::wstring& name) {
  Node n = Lookup(name, false);
  return n.link == "fd" ? n.fd : -1;
}

bool ReadLink(const std::wstring& name, std::string* target) {
  Node n = Lookup(name, false);
  if (n.type == kNone) return errno = ENOENT, false;
  if (n.type != kLink) {
    // /proc/self is a link on Linux: to /proc/<pid>.
    std::vector<std::string> p = Split(name);
    if (p.size() == 2 && p[1] == "self") {
      *target = std::to_string(GetCurrentProcessId());
      return true;
    }
    return errno = EINVAL, false;
  }
  if (n.link == "fd") {
    auto snapshot = CaptureDescriptor(n.fd);
    if (!snapshot) return false;
    if (FileDescriptionIsRevocable(snapshot)) {
      // A revocable capability is not permission to open its private backing
      // pathname as an unrestricted file. Opening this proc link is handled
      // below and retains the same revocation group.
      *target = "anon_inode:[gesher-revocable]";
      return true;
    }
    return FdTarget(n.fd, target);
  }
  wchar_t buf[32768];
  DWORD length = n.link == "exe" ? GetModuleFileNameW(nullptr, buf, 32768)
                                 : GetCurrentDirectoryW(32768, buf);
  if (length == 0 || length >= 32768) return FailWithLastError(), false;
  *target = LinuxPath(buf);
  return true;
}

bool List(const std::wstring& name, std::vector<Entry>* entries) {
  Node n = Lookup(name, true);
  if (n.type == kNone) return errno = ENOENT, false;
  if (n.type != kDir) return errno = ENOTDIR, false;
  entries->clear();
  entries->push_back({".", DT_DIR});
  entries->push_back({"..", DT_DIR});
  entries->insert(entries->end(), n.entries.begin(), n.entries.end());
  return true;
}

int Open(const std::wstring& name, int flags) {
  if ((flags & O_PATH) != 0) flags &= O_PATH | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW;
  Node n = Lookup(name, false);
  if (n.type == kNone) return errno = (flags & O_CREAT) != 0 ? EACCES : ENOENT, -1;
  if (n.type == kLink) {
    if ((flags & O_NOFOLLOW) != 0 && (flags & O_PATH) == 0) return errno = ELOOP, -1;
    // Opening the link opens its file again (pipes and sockets: the same one).
    if (n.link == "fd") {
      auto snapshot = CaptureDescriptor(n.fd);
      if (!snapshot) return -1;
      if (FileDescriptionIsRevocable(snapshot)) return ReopenRevocableFileDescription(snapshot, flags);
      HANDLE h = HandleFromFd(n.fd);
      if (h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_DISK) {
        return fcntl(n.fd, F_DUPFD_CLOEXEC, 0);
      }
    }
    std::string target;
    if (!ReadLink(name, &target)) return -1;
    return open(target.c_str(), flags & ~(O_CREAT | O_EXCL), 0);
  }
  if (n.type == kDir) return errno = (flags & O_ACCMODE) != O_RDONLY ? EISDIR : EOPNOTSUPP, -1;
  if ((flags & O_DIRECTORY) != 0) return errno = ENOTDIR, -1;
  if ((flags & O_DIRECT) != 0) return errno = EINVAL, -1;
  if ((flags & O_NOATIME) != 0) return errno = EOPNOTSUPP, -1;
  if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_TRUNC | O_CREAT)) != 0) {
    return errno = EACCES, -1;
  }
  std::string content = Content(n);
  record_locks::DescriptorGuard descriptors;
  int fd = memfd_create("procfs", (flags & O_CLOEXEC) != 0 ? MFD_CLOEXEC : 0);
  if (fd < 0) return -1;
  if (_write(fd, content.data(), static_cast<unsigned>(content.size())) !=
          static_cast<int>(content.size()) ||
      _lseeki64(fd, 0, SEEK_SET) != 0) {
    int saved = gesher_errno_from_crt(errno);
    close(fd);
    return errno = saved, -1;
  }
  // The content builder needs write access; the caller does not. Reduce the
  // actual kernel handle's rights before publishing the descriptor, so both
  // POSIX I/O and direct/native consumers see the requested read-only/O_PATH
  // access. A metadata-only O_RDONLY would still permit writes through mmap.
  HANDLE readonly = nullptr;
  DWORD access = (flags & O_PATH) != 0 ? FILE_READ_ATTRIBUTES | SYNCHRONIZE : FILE_GENERIC_READ;
  BOOL inherit = (flags & O_CLOEXEC) == 0;
  if (!DuplicateHandle(GetCurrentProcess(), HandleFromFd(fd), GetCurrentProcess(),
                         &readonly, access, inherit, 0)) {
    int saved = ErrnoFromWin32(GetLastError());
    close(fd);
    return errno = saved, -1;
  }
  if (close(fd) < 0) {
    int saved = errno;
    CloseHandle(readonly);
    return errno = saved, -1;
  }
  fd = _open_osfhandle(reinterpret_cast<intptr_t>(readonly), _O_RDONLY | _O_BINARY |
                      ((flags & O_CLOEXEC) != 0 ? _O_NOINHERIT : 0));
  if (fd < 0) {
    int saved = gesher_errno_from_crt(errno);
    CloseHandle(readonly);
    return errno = saved, -1;
  }
  if (RegisterOfd(fd, flags | ((flags & O_PATH) == 0 ? O_LARGEFILE : 0)) < 0) {
    int saved = errno;
    close(fd);
    return errno = saved, -1;
  }
  return fd;
}

}  // namespace procfs
}  // namespace gesher

extern "C" void gesher_set_process_argv0(const char* name) {
  std::lock_guard<std::mutex> lock(gesher::procfs::g_argv0_mutex);
  gesher::procfs::g_argv0 = name != nullptr ? name : "";
}
