// gesher-base: inotify over ReadDirectoryChangesW (docs/posix.md, ADR 0045).
//
// An inotify fd is, like an eventfd, a CRT fd on NUL that epoll.cc maps to
// its object; its readiness is a read-only eventfd, set while events are
// queued, so poll and epoll_wait (libutils' Looper, FileObserver,
// LinuxWatchService) wait on it like on any other fd.
//
// Each watch is an overlapped ReadDirectoryChangesW on its directory (a
// watched file: on its parent, filtered by name). The inotify's worker
// wakes on the completion port and translates the native records:
//   added -> IN_CREATE, removed -> IN_DELETE, renamed -> IN_MOVED_FROM and
//   IN_MOVED_TO with one cookie, modified -> IN_MODIFY, or IN_ATTRIB when
//   only the change time moved (metadata);
//   a watched file renamed within its parent -> IN_MOVE_SELF, then events
//   continue on the same watch descriptor under the new name;
//   the watched directory gone -> IN_DELETE_SELF, IN_IGNORED;
//   a lost buffer -> IN_Q_OVERFLOW.
// Windows reports no close: IN_CLOSE_WRITE is reported once a created or
// modified file is no longer open for writing by anyone, which the worker
// checks every 50 ms by opening it with write sharing denied. IN_OPEN,
// IN_ACCESS and IN_CLOSE_NOWRITE are never reported.
//
// NTFS reports a write only when it updates the file's directory entry,
// often not before the writer closes the file. Writes through this
// process's write()/pwrite() therefore report IN_MODIFY themselves
// (InotifyNoteWrite, ADR 0057). File watches drain completed native reads
// first, preserving the order of earlier renames and this write.
#include <sys/inotify.h>

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gesher/reserved_names.h>

#include "event_internal.h"
#include "internal.h"

namespace gesher {
namespace {

constexpr DWORD kBufferSize = 64 * 1024;
constexpr size_t kMaxQueued = 16384;  // Linux's max_queued_events
constexpr int kMaxDrainReads = 64;
constexpr ULONG_PTR kQuit = 0;
constexpr ULONG_PTR kWake = 1;  // a write was noted: probe for IN_CLOSE_WRITE
constexpr ULONG_PTR kFileReady = 2;
constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                                FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_CREATION |
                                FILE_NOTIFY_CHANGE_SECURITY;
constexpr uint32_t kAlways = IN_IGNORED | IN_Q_OVERFLOW | IN_UNMOUNT;

// FILE_NOTIFY_EXTENDED_INFORMATION (Windows 10 1709), which older SDK
// headers lack.
struct NotifyExtended {
  DWORD next;
  DWORD action;
  LARGE_INTEGER creation_time, modification_time, change_time, access_time;
  LARGE_INTEGER allocated_length, file_size;
  DWORD attributes;
  DWORD reparse_tag;
  LARGE_INTEGER file_id, parent_file_id;
  DWORD name_length;
  WCHAR name[1];
};
constexpr int kReadDirectoryNotifyExtendedInformation = 2;
using ReadDirectoryChangesExWFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, BOOL, DWORD, LPDWORD,
                                                LPOVERLAPPED, LPOVERLAPPED_COMPLETION_ROUTINE, int);

ReadDirectoryChangesExWFn ExtendedReader() {
  static auto fn = reinterpret_cast<ReadDirectoryChangesExWFn>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ReadDirectoryChangesExW")));
  return fn;
}

struct Watch {
  int wd = 0;
  uint32_t mask = 0;
  std::wstring dir;   // the directory read (a watched file's parent)
  std::wstring file;  // a watched file's name; empty for a directory
  unsigned long long volume = 0, index = 0;
  HANDLE handle = INVALID_HANDLE_VALUE;
  HANDLE identity = INVALID_HANDLE_VALUE;  // the watched inode, not its current name
  HANDLE event = nullptr;  // file-watch reads can also be consumed by a writer
  HANDLE port = nullptr;
  PTP_WAIT wait = nullptr;
  OVERLAPPED ov = {};
  std::vector<DWORD> buffer = std::vector<DWORD>(kBufferSize / sizeof(DWORD));
  bool pending = false;
  bool extended = false;
  bool removed = false;  // rm'd or ended: dropped when its read completes
  bool ignored_sent = false;
  bool awaiting_rename = false;
  std::set<std::wstring> writing;  // names to report IN_CLOSE_WRITE for
  // Each name's last modification time and size (extended records): a change
  // that moves neither is to metadata only (IN_ATTRIB).
  std::map<std::wstring, std::pair<long long, long long>> contents;

