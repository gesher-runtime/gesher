// Gesher: a watch on a window's thread. See thread_watch.h.

#include <windows.h>
#include <tlhelp32.h>
#include <wct.h>

#include <stdio.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "thread_watch.h"

namespace gesher {
namespace input {

namespace {

const char kNewline[] = {10, 0};

UINT PingMessage() {
  static const UINT message = RegisterWindowMessageW(L"GesherThreadWatchPing");
  return message;
}

// The export of `module` at or before `rva` (nullptr when none).
const char* NearestExport(HMODULE module, DWORD rva, DWORD* offset) {
  const BYTE* base = reinterpret_cast<const BYTE*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (dir.VirtualAddress == 0 || dir.Size == 0) return nullptr;
  const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
  const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
  const auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
  const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
  const char* best = nullptr;
  DWORD best_rva = 0;
  for (DWORD i = 0; i < exports->NumberOfNames; i++) {
    if (ordinals[i] >= exports->NumberOfFunctions) continue;
    const DWORD f = functions[ordinals[i]];
    // A forwarder's "rva" is in the export directory itself: not code.
    if (f >= dir.VirtualAddress && f < dir.VirtualAddress + dir.Size) continue;
    if (f <= rva && (best == nullptr || f > best_rva)) {
      best_rva = f;
      best = reinterpret_cast<const char*>(base + names[i]);
    }
  }
  if (best != nullptr) *offset = rva - best_rva;
  return best;
}

std::string Describe(DWORD64 pc) {
  char line[512];
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(pc)), &module) ||
      module == nullptr) {
    snprintf(line, sizeof(line), "0x%llx", static_cast<unsigned long long>(pc));
    return line;
  }
  char path[MAX_PATH] = {0};
  GetModuleFileNameA(module, path, MAX_PATH);
  const char* name = path;
  for (const char* p = path; *p != 0; p++) {
    if (*p == 47 || *p == 92) name = p + 1;  // '/' or the backslash
  }
  const DWORD rva = static_cast<DWORD>(pc - reinterpret_cast<uintptr_t>(module));
  DWORD offset = 0;
  const char* symbol = NearestExport(module, rva, &offset);
  if (symbol != nullptr && offset < 0x10000) {
    snprintf(line, sizeof(line), "%s!%s+0x%lx (%s+0x%lx)", name, symbol,
             static_cast<unsigned long>(offset), name, static_cast<unsigned long>(rva));
  } else {
    snprintf(line, sizeof(line), "%s+0x%lx", name, static_cast<unsigned long>(rva));
  }
  return line;
}


// Unwinds `thread` (suspended meanwhile) into `pcs`; the frame count.
int Unwind(HANDLE thread, DWORD64* pcs, int max) {
  int n = 0;
  if (SuspendThread(thread) == static_cast<DWORD>(-1)) return 0;
  CONTEXT context;
  memset(&context, 0, sizeof(context));
  context.ContextFlags = CONTEXT_FULL;
  if (GetThreadContext(thread, &context)) {
    while (n < max && context.Rip != 0) {
      pcs[n++] = context.Rip;
      DWORD64 image_base = 0;
      PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
      if (function == nullptr) {
        // A leaf (or code without unwind data): the return address is on top.
        if (context.Rsp == 0) break;
        context.Rip = *reinterpret_cast<const DWORD64*>(static_cast<uintptr_t>(context.Rsp));
        context.Rsp += 8;
      } else {
        void* handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                         &handler_data, &establisher, nullptr);
      }
    }
  }
  ResumeThread(thread);
  return n;
}

// The image name of process `pid` ("?" when it cannot be opened).
std::string ProcessName(DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (process == nullptr) return "?";
  wchar_t path[MAX_PATH] = {0};
  DWORD size = MAX_PATH;
  std::string out = "?";
  if (QueryFullProcessImageNameW(process, 0, path, &size)) {
    char narrow[MAX_PATH * 3] = {0};
    WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof(narrow), nullptr, nullptr);
    out = narrow;
  }
  CloseHandle(process);
  return out;
}

// WCT_OUT_PROC_FLAG | WCT_OUT_PROC_COM_FLAG | WCT_OUT_PROC_CS_FLAG (not in
// every wct.h).
const DWORD kWctFlags = 0x1 | 0x2 | 0x4;

