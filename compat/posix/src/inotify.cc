// gesher-base: inotify over ReadDirectoryChangesW (docs/posix.md, ADR 0045).
//
// An inotify fd is, like an eventfd, a CRT fd on NUL that epoll.cc maps to
// its object; its readiness is a read-only eventfd, set while events are
// queued, so poll and epoll_wait (libutils' Looper, FileObserver,
// LinuxWatchService) wait on it like on any other fd.
//
// Each watch is an overlapped ReadDirectoryChangesW on its directory (a
// watched file: on its parent, filtered by name), completed on the inotify's
// I/O completion port, which a worker thread reads and turns into Linux
// events:
//   added -> IN_CREATE, removed -> IN_DELETE, renamed -> IN_MOVED_FROM and
//   IN_MOVED_TO with one cookie, modified -> IN_MODIFY, or IN_ATTRIB when
//   only the change time moved (metadata);
//   the watched directory gone -> IN_DELETE_SELF, IN_IGNORED;
//   a lost buffer -> IN_Q_OVERFLOW.
// Windows reports no close: IN_CLOSE_WRITE is reported once a created or
// modified file is no longer open for writing by anyone, which the worker
// checks every 50 ms by opening it with write sharing denied. IN_OPEN,
// IN_ACCESS and IN_CLOSE_NOWRITE are never reported.
#include <sys/inotify.h>

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <windows.h>

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "event_internal.h"
#include "internal.h"

namespace gesher {
namespace {

constexpr DWORD kBufferSize = 64 * 1024;
constexpr size_t kMaxQueued = 16384;  // Linux's max_queued_events
constexpr ULONG_PTR kQuit = 0;
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
  OVERLAPPED ov = {};
  std::vector<DWORD> buffer = std::vector<DWORD>(kBufferSize / sizeof(DWORD));
  bool extended = false;
  bool removed = false;  // rm'd or ended: dropped when its read completes
  bool ignored_sent = false;
  std::set<std::wstring> writing;  // names to report IN_CLOSE_WRITE for
  // Each name's last modification time and size (extended records): a change
  // that moves neither is to metadata only (IN_ATTRIB).
  std::map<std::wstring, std::pair<long long, long long>> contents;
};

}  // namespace

struct Inotify {
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<std::string> queue;  // events, in Linux's layout
  std::map<int, std::unique_ptr<Watch>> watches;
  int next_wd = 1;
  uint32_t next_cookie = 1;
  HANDLE port = nullptr;
  std::shared_ptr<EventFd> ready;
  std::thread worker;

  ~Inotify() {
    if (worker.joinable()) {
      PostQueuedCompletionStatus(port, 0, kQuit, nullptr);
      worker.join();
    }
    for (auto& [wd, w] : watches) {
      if (w->handle != INVALID_HANDLE_VALUE) CloseHandle(w->handle);
    }
    if (port != nullptr) CloseHandle(port);
  }

  // Queues an event; called with `mutex` held.
  void Emit(int wd, uint32_t mask, uint32_t cookie, const std::string& name) {
    size_t len = name.empty() ? 0 : (name.size() + 1 + 15) / 16 * 16;
    std::string event(sizeof(inotify_event) + len, '\0');
    inotify_event header = {wd, mask, cookie, static_cast<uint32_t>(len)};
    memcpy(event.data(), &header, sizeof(header));
    memcpy(event.data() + sizeof(header), name.data(), name.size());
    // Linux merges an event with the identical one before it, if unread.
    if (!queue.empty() && queue.back() == event) return;
    if (queue.size() >= kMaxQueued) {
      inotify_event overflow = {-1, IN_Q_OVERFLOW, 0, 0};
      std::string o(reinterpret_cast<const char*>(&overflow), sizeof(overflow));
      if (queue.back() != o) queue.push_back(o);
      return;
    }
    queue.push_back(std::move(event));
    SetReadable(*ready, true);
    changed.notify_all();
  }