  ~Watch() {
    if (wait != nullptr) {
      SetThreadpoolWait(wait, nullptr, nullptr);
      WaitForThreadpoolWaitCallbacks(wait, TRUE);
      CloseThreadpoolWait(wait);
    }
    if (event != nullptr && pending) {
      CancelIoEx(handle, &ov);
      DWORD bytes;
      GetOverlappedResult(handle, &ov, &bytes, TRUE);
    }
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    if (event != nullptr) CloseHandle(event);
    if (identity != INVALID_HANDLE_VALUE) CloseHandle(identity);
  }
};

void CALLBACK FileReadReady(PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT, TP_WAIT_RESULT) {
  const Watch& w = *static_cast<Watch*>(context);
  // A writer may consume this read before the wake is handled. The
  // worker looks up the descriptor and checks its current read's event.
  PostQueuedCompletionStatus(w.port, static_cast<DWORD>(w.wd), kFileReady, nullptr);
}

}  // namespace

struct Inotify;
std::mutex g_live_mutex;
std::vector<Inotify*> g_live;  // every Inotify, for InotifyNoteWrite
std::atomic<int> g_live_count{0};

struct Inotify {
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<std::string> queue;  // events, in Linux's layout
  bool overflow_queued = false;  // one loss marker, until ReadInotify removes it
  std::map<int, std::unique_ptr<Watch>> watches;
  int next_wd = 1;
  uint32_t next_cookie = 1;
  HANDLE port = nullptr;
  std::shared_ptr<EventFd> ready;
  std::thread worker;

  ~Inotify() {
    {
      std::lock_guard<std::mutex> lock(g_live_mutex);
      auto it = std::find(g_live.begin(), g_live.end(), this);
      if (it != g_live.end()) {
        g_live.erase(it);
        g_live_count.fetch_sub(1);
      }
    }
    if (worker.joinable()) {
      PostQueuedCompletionStatus(port, 0, kQuit, nullptr);
      worker.join();
    }
    watches.clear();
    if (port != nullptr) CloseHandle(port);
  }

  // Queues an event; called with `mutex` held.
  void Emit(int wd, uint32_t mask, uint32_t cookie, const std::string& name) {
    // Overflow precedes ordinary duplicate merging, including a duplicate at
    // capacity. Keep one marker anywhere in the queue, not just at its tail.
    const bool overflow = mask == IN_Q_OVERFLOW || queue.size() >= kMaxQueued;
    if (overflow && overflow_queued) return;
    size_t len = overflow || name.empty() ? 0 : (name.size() + 1 + 15) / 16 * 16;
    std::string event(sizeof(inotify_event) + len, '\0');
    inotify_event header = overflow ? inotify_event{-1, IN_Q_OVERFLOW, 0, 0}
                                   : inotify_event{wd, mask, cookie, static_cast<uint32_t>(len)};
    memcpy(event.data(), &header, sizeof(header));
    if (!overflow) memcpy(event.data() + sizeof(header), name.data(), name.size());
    // Linux merges an event with the identical one before it, if unread.
    if (!overflow && !queue.empty() && queue.back() == event) return;
    queue.push_back(std::move(event));
    overflow_queued |= overflow;
    SetReadable(*ready, true);
    changed.notify_all();
  }

  // A watch's event, if its mask wants it; IN_ONESHOT ends the watch.
  void EmitFor(Watch& w, uint32_t mask, uint32_t cookie, const std::wstring& name) {
    if ((mask & (w.mask | kAlways)) == 0 || w.removed) return;
    Emit(w.wd, mask, cookie, Narrow(reserved_names::Decoded(name).c_str()));
    if ((w.mask & IN_ONESHOT) != 0 && (mask & kAlways) == 0) End(w);
  }

  // Ends a watch: IN_IGNORED now, the watch itself once its read completes.
  void End(Watch& w) {
    if (!w.ignored_sent) {
      w.ignored_sent = true;
      Emit(w.wd, IN_IGNORED, 0, "");
    }
    if (!w.removed) {
      w.removed = true;
      if (w.pending) CancelIoEx(w.handle, &w.ov);
    }
  }