// What `tid` waits for, across processes (Wait Chain Traversal: ALPC, COM,
// locks, SendMessage), one line.
void DumpWaitChain(DWORD tid, std::vector<DWORD>* foreign, const char* prefix) {
  using OpenFn = HWCT(WINAPI*)(DWORD, PWAITCHAINCALLBACK);
  using CloseFn = VOID(WINAPI*)(HWCT);
  using GetFn = BOOL(WINAPI*)(HWCT, DWORD_PTR, DWORD, DWORD, LPDWORD, PWAITCHAIN_NODE_INFO,
                              LPBOOL);
  using RegisterFn = VOID(WINAPI*)(PCOGETCALLSTATE, PCOGETACTIVATIONSTATE);
  HMODULE advapi = LoadLibraryW(L"advapi32.dll");
  if (advapi == nullptr) return;
  auto open = reinterpret_cast<OpenFn>(
      reinterpret_cast<void*>(GetProcAddress(advapi, "OpenThreadWaitChainSession")));
  auto close = reinterpret_cast<CloseFn>(
      reinterpret_cast<void*>(GetProcAddress(advapi, "CloseThreadWaitChainSession")));
  auto get = reinterpret_cast<GetFn>(
      reinterpret_cast<void*>(GetProcAddress(advapi, "GetThreadWaitChain")));
  auto reg = reinterpret_cast<RegisterFn>(
      reinterpret_cast<void*>(GetProcAddress(advapi, "RegisterWaitChainCOMCallback")));
  if (open == nullptr || close == nullptr || get == nullptr) return;
  static bool registered = false;
  if (!registered && reg != nullptr) {
    registered = true;
    if (HMODULE ole = LoadLibraryW(L"ole32.dll")) {
      auto call_state = reinterpret_cast<PCOGETCALLSTATE>(
          reinterpret_cast<void*>(GetProcAddress(ole, "CoGetCallState")));
      auto activation_state = reinterpret_cast<PCOGETACTIVATIONSTATE>(
          reinterpret_cast<void*>(GetProcAddress(ole, "CoGetActivationState")));
      if (call_state != nullptr && activation_state != nullptr) reg(call_state, activation_state);
    }
  }
  HWCT session = open(0, nullptr);
  if (session == nullptr) return;
  WAITCHAIN_NODE_INFO nodes[16];
  DWORD count = 16;
  BOOL cycle = FALSE;
  std::string line = prefix;
  if (get(session, 0, kWctFlags, tid,
          &count, nodes, &cycle)) {
    static const char* const kTypes[] = {"?", "critical section", "SendMessage", "mutex",
                                         "ALPC", "COM", "thread wait", "process wait",
                                         "thread", "COM activation", "unknown"};
    for (DWORD i = 0; i < count && i < 16; i++) {
      const WAITCHAIN_NODE_INFO& node = nodes[i];
      char part[512];
      const int type = static_cast<int>(node.ObjectType);
      const char* type_name = type >= 1 && type <= 10 ? kTypes[type] : "?";
      if (node.ObjectType == WctThreadType) {
        const DWORD pid = node.ThreadObject.ProcessId;
        if (foreign != nullptr && pid != GetCurrentProcessId() && pid != 0) {
          bool seen = false;
          for (DWORD f : *foreign) seen = seen || f == pid;
          if (!seen) foreign->push_back(pid);
        }
        snprintf(part, sizeof(part), " -> thread %lu of process %lu (%s, status %d, waiting %lu ms)",
                 static_cast<unsigned long>(node.ThreadObject.ThreadId),
                 static_cast<unsigned long>(node.ThreadObject.ProcessId),
                 ProcessName(node.ThreadObject.ProcessId).c_str(),
                 static_cast<int>(node.ObjectStatus),
                 static_cast<unsigned long>(node.ThreadObject.WaitTime));
      } else {
        char name[WCT_OBJNAME_LENGTH * 3] = {0};
        WideCharToMultiByte(CP_UTF8, 0, node.LockObject.ObjectName, -1, name, sizeof(name),
                            nullptr, nullptr);
        snprintf(part, sizeof(part), " -> %s '%s' (status %d)", type_name, name,
                 static_cast<int>(node.ObjectStatus));
      }
      line += part;
    }
    if (cycle) line += " (a cycle)";
  } else {
    char part[64];
    snprintf(part, sizeof(part), " GetThreadWaitChain failed (%lu)",
             static_cast<unsigned long>(GetLastError()));
    line += part;
  }
  close(session);
  fprintf(stderr, "%s%s", line.c_str(), kNewline);
}

// The wait chains of process `pid`'s threads that wait (another process the
// IME's threads wait for, such as ImeBroker.exe).
void DumpProcessChains(DWORD pid) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 entry;
  entry.dwSize = sizeof(entry);
  fprintf(stderr, "gesher-ime:  process %lu (%s), its threads:%s", static_cast<unsigned long>(pid),
          ProcessName(pid).c_str(), kNewline);
  int shown = 0;
  for (BOOL ok = Thread32First(snapshot, &entry); ok && shown < 40;
       ok = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != pid) continue;
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "gesher-ime:   waits (thread %lu):",
             static_cast<unsigned long>(entry.th32ThreadID));
    DumpWaitChain(entry.th32ThreadID, nullptr, prefix);
    shown++;
  }
  CloseHandle(snapshot);
  fflush(stderr);
}