  // A watch's event, if its mask wants it; IN_ONESHOT ends the watch.
  void EmitFor(Watch& w, uint32_t mask, uint32_t cookie, const std::wstring& name) {
    if ((mask & (w.mask | kAlways)) == 0 || w.removed) return;
    Emit(w.wd, mask, cookie, Narrow(name.c_str()));
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
      CancelIoEx(w.handle, &w.ov);
    }
  }

  bool Issue(Watch& w) {
    memset(&w.ov, 0, sizeof(w.ov));
    DWORD unused;
    if (w.extended) {
      return ExtendedReader()(w.handle, w.buffer.data(), kBufferSize, FALSE, kNotifyFilter, &unused,
                              &w.ov, nullptr, kReadDirectoryNotifyExtendedInformation);
    }
    return ReadDirectoryChangesW(w.handle, w.buffer.data(), kBufferSize, FALSE, kNotifyFilter,
                                 &unused, &w.ov, nullptr);
  }

  void Changes(Watch& w, DWORD bytes);
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

}  // namespace

// One completed read's records.
void Inotify::Changes(Watch& w, DWORD bytes) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(w.buffer.data());
  const unsigned char* end = p + bytes;
  uint32_t cookie = 0;
  for (;;) {
    if (p + sizeof(FILE_NOTIFY_INFORMATION) > end) break;
    DWORD next, action, attributes = 0, name_length;
    const WCHAR* name_ptr;
    bool attrib_only = false, known_attributes = false;
    if (w.extended) {
      const NotifyExtended* e = reinterpret_cast<const NotifyExtended*>(p);
      next = e->next;
      action = e->action;
      attributes = e->attributes;
      known_attributes = true;
      name_length = e->name_length;
      name_ptr = e->name;
      std::wstring key(e->name, e->name_length / sizeof(WCHAR));
      std::pair<long long, long long> now(e->modification_time.QuadPart, e->file_size.QuadPart);
      auto seen = w.contents.find(key);
      if (action == FILE_ACTION_MODIFIED && seen != w.contents.end()) {
        attrib_only = seen->second == now;  // chmod, times: content unchanged
      }
      if (action == FILE_ACTION_REMOVED || action == FILE_ACTION_RENAMED_OLD_NAME) {
        w.contents.erase(key);
      } else {
        w.contents[key] = now;
      }
    } else {
      const FILE_NOTIFY_INFORMATION* e = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
      next = e->NextEntryOffset;
      action = e->Action;
      name_length = e->FileNameLength;
      name_ptr = e->FileName;
    }
    std::wstring name(name_ptr, name_length / sizeof(WCHAR));
    if (!known_attributes && action != FILE_ACTION_REMOVED &&
        action != FILE_ACTION_RENAMED_OLD_NAME && IsDirectory(Join(w.dir, name))) {
      attributes = FILE_ATTRIBUTE_DIRECTORY;
    }
    uint32_t isdir = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? IN_ISDIR : 0;
    if (!w.file.empty()) {
      // A watched file: only its own name, and events without a name.
      if (CompareStringOrdinal(name.c_str(), -1, w.file.c_str(), -1, TRUE) == CSTR_EQUAL) {
        switch (action) {
          case FILE_ACTION_MODIFIED:
            EmitFor(w, attrib_only ? IN_ATTRIB : IN_MODIFY, 0, L"");
            if (!attrib_only) w.writing.insert(L"");
            break;
          case FILE_ACTION_REMOVED:
            EmitFor(w, IN_DELETE_SELF, 0, L"");
            End(w);
            break;
          case FILE_ACTION_RENAMED_OLD_NAME:
            EmitFor(w, IN_MOVE_SELF, 0, L"");
            End(w);  // the name now belongs to no file this watch knows
            break;
          default:
            break;
        }
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
  }
}

// IN_CLOSE_WRITE for the files no one has open for writing any more.
void Inotify::ProbeWriters() {
  std::vector<std::pair<int, std::wstring>> probes;
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& [wd, w] : watches) {
      if (w->removed) continue;
      if ((w->mask & IN_CLOSE_WRITE) == 0) {
        w->writing.clear();
        continue;
      }
      for (const std::wstring& name : w->writing) probes.emplace_back(wd, name);
    }
  }
  for (auto& [wd, name] : probes) {
    std::wstring path;
    {
      std::lock_guard<std::mutex> lock(mutex);
      auto it = watches.find(wd);
      if (it == watches.end()) continue;
      path = name.empty() ? Join(it->second->dir, it->second->file) : Join(it->second->dir, name);
    }
    // Denying write sharing fails while any handle can write.
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD error = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) continue;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = watches.find(wd);
    if (it == watches.end() || it->second->writing.erase(name) == 0) continue;
    if (error == 0) EmitFor(*it->second, IN_CLOSE_WRITE, 0, name);
  }
}