  bool Issue(Watch& w) {
    memset(&w.ov, 0, sizeof(w.ov));
    if (w.event != nullptr) {
      ResetEvent(w.event);
      w.ov.hEvent = w.event;
    }
    DWORD unused;
    w.pending = w.extended
                    ? ExtendedReader()(w.handle, w.buffer.data(), kBufferSize, FALSE, kNotifyFilter,
                                       &unused, &w.ov, nullptr, kReadDirectoryNotifyExtendedInformation)
                    : ReadDirectoryChangesW(w.handle, w.buffer.data(), kBufferSize, FALSE, kNotifyFilter,
                                            &unused, &w.ov, nullptr);
    if (w.pending && w.wait != nullptr) SetThreadpoolWait(w.wait, w.event, nullptr);
    return w.pending;
  }

  void Changes(Watch& w, DWORD bytes);
  void Complete(Watch& w, BOOL ok, DWORD bytes);
  bool DrainFile(Watch& w);
  void ProbeWriters();
  void Run();
};

namespace {

bool IsDirectory(const std::wstring& path) {
  DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
  return dir.empty() || dir.back() == L'\\' ? dir + name : dir + L'\\' + name;
}

unsigned long long FileIndex(const BY_HANDLE_FILE_INFORMATION& info) {
  return (static_cast<unsigned long long>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
}

bool MatchesFile(const Watch& w, const BY_HANDLE_FILE_INFORMATION& info) {
  return info.dwVolumeSerialNumber == w.volume && FileIndex(info) == w.index;
}

bool MatchesNamedEntry(const Watch& w, const std::wstring& name) {
  // Following a symlink here could identify its target as the watched file.
  HANDLE candidate = CreateFileW(Join(w.dir, name).c_str(), FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (candidate == INVALID_HANDLE_VALUE) return false;
  BY_HANDLE_FILE_INFORMATION info;
  bool same = GetFileInformationByHandle(candidate, &info) && MatchesFile(w, info);
  CloseHandle(candidate);
  return same;
}

}  // namespace

// Case-only renames can report a transient removal before their rename pair.
// The retained file handle distinguishes a live inode from an actual unlink;
// looking up the old pathname races subsequent renames and replacements.
bool FileStillLinked(const Watch& w) {
  FILE_STANDARD_INFO info;
  return w.identity != INVALID_HANDLE_VALUE &&
         GetFileInformationByHandleEx(w.identity, FileStandardInfo, &info, sizeof(info)) &&
         !info.DeletePending && info.NumberOfLinks != 0;
}

// One completed read's records.
void Inotify::Changes(Watch& w, DWORD bytes) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(w.buffer.data());
  size_t remaining = (std::min)(bytes, kBufferSize);
  const size_t header = w.extended ? offsetof(NotifyExtended, name)
                                   : offsetof(FILE_NOTIFY_INFORMATION, FileName);
  uint32_t cookie = 0;
  while (!w.removed && remaining >= header) {
    DWORD next, action, attributes = 0, name_length;
    unsigned long long file_id = 0;
    std::pair<long long, long long> now;
    bool attrib_only = false, known_attributes = false;
    if (w.extended) {
      NotifyExtended e;
      // Records are DWORD-aligned; the extended header contains 64-bit values.
      memcpy(&e, p, header);
      next = e.next;
      action = e.action;
      attributes = e.attributes;
      file_id = static_cast<unsigned long long>(e.file_id.QuadPart);
      known_attributes = true;
      name_length = e.name_length;
      now = {e.modification_time.QuadPart, e.file_size.QuadPart};
    } else {
      FILE_NOTIFY_INFORMATION e;
      memcpy(&e, p, header);
      next = e.NextEntryOffset;
      action = e.Action;
      name_length = e.FileNameLength;
    }
    size_t record = next == 0 ? remaining : next;
    if (record > remaining || record < header || name_length > record - header ||
        name_length % sizeof(WCHAR) != 0 || next % sizeof(DWORD) != 0) {
      break;
    }
    std::wstring name(reinterpret_cast<const WCHAR*>(p + header), name_length / sizeof(WCHAR));
    if (known_attributes) {
      auto seen = w.contents.find(name);
      if (action == FILE_ACTION_MODIFIED && seen != w.contents.end()) {
        attrib_only = seen->second == now;  // chmod, times: content unchanged
      }
      if (action == FILE_ACTION_REMOVED || action == FILE_ACTION_RENAMED_OLD_NAME) {
        w.contents.erase(name);
      } else {
        w.contents[name] = now;
      }
    }
    if (!known_attributes && action != FILE_ACTION_REMOVED &&
        action != FILE_ACTION_RENAMED_OLD_NAME && IsDirectory(Join(w.dir, name))) {
      attributes = FILE_ATTRIBUTE_DIRECTORY;
    }
    uint32_t isdir = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? IN_ISDIR : 0;
    if (!w.file.empty()) {
      // Track renames even when the user did not request IN_MOVE_SELF.
      if (w.awaiting_rename && action == FILE_ACTION_RENAMED_NEW_NAME) {
        // Basic records carry no identity or guaranteed pairing. Only
        // follow a destination that still identifies the watched file.
        bool same_file = w.index != 0 &&
                         (file_id != 0 ? file_id == w.index : MatchesNamedEntry(w, name));
        if (same_file) {
          w.file = name;
          w.awaiting_rename = false;
        } else if (file_id == 0 || w.index == 0) {
          // The destination may already have moved or been deleted. Basic
          // records cannot reconstruct that history by their current names.
          // Tell the caller to rescan/rebuild instead of silently losing it.
          Emit(-1, IN_Q_OVERFLOW, 0, "");
        }
      } else if (w.awaiting_rename
                     ? file_id != 0 && file_id == w.index
                     : (file_id == 0 || w.index == 0 || file_id == w.index) &&
                           CompareStringOrdinal(name.c_str(), -1, w.file.c_str(), -1,
                                                TRUE) == CSTR_EQUAL) {
        switch (action) {
          case FILE_ACTION_MODIFIED:
            EmitFor(w, attrib_only ? IN_ATTRIB : IN_MODIFY, 0, L"");
            if (!attrib_only) w.writing.insert(L"");
            break;
          case FILE_ACTION_REMOVED:
            if (FileStillLinked(w)) break;
            EmitFor(w, IN_DELETE_SELF, 0, L"");
            End(w);
            break;
          case FILE_ACTION_RENAMED_OLD_NAME:
            EmitFor(w, IN_MOVE_SELF, 0, L"");
            w.awaiting_rename = !w.removed;
            break;
          default:
            break;
        }
      } else if (w.awaiting_rename && file_id == 0) {
        // An unidentifiable basic record may belong to the renamed file.
        // Do not invent a modification/deletion or imply a complete stream.
        Emit(-1, IN_Q_OVERFLOW, 0, "");
      }
    } else {
      switch (action) {
        case FILE_ACTION_ADDED:
          EmitFor(w, IN_CREATE | isdir, 0, name);
          if (!isdir) w.writing.insert(name);
          break;
        case FILE_ACTION_REMOVED:
          w.writing.erase(name);
          EmitFor(w, IN_DELETE | isdir, 0, name);
          break;
        case FILE_ACTION_MODIFIED:
          // A directory's own changes are not its parent's events on Linux.
          if (!isdir) {
            EmitFor(w, attrib_only ? IN_ATTRIB : IN_MODIFY, 0, name);
            if (!attrib_only) w.writing.insert(name);
          }
          break;
        case FILE_ACTION_RENAMED_OLD_NAME:
          cookie = next_cookie++;
          w.writing.erase(name);
          EmitFor(w, IN_MOVED_FROM | isdir, cookie, name);
          break;
        case FILE_ACTION_RENAMED_NEW_NAME:
          EmitFor(w, IN_MOVED_TO | isdir, cookie, name);
          cookie = 0;
          break;
        default:
          break;
      }
    }
    if (next == 0) break;
    p += next;
    remaining -= next;
  }
}

void Inotify::Complete(Watch& w, BOOL ok, DWORD bytes) {
  w.pending = false;
  if (w.removed) return;
  if (ok && bytes > 0) {
    Changes(w, bytes);
  } else if (ok && IsDirectory(w.dir)) {
    Emit(-1, IN_Q_OVERFLOW, 0, "");  // more changes than the buffer held
  }
  if (!w.removed && (!ok || (bytes == 0 && !IsDirectory(w.dir)) || !Issue(w))) {
    if (w.file.empty()) EmitFor(w, IN_DELETE_SELF, 0, L"");
    End(w);
  }
}

// File-watch reads signal directly, even if the worker is busy. Reissue
// after each completed read to collect records buffered by the filesystem
// between requests. Always consume and rearm under the inotify mutex.
bool Inotify::DrainFile(Watch& w) {
  for (int reads = 0; w.pending && WaitForSingleObject(w.event, 0) == WAIT_OBJECT_0; ++reads) {
    if (reads == kMaxDrainReads) return false;
    SetThreadpoolWait(w.wait, nullptr, nullptr);
    DWORD bytes = 0;
    BOOL ok = GetOverlappedResult(w.handle, &w.ov, &bytes, FALSE);
    Complete(w, ok, bytes);
  }
  return true;
}

// IN_CLOSE_WRITE for the files no one has open for writing any more.
void Inotify::ProbeWriters() {
  std::vector<std::pair<int, std::wstring>> probes;
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& [wd, w] : watches) {
      if (w->removed || w->awaiting_rename) continue;
      if ((w->mask & IN_CLOSE_WRITE) == 0) {
        w->writing.clear();
        continue;
      }
      for (const std::wstring& name : w->writing) probes.emplace_back(wd, name);
    }
  }
  for (auto& [wd, name] : probes) {
    bool file_watch = name.empty();
    std::wstring path;
    {
      std::lock_guard<std::mutex> lock(mutex);
      auto it = watches.find(wd);
      if (it == watches.end() || it->second->removed) continue;
      if (file_watch && it->second->awaiting_rename) continue;
      path = Join(it->second->dir, file_watch ? it->second->file : name);
    }
    // Denying write sharing fails while any handle can write.
    DWORD flags = file_watch ? FILE_FLAG_OPEN_REPARSE_POINT : FILE_ATTRIBUTE_NORMAL;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, flags, nullptr);
    DWORD error = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) continue;
    // The file may have moved while its rename notification was queued.
    if (file_watch && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
                       error == ERROR_DELETE_PENDING)) {
      continue;
    }
    BY_HANDLE_FILE_INFORMATION info;
    bool got_info = file_watch && error == 0 && GetFileInformationByHandle(h, &info);
    std::lock_guard<std::mutex> lock(mutex);
    auto it = watches.find(wd);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (it == watches.end() || it->second->removed) continue;
    Watch& w = *it->second;
    if (file_watch) {
      if (w.awaiting_rename || path != Join(w.dir, w.file)) continue;
      if (error == 0 && w.index != 0 && (!got_info || !MatchesFile(w, info))) continue;
    }
    if (w.writing.erase(name) != 0 && error == 0) EmitFor(w, IN_CLOSE_WRITE, 0, name);
  }
}

