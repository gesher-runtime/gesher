// HOST fixture: calibrate Windows pool workers and compare all other exact thread identities.
#pragma once

#include <windows.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace gesher::test {
namespace owner_thread_diagnostics {
struct Snapshot {
  DWORD tids[256]{};
  unsigned captured = 0, observed = 0;
  DWORD error = 0, close_error = 0;
};

Snapshot Capture() {
  Snapshot result;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) { result.error = GetLastError(); return result; }
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  if (Thread32First(snapshot, &entry)) {
    do {
      if (entry.th32OwnerProcessID == GetCurrentProcessId()) {
        ++result.observed;
        if (result.captured < 256) result.tids[result.captured++] = entry.th32ThreadID;
      }
      entry.dwSize = sizeof(entry);
    } while (Thread32Next(snapshot, &entry));
    const DWORD error = GetLastError();
    if (error != ERROR_NO_MORE_FILES) result.error = error;
  } else result.error = GetLastError();
  if (!CloseHandle(snapshot)) result.close_error = GetLastError();
  return result;
}

struct SavedErrors {
  DWORD windows = GetLastError();
  int c = errno;
  ~SavedErrors() { errno = c; SetLastError(windows); }
};

using Query = NTSTATUS (NTAPI*)(HANDLE, THREADINFOCLASS, PVOID, ULONG, PULONG);
Query ResolveStartQuery() {
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  FARPROC symbol = ntdll ? GetProcAddress(ntdll, "NtQueryInformationThread") : nullptr;
  Query query = nullptr;
  static_assert(sizeof(query) == sizeof(symbol));
  std::memcpy(&query, &symbol, sizeof(query));
  return query;
}

void* StartAddress(Query query, HANDLE thread) {
  void* start = nullptr;
  ULONG bytes = 0;
  return query && query(thread, static_cast<THREADINFOCLASS>(9), &start, sizeof(start), &bytes) >= 0
      && bytes == sizeof(start) ? start : nullptr;
}

struct Identity {
  DWORD tid = 0;
  uint64_t birth = 0;
  uintptr_t start = 0;
  bool operator==(const Identity&) const = default;
};
struct QualifiedSnapshot {
  std::array<Identity, 256> product{};
  size_t count = 0;
  size_t pool = 0;
};

Query start_query = nullptr;
void* pool_start = nullptr;

bool CaptureQualified(QualifiedSnapshot* result) {
  SavedErrors saved;
  Snapshot snapshot = Capture();
  if (!pool_start || snapshot.error || snapshot.close_error || snapshot.captured != snapshot.observed) return false;
  *result = {};
  for (unsigned i = 0; i < snapshot.captured; ++i) {
    DWORD tid = snapshot.tids[i];
    HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION | SYNCHRONIZE, FALSE, tid);
    if (!thread) return false;
    const bool own = GetProcessIdOfThread(thread) == GetCurrentProcessId();
    FILETIME birth{}, exit{}, kernel{}, user{};
    void* start = own ? StartAddress(start_query, thread) : nullptr;
    const bool stable = start && GetThreadTimes(thread, &birth, &exit, &kernel, &user) &&
        WaitForSingleObject(thread, 0) == WAIT_TIMEOUT;
    if (!CloseHandle(thread) || !stable) return false;
    if (start == pool_start) {
      ++result->pool;
    } else {
      result->product[result->count++] = {tid,
          (uint64_t{birth.dwHighDateTime} << 32) | birth.dwLowDateTime,
          reinterpret_cast<uintptr_t>(start)};
    }
  }
  std::sort(result->product.begin(), result->product.begin() + result->count,
            [](const Identity& a, const Identity& b) { return a.tid < b.tid; });
  return true;
}

void CALLBACK ReadPoolStart(PTP_CALLBACK_INSTANCE, void* result, PTP_WORK) {
  *static_cast<void**>(result) = StartAddress(start_query, GetCurrentThread());
}