void Inotify::Run() {
  bool quitting = false;
  for (;;) {
    bool probing;
    {
      std::lock_guard<std::mutex> lock(mutex);
      probing = false;
      for (auto& [wd, w] : watches) probing |= !w->writing.empty() && !w->removed;
    }
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    OVERLAPPED* ov = nullptr;
    BOOL ok = GetQueuedCompletionStatus(port, &bytes, &key, &ov, probing ? 50 : INFINITE);
    DWORD error = ok ? 0 : GetLastError();
    if (ov == nullptr) {
      if (!ok && error == WAIT_TIMEOUT) {
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
          CancelIoEx(w->handle, &w->ov);
        }
      }
      if (watches.empty()) return;
      continue;
    }
    std::unique_lock<std::mutex> lock(mutex);
    Watch* w = reinterpret_cast<Watch*>(key);
    if (w->removed) {
      // Its read has ended: drop it.
      int wd = w->wd;
      CloseHandle(w->handle);
      watches.erase(wd);
      if (quitting && watches.empty()) return;
      continue;
    }
    if (ok && bytes > 0) {
      Changes(*w, bytes);
    } else if (ok && IsDirectory(w->dir)) {
      Emit(-1, IN_Q_OVERFLOW, 0, "");  // more changes than the buffer held
    }
    // The directory is gone (deleted, or its volume) when the read fails.
    if (!w->removed && (!ok || (bytes == 0 && !IsDirectory(w->dir)) || !Issue(*w))) {
      if (w->file.empty()) EmitFor(*w, IN_DELETE_SELF, 0, L"");
      End(*w);
    }
    if (w->removed) {
      // Ended (IN_ONESHOT, its file gone, its directory gone) with no read
      // pending: drop it now.
      int wd = w->wd;
      CloseHandle(w->handle);
      watches.erase(wd);
      continue;
    }
    lock.unlock();
    ProbeWriters();
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
  // Every fd gesher-base reserves is uninheritable: IN_CLOEXEC holds always.
  return gesher::RegisterInotify(std::move(in), in->ready);
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
  HANDLE target = CreateFileW(windows.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, open_flags, nullptr);
  if (target == INVALID_HANDLE_VALUE) return gesher::FailWithLastError();
  BY_HANDLE_FILE_INFORMATION info;
  wchar_t final_path[32768];
  DWORD n = 0;
  bool ok = GetFileInformationByHandle(target, &info) &&
            (n = GetFinalPathNameByHandleW(target, final_path, 32768,
                                           FILE_NAME_NORMALIZED | VOLUME_NAME_DOS)) != 0 &&
            n < 32768;
  DWORD error = GetLastError();
  CloseHandle(target);
  if (!ok) {
    SetLastError(error);
    return gesher::FailWithLastError();
  }
  bool directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  if ((mask & IN_ONLYDIR) != 0 && !directory) return errno = ENOTDIR, -1;
  unsigned long long index =
      (static_cast<unsigned long long>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
  uint32_t events = mask & (IN_ALL_EVENTS | IN_ONESHOT | IN_EXCL_UNLINK);

  std::lock_guard<std::mutex> lock(in->mutex);
  for (auto& [wd, w] : in->watches) {
    if (!w->removed && w->volume == info.dwVolumeSerialNumber && w->index == index) {
      if ((mask & IN_MASK_CREATE) != 0) return errno = EEXIST, -1;
      w->mask = (mask & IN_MASK_ADD) != 0 ? w->mask | events : events;
      return wd;
    }
  }
  auto w = std::make_unique<Watch>();
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
  if (CreateIoCompletionPort(w->handle, in->port, reinterpret_cast<ULONG_PTR>(w.get()), 0) == nullptr) {
    int r = gesher::FailWithLastError();
    CloseHandle(w->handle);
    return r;
  }
  bool issued = in->Issue(*w);
  if (!issued && w->extended) {
    w->extended = false;  // a volume without extended information (FAT, network)
    issued = in->Issue(*w);
  }
  if (!issued) {
    int r = gesher::FailWithLastError();
    CloseHandle(w->handle);
    return r;
  }
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