void Inotify::Run() {
  bool quitting = false;
  for (;;) {
    bool probing;
    {
      std::lock_guard<std::mutex> lock(mutex);
      probing = false;
      for (auto& [wd, w] : watches) {
        probing |= !w->writing.empty() && !w->removed && !w->awaiting_rename;
      }
    }
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    OVERLAPPED* ov = nullptr;
    BOOL ok = GetQueuedCompletionStatus(port, &bytes, &key, &ov, probing ? 50 : INFINITE);
    DWORD error = ok ? 0 : GetLastError();
    if (ov == nullptr) {
      if (ok && key == kFileReady) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          auto it = watches.find(static_cast<int>(bytes));
          if (it != watches.end()) {
            DrainFile(*it->second);
            if (it->second->removed && !it->second->pending) watches.erase(it);
          }
          if (quitting && watches.empty()) return;
        }
        ProbeWriters();
        continue;
      }
      if ((!ok && error == WAIT_TIMEOUT) || (ok && key == kWake)) {
        ProbeWriters();
        continue;
      }
      if (!ok) return;  // the port is gone
      // kQuit: cancel every read, then wait for them to end.
      std::lock_guard<std::mutex> lock(mutex);
      quitting = true;
      for (auto& [wd, w] : watches) {
        if (!w->removed) {
          w->removed = true;
          if (w->pending) CancelIoEx(w->handle, &w->ov);
        }
      }
      if (watches.empty()) return;
      continue;
    }
    std::unique_lock<std::mutex> lock(mutex);
    Watch* w = reinterpret_cast<Watch*>(key);
    Complete(*w, ok, bytes);
    if (w->removed) {
      watches.erase(w->wd);
      if (quitting && watches.empty()) return;
      continue;
    }
    lock.unlock();
    ProbeWriters();
  }
}