DWORD WINAPI WaitInFixtureThread(void* release) {
  return WaitForSingleObject(static_cast<HANDLE>(release), 5000);
}
}  // namespace owner_thread_diagnostics

bool CalibrateOwnerThreads() {
  using namespace owner_thread_diagnostics;
  SavedErrors saved;
  start_query = ResolveStartQuery();
  if (!start_query) return false;
  PTP_WORK work = CreateThreadpoolWork(ReadPoolStart, &pool_start, nullptr);
  if (!work) return false;
  SubmitThreadpoolWork(work);
  WaitForThreadpoolWorkCallbacks(work, FALSE);
  CloseThreadpoolWork(work);
  HMODULE module = nullptr;
  if (!pool_start ||
      !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(pool_start), &module) ||
      module != GetModuleHandleW(L"ntdll.dll")) return false;

  QualifiedSnapshot baseline, leaked, retired;
  if (!CaptureQualified(&baseline) || baseline.pool == 0) return false;
  HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!release) return false;
  DWORD tid = 0;
  HANDLE thread = CreateThread(nullptr, 0, WaitInFixtureThread, release, 0, &tid);
  const bool captured = thread && CaptureQualified(&leaked);
  const bool signaled = SetEvent(release);
  const bool exited = thread && WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0;
  const bool thread_closed = thread && CloseHandle(thread);
  const bool event_closed = CloseHandle(release);
  if (!captured || !signaled || !exited || !thread_closed || !event_closed ||
      !CaptureQualified(&retired)) return false;
  auto entry = std::find_if(leaked.product.begin(), leaked.product.begin() + leaked.count,
                           [tid](const Identity& item) { return item.tid == tid; });
  if (leaked.count != baseline.count + 1 || entry == leaked.product.begin() + leaked.count ||
      entry->start != reinterpret_cast<uintptr_t>(&WaitInFixtureThread) ||
      retired.product != baseline.product || leaked.product == baseline.product) return false;
  std::printf("TCP_PRIORITY_OWNER_CLASSIFIER default_pool=1 ordinary_thread_rejected=1 exact_retirement=1\n");
  return true;
}

bool QualifiedOwnerThreadsMatch(int mode) {
  using namespace owner_thread_diagnostics;
  static QualifiedSnapshot baseline;
  QualifiedSnapshot current;
  if (!CaptureQualified(&current)) return false;
  if (mode == 0) baseline = current;
  const bool matches = current.product == baseline.product;
  std::printf("TCP_PRIORITY_OWNER_IDENTITIES mode=%d entries=%zu/%zu pool=%zu/%zu matched=%d\n",
              mode, current.count, baseline.count, current.pool, baseline.pool, matches);
  return matches;
}

