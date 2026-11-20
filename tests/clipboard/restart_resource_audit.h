// Isolated diagnostics for the actual Windows restart fixture. The normal fixture
// still runs first with its unchanged process-wide handle equality assertion.
#pragma once
#include <processsnapshot.h>
#include <vector>

namespace gesher::zygote {
void RestartAudit(const char* stage, const void* resource, BOOL success, DWORD error) {
  DWORD saved_error = GetLastError(), count = 0;
  BOOL counted = GetProcessHandleCount(GetCurrentProcess(), &count);
  DWORD count_error = counted ? ERROR_SUCCESS : GetLastError();
  std::printf("restart audit stage=%s resource=%p success=%d error=%lu handles=%lu counted=%d count-error=%lu\n",
      stage, const_cast<void*>(resource), success, error, count, counted, count_error);
  std::fflush(stdout);
  SetLastError(saved_error);
}
}

namespace restart_audit {
struct Entry {
  HANDLE handle;
  std::wstring type, name;
  DWORD pid, tid;
};
using Entries = std::vector<Entry>;

std::wstring Bounded(const wchar_t* value, WORD bytes) {
  size_t length = bytes / sizeof(wchar_t);
  return value == nullptr ? std::wstring() : std::wstring(value, length > 256 ? 256 : length);
}

DWORD Capture(Entries* entries) {
  HPSS snapshot = nullptr;
  HPSSWALK marker = nullptr;
  DWORD status = PssCaptureSnapshot(GetCurrentProcess(),
      static_cast<PSS_CAPTURE_FLAGS>(PSS_CAPTURE_HANDLES | PSS_CAPTURE_HANDLE_NAME_INFORMATION
          | PSS_CAPTURE_HANDLE_BASIC_INFORMATION | PSS_CAPTURE_HANDLE_TYPE_SPECIFIC_INFORMATION),
      0, &snapshot);
  if (status != ERROR_SUCCESS) return status;
  status = PssWalkMarkerCreate(nullptr, &marker);
  if (status == ERROR_SUCCESS) {
    for (;;) {
      PSS_HANDLE_ENTRY handle{};
      status = PssWalkSnapshot(snapshot, PSS_WALK_HANDLES, marker, &handle, sizeof(handle));
      if (status == ERROR_NO_MORE_ITEMS) { status = ERROR_SUCCESS; break; }
      if (status != ERROR_SUCCESS) break;
      if (entries->size() >= 1024) { status = ERROR_MORE_DATA; break; }
      DWORD pid = 0, tid = 0;
      if ((handle.Flags & PSS_HANDLE_HAVE_TYPE_SPECIFIC_INFORMATION) != 0) {
        if (handle.ObjectType == PSS_OBJECT_TYPE_PROCESS) {
          pid = handle.TypeSpecificInformation.Process.ProcessId;
        } else if (handle.ObjectType == PSS_OBJECT_TYPE_THREAD) {
          pid = handle.TypeSpecificInformation.Thread.ProcessId;
          tid = handle.TypeSpecificInformation.Thread.ThreadId;
        }
      }
      entries->push_back({handle.Handle, Bounded(handle.TypeName, handle.TypeNameLength),
          Bounded(handle.ObjectName, handle.ObjectNameLength), pid, tid});
    }
    DWORD cleanup = PssWalkMarkerFree(marker);
    if (status == ERROR_SUCCESS) status = cleanup;
  }
  DWORD cleanup = PssFreeSnapshot(GetCurrentProcess(), snapshot);
  return status == ERROR_SUCCESS ? cleanup : status;
}

void Delta(const char* stage, const Entries& before, const Entries& after) {
  std::printf("restart audit snapshot=%s before=%zu after=%zu\n", stage, before.size(), after.size());
  for (const auto& entry : after) {
    bool found = false;
    for (const auto& old : before) {
      if (entry.handle == old.handle && entry.type == old.type && entry.name == old.name
          && entry.pid == old.pid && entry.tid == old.tid) { found = true; break; }
    }
    if (!found) {
      std::printf("restart audit added handle=%p type=%ls name=%ls pid=%lu tid=%lu\n",
          entry.handle, entry.type.c_str(), entry.name.c_str(), entry.pid, entry.tid);
    }
  }
  std::fflush(stdout);
}
}