void InotifyNoteWrite(int fd) {
  if (g_live_count.load(std::memory_order_relaxed) == 0) return;
  InotifyNoteWrite(HandleFromFd(fd));
}

void InotifyNoteWrite(HANDLE h) {
  if (g_live_count.load(std::memory_order_relaxed) == 0) return;
  if (h == INVALID_HANDLE_VALUE || GetFileType(h) != FILE_TYPE_DISK) return;
  std::wstring full(32768, L' ');
  DWORD n = GetFinalPathNameByHandleW(h, full.data(), 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (n == 0 || n >= 32768) return;
  full.resize(n);
  size_t slash = full.find_last_of(L'\\');
  if (slash == std::wstring::npos) return;
  std::wstring dir = full.substr(0, slash), name = full.substr(slash + 1);
  auto same = [](const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
  };
  BY_HANDLE_FILE_INFORMATION info;
  bool queried_info = false;
  bool got_info = false;
  std::lock_guard<std::mutex> live(g_live_mutex);
  for (Inotify* in : g_live) {
    bool noted = false;
    {
      std::lock_guard<std::mutex> lock(in->mutex);
      for (auto it = in->watches.begin(); it != in->watches.end();) {
        auto current = it++;
        Watch& w = *current->second;
        if (w.removed || !same(w.dir, dir)) continue;
        if (w.file.empty()) {
          in->EmitFor(w, IN_MODIFY, 0, name);
          if (!w.removed) w.writing.insert(name);
        } else {
          if ((w.mask & (IN_MODIFY | IN_CLOSE_WRITE)) == 0) continue;
          if (!queried_info) {
            got_info = GetFileInformationByHandle(h, &info);
            queried_info = true;
          }
          if (w.index != 0 ? !got_info || !MatchesFile(w, info) : !same(w.file, name)) continue;
          if (!in->DrainFile(w)) {
            // The producer is outrunning this bounded synchronous drain.
            // An overflow is better than publishing the write out of order.
            in->Emit(-1, IN_Q_OVERFLOW, 0, "");
            continue;
          }
          if (w.removed) {
            if (!w.pending) in->watches.erase(current);
            continue;
          }
          in->EmitFor(w, IN_MODIFY, 0, L"");
          if (!w.removed) w.writing.insert(L"");
        }
        noted = true;
      }
    }
    if (noted) PostQueuedCompletionStatus(in->port, 0, kWake, nullptr);
  }
}

int ReadInotify(Inotify& in, void* buf, unsigned int count) {
  std::unique_lock<std::mutex> lock(in.mutex);
  while (in.queue.empty()) {
    if (IsNonBlocking(*in.ready)) return errno = EAGAIN, -1;
    in.changed.wait(lock);
  }
  size_t total = 0;
  while (!in.queue.empty() && total + in.queue.front().size() <= count) {
    memcpy(static_cast<char*>(buf) + total, in.queue.front().data(), in.queue.front().size());
    total += in.queue.front().size();
    if (in.overflow_queued) {
      inotify_event header;
      memcpy(&header, in.queue.front().data(), sizeof(header));
      if (header.mask == IN_Q_OVERFLOW) in.overflow_queued = false;
    }
    in.queue.pop_front();
  }
  if (in.queue.empty()) SetReadable(*in.ready, false);
  if (total == 0) return errno = EINVAL, -1;  // the buffer holds not even one event
  return static_cast<int>(total);
}

}  // namespace gesher

