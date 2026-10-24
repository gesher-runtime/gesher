// Gesher: a watch on a window's thread. See thread_watch.h.

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

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
            Dump(waited);
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
  void Dump(ULONGLONG waited) {
    DWORD64 pcs[64];
    int n = 0;
    const uint32_t msg = current_msg.load();
    if (SuspendThread(thread_handle_) != static_cast<DWORD>(-1)) {
      CONTEXT context;
      memset(&context, 0, sizeof(context));
      context.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(thread_handle_, &context)) {
        while (n < 64 && context.Rip != 0) {
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
      ResumeThread(thread_handle_);
    }
    fprintf(stderr,
            "gesher-ime: the thread of window \"%s\" has not answered for %llu ms "
            "(in message 0x%x of its procedure; 0: outside it); its stack:%s",
            name_.c_str(), static_cast<unsigned long long>(waited), msg, kNewline);
    for (int i = 0; i < n; i++) {
      fprintf(stderr, "gesher-ime:   #%02d %s%s", i, Describe(pcs[i]).c_str(), kNewline);
    }
    fflush(stderr);
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