// The process's other threads that are in the IME or the text services
// (what the window's thread may be waiting for).
void DumpImeThreads(DWORD skip, std::vector<DWORD>* foreign) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 entry;
  entry.dwSize = sizeof(entry);
  const DWORD pid = GetCurrentProcessId();
  const DWORD self = GetCurrentThreadId();
  for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self ||
        entry.th32ThreadID == skip) {
      continue;
    }
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                   THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
    if (thread == nullptr) continue;
    DWORD64 pcs[48];
    const int n = Unwind(thread, pcs, 48);
    CloseHandle(thread);
    std::string frames[48];
    bool ime = false;
    for (int i = 0; i < n; i++) {
      frames[i] = Describe(pcs[i]);
      const std::string& f = frames[i];
      if (f.find("imj") != std::string::npos || f.find("MSCTF") != std::string::npos ||
          f.find("msctf") != std::string::npos || f.find("imetip") != std::string::npos ||
          f.find("mscand") != std::string::npos || f.find("IMM32") != std::string::npos) {
        ime = true;
      }
    }
    if (!ime) continue;
    fprintf(stderr, "gesher-ime:  thread %lu:%s", static_cast<unsigned long>(entry.th32ThreadID),
            kNewline);
    for (int i = 0; i < n; i++) {
      fprintf(stderr, "gesher-ime:   #%02d %s%s", i, frames[i].c_str(), kNewline);
    }
    DumpWaitChain(entry.th32ThreadID, foreign, "gesher-ime:   waits:");
  }
  CloseHandle(snapshot);
  fflush(stderr);
}

}  // namespace

class ThreadWatch {
 public:
  ThreadWatch(HWND hwnd, HANDLE thread, const char* name)
      : hwnd_(hwnd), thread_handle_(thread), name_(name != nullptr ? name : "window") {
    worker_ = std::thread([this] { Run(); });
  }

  ~ThreadWatch() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    CloseHandle(thread_handle_);
  }

  void Answered() { answered_.store(true); }
  std::atomic<uint32_t> current_msg{0};

 private:
  void Run() {
    bool outstanding = false;
    ULONGLONG posted_at = 0;
    int dumps = 0;
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_) {
      cv_.wait_for(lock, std::chrono::seconds(1));
      if (stop_) break;
      const ULONGLONG now = GetTickCount64();
      if (outstanding) {
        if (answered_.exchange(false)) {
          const ULONGLONG waited = now - posted_at;
          if (waited >= 4000) {
            fprintf(stderr, "gesher-ime: the thread of window \"%s\" answered after %llu ms%s",
                    name_.c_str(), static_cast<unsigned long long>(waited), kNewline);
            fflush(stderr);
          }
          outstanding = false;
          dumps = 0;
        } else {
          const ULONGLONG waited = now - posted_at;
          if (dumps < 4 && waited >= 5000 + static_cast<ULONGLONG>(dumps) * 60000) {
            Dump(waited, dumps == 0);
            dumps++;
          }
        }
      }
      if (!outstanding) {
        if (!IsWindow(hwnd_)) break;
        answered_.store(false);
        PostMessageW(hwnd_, PingMessage(), 0, 0);
        posted_at = now;
        outstanding = true;
      }
    }
  }

  // The window thread's stack: suspended while its frames are unwound
  // (nothing that takes a lock the thread may hold), described after.
  void Dump(ULONGLONG waited, bool first) {
    DWORD64 pcs[64];
    const uint32_t msg = current_msg.load();
    const int n = Unwind(thread_handle_, pcs, 64);
    fprintf(stderr,
            "gesher-ime: the thread of window \"%s\" has not answered for %llu ms "
            "(in message 0x%x of its procedure; 0: outside it); its stack:%s",
            name_.c_str(), static_cast<unsigned long long>(waited), msg, kNewline);
    for (int i = 0; i < n; i++) {
      fprintf(stderr, "gesher-ime:   #%02d %s%s", i, Describe(pcs[i]).c_str(), kNewline);
    }
    fflush(stderr);
    std::vector<DWORD> foreign;
    DumpWaitChain(GetThreadId(thread_handle_), &foreign, "gesher-ime:   waits:");
    if (first) DumpImeThreads(GetThreadId(thread_handle_), &foreign);
    if (first) {
      for (DWORD pid : foreign) DumpProcessChains(pid);
    }
  }

  HWND hwnd_;
  HANDLE thread_handle_;
  std::string name_;
  std::atomic<bool> answered_{false};
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::thread worker_;
};

ThreadWatch* StartThreadWatch(void* hwnd_ptr, const char* name) {
  HWND hwnd = static_cast<HWND>(hwnd_ptr);
  const DWORD tid = GetWindowThreadProcessId(hwnd, nullptr);
  if (tid == 0 || tid == GetCurrentThreadId()) return nullptr;
  HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                 THREAD_QUERY_INFORMATION, FALSE, tid);
  if (thread == nullptr) return nullptr;
  return new ThreadWatch(hwnd, thread, name);
}

void StopThreadWatch(ThreadWatch* watch) { delete watch; }

bool ThreadWatchMessage(ThreadWatch* watch, uint32_t msg) {
  if (watch == nullptr || msg != PingMessage()) return false;
  watch->Answered();
  return true;
}

void ThreadWatchEnter(ThreadWatch* watch, uint32_t msg, uint32_t* saved) {
  if (watch == nullptr) return;
  *saved = watch->current_msg.exchange(msg);
}

void ThreadWatchLeave(ThreadWatch* watch, uint32_t saved) {
  if (watch != nullptr) watch->current_msg.store(saved);
}

}  // namespace input
}  // namespace gesher