using gesher::Inotify;
using gesher::Watch;

extern "C" {

int inotify_init1(int flags) {
  if ((flags & ~(IN_NONBLOCK | IN_CLOEXEC)) != 0) return errno = EINVAL, -1;
  auto in = std::make_shared<Inotify>();
  in->ready = gesher::NewReadiness((flags & IN_NONBLOCK) != 0);
  in->port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
  if (in->ready == nullptr || in->port == nullptr) return gesher::FailWithLastError();
  Inotify* raw = in.get();
  in->worker = std::thread([raw] { raw->Run(); });
  {
    std::lock_guard<std::mutex> lock(gesher::g_live_mutex);
    gesher::g_live.push_back(raw);
    gesher::g_live_count.fetch_add(1);
  }
  // Every fd gesher-base reserves is uninheritable: IN_CLOEXEC holds always.
  std::shared_ptr<gesher::EventFd> ready = in->ready;
  return gesher::RegisterInotify(std::move(in), std::move(ready));
}

int inotify_init(void) { return inotify_init1(0); }

int inotify_add_watch(int fd, const char* path, uint32_t mask) {
  std::shared_ptr<Inotify> in = gesher::LookupInotify(fd);
  if (in == nullptr) {
    return errno = gesher::HandleFromFd(fd) == INVALID_HANDLE_VALUE ? EBADF : EINVAL, -1;
  }
  if ((mask & IN_ALL_EVENTS) == 0 ||
      (mask & (IN_MASK_ADD | IN_MASK_CREATE)) == (IN_MASK_ADD | IN_MASK_CREATE)) {
    return errno = EINVAL, -1;
  }
  std::wstring windows;
  if (!gesher::DiskPath(path, &windows, EACCES)) return -1;
  DWORD open_flags = FILE_FLAG_BACKUP_SEMANTICS |
                     ((mask & IN_DONT_FOLLOW) != 0 ? FILE_FLAG_OPEN_REPARSE_POINT : 0);
  auto w = std::make_unique<Watch>();
  HANDLE target = CreateFileW(windows.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, open_flags, nullptr);
  if (target == INVALID_HANDLE_VALUE) return gesher::FailWithLastError();
  w->identity = target;
  BY_HANDLE_FILE_INFORMATION info;
  wchar_t final_path[32768];
  DWORD n = 0;
  bool ok = GetFileInformationByHandle(target, &info) &&
            (n = GetFinalPathNameByHandleW(target, final_path, 32768,
                                           FILE_NAME_NORMALIZED | VOLUME_NAME_DOS)) != 0 &&
            n < 32768;
  DWORD error = GetLastError();
  if (!ok) {
    SetLastError(error);
    return gesher::FailWithLastError();
  }
  bool directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  if ((mask & IN_ONLYDIR) != 0 && !directory) return errno = ENOTDIR, -1;
  unsigned long long index = gesher::FileIndex(info);
  uint32_t events = mask & (IN_ALL_EVENTS | IN_ONESHOT | IN_EXCL_UNLINK);

  std::lock_guard<std::mutex> lock(in->mutex);
  for (auto& [wd, w] : in->watches) {
    if (!w->removed && w->volume == info.dwVolumeSerialNumber && w->index == index) {
      if ((mask & IN_MASK_CREATE) != 0) return errno = EEXIST, -1;
      w->mask = (mask & IN_MASK_ADD) != 0 ? w->mask | events : events;
      return wd;
    }
  }
  w->wd = in->next_wd++;
  w->mask = events;
  w->volume = info.dwVolumeSerialNumber;
  w->index = index;
  std::wstring full(final_path, n);
  if (directory) {
    w->dir = full;
  } else {
    size_t slash = full.find_last_of(L'\\');
    w->dir = full.substr(0, slash);
    w->file = full.substr(slash + 1);
  }
  w->handle = CreateFileW(w->dir.c_str(), FILE_LIST_DIRECTORY,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
  if (w->handle == INVALID_HANDLE_VALUE) return gesher::FailWithLastError();
  w->extended = gesher::ExtendedReader() != nullptr;
  if (directory) {
    if (CreateIoCompletionPort(w->handle, in->port, reinterpret_cast<ULONG_PTR>(w.get()), 0) == nullptr) {
      return gesher::FailWithLastError();
    }
  } else {
    // Unlike an IOCP-associated handle, an event-backed read's result is
    // available before the worker dequeues its wakeup.
    w->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (w->event == nullptr) return gesher::FailWithLastError();
    w->port = in->port;
    w->wait = CreateThreadpoolWait(gesher::FileReadReady, w.get(), nullptr);
    if (w->wait == nullptr) return gesher::FailWithLastError();
  }
  bool issued = in->Issue(*w);
  if (!issued && w->extended) {
    w->extended = false;  // a volume without extended information (FAT, network)
    issued = in->Issue(*w);
  }
  if (!issued) return gesher::FailWithLastError();
  int wd = w->wd;
  in->watches[wd] = std::move(w);
  return wd;
}

int inotify_rm_watch(int fd, int wd) {
  std::shared_ptr<Inotify> in = gesher::LookupInotify(fd);
  if (in == nullptr) {
    return errno = gesher::HandleFromFd(fd) == INVALID_HANDLE_VALUE ? EBADF : EINVAL, -1;
  }
  std::lock_guard<std::mutex> lock(in->mutex);
  auto it = in->watches.find(wd);
  if (it == in->watches.end() || it->second->removed) return errno = EINVAL, -1;
  in->End(*it->second);
  return 0;
}

}  // extern "C"