// Keep only bounded numeric observations at the first census, never thread handles.
// Subsequent successful modes do no work. A separate failure snapshot provides
// start-module evidence alongside the calibrated comparison.
void ObserveOwnerThreads(int mode, bool failed) {
  using namespace owner_thread_diagnostics;
  static Snapshot baseline;
  if (mode != 0 && !failed) return;
  SavedErrors saved;
  if (mode == 0 && !failed) { baseline = Capture(); return; }
  const Snapshot current = Capture();
  std::fprintf(stderr, "TCP_PRIORITY_THREAD_DIAGNOSTIC mode=%d snapshots=post_census "
      "baseline=%u/%u baseline_error=%lu baseline_close_error=%lu "
      "current=%u/%u current_error=%lu current_close_error=%lu calibrated_pool=1\n",
      mode, baseline.captured, baseline.observed, baseline.error, baseline.close_error,
      current.captured, current.observed, current.error, current.close_error);
  for (unsigned i = 0; i < baseline.captured; ++i)
    std::fprintf(stderr, "TCP_PRIORITY_THREAD_BASELINE mode=%d tid=%lu numeric_observation_only=1\n",
                 mode, baseline.tids[i]);

  // Resolve the optional documented native query at runtime. A missing query,
  // vanished thread or unknown module is diagnostic data, never a reason to pass.
  Query query = ResolveStartQuery();
  unsigned opened = 0, closed = 0, modules = 0, released = 0;
  for (unsigned i = 0; i < current.captured; ++i) {
    const DWORD tid = current.tids[i];
    bool seen = false;
    for (unsigned j = 0; j < baseline.captured; ++j) seen |= baseline.tids[j] == tid;
    HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION | SYNCHRONIZE, FALSE, tid);
    if (!thread) {
      const DWORD error = GetLastError();
      std::fprintf(stderr, "TCP_PRIORITY_THREAD_CURRENT mode=%d tid=%lu seen_baseline_tid=%d open_error=%lu\n",
                   mode, tid, seen, error);
      continue;
    }
    ++opened;
    // A snapshot TID can retire and be reused. Query no thread details until its
    // newly retained handle is confirmed to belong to this process.
    const DWORD owner = GetProcessIdOfThread(thread);
    if (owner == GetCurrentProcessId()) {
      FILETIME birth{}, exit{}, kernel{}, user{};
      const BOOL times = GetThreadTimes(thread, &birth, &exit, &kernel, &user);
      const DWORD times_error = times ? 0 : GetLastError();
      const uint64_t created = (uint64_t{birth.dwHighDateTime} << 32) | birth.dwLowDateTime;
      const DWORD wait = WaitForSingleObject(thread, 0);
      void* start = nullptr;
      ULONG bytes = 0;
      const NTSTATUS status = query ? query(thread, static_cast<THREADINFOCLASS>(9),
          &start, sizeof(start), &bytes) : static_cast<NTSTATUS>(0xc0000002u);
      wchar_t path[1024]{};
      const wchar_t* name = L"unknown";
      unsigned long long rva = 0;
      HMODULE module = nullptr;
      bool mapped = false;
      if (status >= 0 && bytes == sizeof(start) && start &&
          GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(start), &module)) {
        ++modules;  // Hold this reference only while copying its name and RVA.
        const DWORD length = GetModuleFileNameW(module, path, 1024);
        const uintptr_t address = reinterpret_cast<uintptr_t>(start);
        const uintptr_t base = reinterpret_cast<uintptr_t>(module);
        if (length > 0 && length < 1024 && address >= base) {
          const wchar_t* slash = std::wcsrchr(path, L'\\');
          name = slash ? slash + 1 : path;
          rva = static_cast<unsigned long long>(address - base);
          mapped = true;
        }
        if (FreeLibrary(module)) ++released;
        else std::fprintf(stderr, "TCP_PRIORITY_THREAD_MODULE_CLOSE mode=%d tid=%lu error=%lu\n",
                          mode, tid, GetLastError());
      }
      std::fprintf(stderr, "TCP_PRIORITY_THREAD_CURRENT mode=%d tid=%lu seen_baseline_tid=%d "
          "birth=%llu times_error=%lu wait=%lu query_status=%lu query_bytes=%lu "
          "mapped=%d module=%ls start_rva=%llu\n", mode, tid, seen,
          static_cast<unsigned long long>(created), times_error, wait,
          static_cast<unsigned long>(status), bytes, mapped, name, rva);
    } else {
      std::fprintf(stderr, "TCP_PRIORITY_THREAD_CURRENT mode=%d tid=%lu skipped_owner_changed=1\n", mode, tid);
    }
    if (CloseHandle(thread)) ++closed;
    else std::fprintf(stderr, "TCP_PRIORITY_THREAD_CLOSE mode=%d tid=%lu error=%lu\n", mode, tid, GetLastError());
  }
  std::fprintf(stderr, "TCP_PRIORITY_THREAD_DIAGNOSTIC_END mode=%d opened=%u closed=%u "
      "module_refs=%u released=%u unclosed_handles=%u unreleased_modules=%u\n",
      mode, opened, closed, modules, released, opened - closed, modules - released);
  std::fflush(stderr);
}
}  // namespace gesher::test
