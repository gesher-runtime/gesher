// Recursive ReadDirectoryChangesW, one 32-KiB buffer and three handles per server.
// Never enters the inotify/socketpair mixed-fd 10ms fallback (Issue #178 NATIVE-01).
#include <windows.h>
#include <atomic>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include "services_dexopt_watch.h"

namespace gesher::dexopt {
namespace {
struct Watch {
  HANDLE directory = INVALID_HANDLE_VALUE, ready = nullptr, wake = nullptr;
  OVERLAPPED overlapped = {};
  std::vector<DWORD> buffer = std::vector<DWORD>(32 * 1024 / sizeof(DWORD));
  std::mutex lock;
  std::atomic<bool> reading{false};
  bool pending = false, closed = false;

  ~Watch() {
    if (pending) {
      CancelIoEx(directory, &overlapped);
      DWORD ignored = 0;
      GetOverlappedResult(directory, &overlapped, &ignored, TRUE);
    }
    if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory);
    if (ready != nullptr) CloseHandle(ready);
    if (wake != nullptr) CloseHandle(wake);
  }
  bool Arm(uint32_t* error) {
    ResetEvent(ready);
    overlapped = {};
    overlapped.hEvent = ready;
    DWORD ignored = 0;
    BOOL ok = ReadDirectoryChangesW(directory, buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(DWORD)),
        TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
        FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE, &ignored, &overlapped, nullptr);
    if (!ok && GetLastError() != ERROR_IO_PENDING) { *error = GetLastError(); return false; }
    pending = true;
    return true;
  }
};
std::mutex watches_lock;
std::map<uint64_t, std::shared_ptr<Watch>> watches;
uint64_t next_token = 0;
std::shared_ptr<Watch> Find(uint64_t token) {
  std::lock_guard<std::mutex> guard(watches_lock);
  auto it = watches.find(token);
  return it == watches.end() ? nullptr : it->second;
}
}

uint64_t OpenProfileWatch(const std::u16string& path, uint32_t* error) {
  *error = ERROR_SUCCESS;
  auto watch = std::make_shared<Watch>();
  watch->directory = CreateFileW(reinterpret_cast<const wchar_t*>(path.c_str()), FILE_LIST_DIRECTORY,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
  if (watch->directory == INVALID_HANDLE_VALUE) { *error = GetLastError(); return 0; }
  watch->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  watch->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (watch->ready == nullptr || watch->wake == nullptr) { *error = GetLastError(); return 0; }
  // Queue native I/O before Java takes the installed-generation snapshot.
  if (!watch->Arm(error)) return 0;
  std::lock_guard<std::mutex> guard(watches_lock);
  uint64_t token = ++next_token;
  watches.emplace(token, std::move(watch));
  return token;
}

bool ReadProfileWatch(uint64_t token, std::vector<std::u16string>* names, uint32_t* error) {
  names->clear(); *error = ERROR_SUCCESS;
  auto watch = Find(token);
  if (!watch) { *error = ERROR_INVALID_HANDLE; return false; }
  if (watch->reading.exchange(true)) { *error = ERROR_BUSY; return false; }
  struct Reading { std::shared_ptr<Watch> watch; ~Reading() { watch->reading = false; } } reading{watch};
  HANDLE handles[] = {watch->wake, watch->ready};
  DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
  std::lock_guard<std::mutex> guard(watch->lock);
  if (watch->closed) { *error = ERROR_OPERATION_ABORTED; return false; }
  if (wait == WAIT_OBJECT_0) return true; // explicit package refresh, I/O remains pending
  if (wait != WAIT_OBJECT_0 + 1) { *error = GetLastError(); return false; }
  DWORD bytes = 0;
  BOOL ok = GetOverlappedResult(watch->directory, &watch->overlapped, &bytes, FALSE);
  watch->pending = false;
  if (!ok && GetLastError() != ERROR_NOTIFY_ENUM_DIR) { *error = GetLastError(); return false; }
  if (!ok) bytes = 0;
  const auto* buffer = reinterpret_cast<const unsigned char*>(watch->buffer.data());
  size_t offset = 0;
  while (offset < bytes) {
    constexpr size_t header = offsetof(FILE_NOTIFY_INFORMATION, FileName);
    if (bytes - offset < header) { names->clear(); break; }
    const auto* record = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer + offset);
    size_t length = record->FileNameLength;
    if ((length % sizeof(char16_t)) != 0 || length > bytes - offset - header) { names->clear(); break; }
    names->emplace_back(reinterpret_cast<const char16_t*>(record->FileName), length / sizeof(char16_t));
    if (record->NextEntryOffset == 0) break;
    size_t next = record->NextEntryOffset;
    if (next < header + length || next % sizeof(DWORD) != 0 || next >= bytes - offset) { names->clear(); break; }
    offset += next;
  }
  // Re-arm before returning names, so Java processing never opens a notification gap.
  return watch->Arm(error);
}

void WakeProfileWatch(uint64_t token) {
  auto watch = Find(token);
  if (watch) SetEvent(watch->wake);
}

void CloseProfileWatch(uint64_t token) {
  std::shared_ptr<Watch> watch;
  {
    std::lock_guard<std::mutex> guard(watches_lock);
    auto it = watches.find(token);
    if (it == watches.end()) return;
    watch = it->second;
    watches.erase(it);
  }
  std::lock_guard<std::mutex> guard(watch->lock);
  watch->closed = true;
  if (watch->pending) CancelIoEx(watch->directory, &watch->overlapped);
  SetEvent(watch->wake);
  // The reader's shared ownership keeps OVERLAPPED/buffer/handles alive until
  // cancellation has completed. The destructor drains any pending native I/O.
}
}
