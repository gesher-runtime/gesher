// gesher-base: eventfd and epoll, as libutils' Looper uses them: an eventfd
// wakes a thread blocked in epoll_wait on it and on other fds.
//
// An eventfd is a 64-bit counter with two manual-reset Win32 events, set
// while it can be read and while it can be written, so poll and epoll_wait
// wait on them with the real timeout; blocking reads and writes wait on a
// condition variable instead, since a writer may need more room than
// "writable" promises. An epoll is an interest list, fd -> (events, data),
// whose epoll_wait is poll() (fd.cc) over that list, woken early by an
// auto-reset event when epoll_ctl changes it.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "event_internal.h"
#include "internal.h"
#include "socket_internal.h"

namespace gesher {

struct EventFd {
  // The largest counter value; writes that would pass it block.
  static constexpr uint64_t kMax = ~uint64_t{0} - 1;

  std::mutex mutex;
  std::condition_variable changed;
  uint64_t counter = 0;
  bool semaphore = false;
  bool nonblocking = false;
  // An inotify's readiness (inotify.cc): never writable, as an inotify fd is not.
  bool read_only = false;
  HANDLE readable = nullptr;  // set while counter > 0
  HANDLE writable = nullptr;  // set while counter < kMax, as Linux's POLLOUT
  bool readable_set = false;
  bool writable_set = false;

  ~EventFd() {
    if (readable != nullptr) CloseHandle(readable);
    if (writable != nullptr) CloseHandle(writable);
  }

  // Brings the events in line with the counter and wakes blocked readers
  // and writers. Called with `mutex` held.
  void Update() {
    bool r = counter > 0, w = counter < kMax;
    if (r != readable_set) {
      if (r) SetEvent(readable); else ResetEvent(readable);
      readable_set = r;
    }
    if (w != writable_set) {
      if (w) SetEvent(writable); else ResetEvent(writable);
      writable_set = w;
    }
    changed.notify_all();
  }
};

namespace {

struct Entry {
  uint32_t events;
  epoll_data_t data;
  uint64_t id;  // tells a re-added fd from the entry a wait started with
  bool armed;   // false once an EPOLLONESHOT entry fired, until EPOLL_CTL_MOD
};

std::atomic<size_t> epoll_count{0};

struct Epoll {
  std::mutex mutex;
  std::map<int, Entry> entries;
  uint64_t next_id = 1;
  HANDLE changed = nullptr;  // auto-reset; epoll_ctl sets it to wake waiters

  Epoll() { epoll_count.fetch_add(1, std::memory_order_release); }
  ~Epoll() {
    if (changed != nullptr) CloseHandle(changed);
    epoll_count.fetch_sub(1, std::memory_order_release);
  }
};

// fd -> object, for each kind. An inotify's fd has its readiness as its
// eventfd too, so poll and epoll_wait see it as one.
struct Object {
  std::shared_ptr<EventFd> eventfd;
  std::shared_ptr<Epoll> epoll;
  std::shared_ptr<Inotify> inotify;
};

std::mutex objects_mutex;
std::unordered_map<int, Object> objects;
// Lets processes without eventfds or epolls skip the lock.
std::atomic<size_t> object_count{0};

// Every live epoll, for ForgetEpollEntries; expired ones are pruned there.
std::mutex epolls_mutex;
std::vector<std::weak_ptr<Epoll>> epolls;

bool Lookup(int fd, Object* object) {
  if (object_count.load(std::memory_order_acquire) == 0) return false;
  std::lock_guard<std::mutex> lock(objects_mutex);
  auto it = objects.find(fd);
  if (it == objects.end()) return false;
  *object = it->second;
  return true;
}

std::shared_ptr<Epoll> LookupEpoll(int fd) {
  Object object;
  return Lookup(fd, &object) ? object.epoll : nullptr;
}

void Insert(int fd, Object object) {
  Object old;  // released after the lock, as it may be the last reference
  std::lock_guard<std::mutex> lock(objects_mutex);
  auto [it, inserted] = objects.try_emplace(fd);
  if (inserted) object_count.fetch_add(1, std::memory_order_release);
  old = std::move(it->second);
  it->second = std::move(object);
}

// Registers a new object under a fresh fd on NUL.
int Register(Object object) {
  int fd = ReserveFd();
  if (fd >= 0) Insert(fd, std::move(object));
  return fd;
}

int ReadEventFd(EventFd& e, void* buf, unsigned int count) {
  if (count < sizeof(uint64_t)) return errno = EINVAL, -1;
  std::unique_lock<std::mutex> lock(e.mutex);
  while (e.counter == 0) {
    if (e.nonblocking) return errno = EAGAIN, -1;
    e.changed.wait(lock);
  }
  uint64_t value = e.semaphore ? 1 : e.counter;
  e.counter -= value;
  e.Update();
  memcpy(buf, &value, sizeof(value));
  return sizeof(value);
}

int WriteEventFd(EventFd& e, const void* buf, unsigned int count) {
  if (count < sizeof(uint64_t)) return errno = EINVAL, -1;
  uint64_t value;
  memcpy(&value, buf, sizeof(value));
  if (value == ~uint64_t{0}) return errno = EINVAL, -1;
  std::unique_lock<std::mutex> lock(e.mutex);
  while (EventFd::kMax - e.counter < value) {
    if (e.nonblocking) return errno = EAGAIN, -1;
    e.changed.wait(lock);
  }
  e.counter += value;
  e.Update();
  return sizeof(value);
}

// epoll's event bits below EPOLLEXCLUSIVE are poll's.
constexpr uint32_t kPollable = EPOLLIN | EPOLLPRI | EPOLLOUT | EPOLLRDNORM | EPOLLRDBAND |
                               EPOLLWRNORM | EPOLLWRBAND | EPOLLMSG | EPOLLRDHUP;

// What epoll reports for poll's `revents`: the requested events, and errors
// and hangups always.
uint32_t ToEpoll(short revents, uint32_t events) {
  uint32_t r = static_cast<uint16_t>(revents) & ((events & kPollable) | EPOLLERR | EPOLLHUP);
  if ((revents & POLLHUP) != 0 && (events & EPOLLRDHUP) != 0) r |= EPOLLRDHUP;
  return r;
}

// Regular files and directories cannot be polled on Linux, so epoll refuses
// them with EPERM. Pipes, sockets, eventfds and character devices can.
bool Pollable(int fd) {
  SocketFd socket;
  return LookupSocket(fd, &socket) || GetFileType(HandleFromFd(fd)) != FILE_TYPE_DISK;
}

}  // namespace

std::shared_ptr<EventFd> LookupEventFd(int fd) {
  Object object;
  return Lookup(fd, &object) ? object.eventfd : nullptr;
}

short PollEventFd(EventFd& e, short events, HANDLE* wake) {
  std::lock_guard<std::mutex> lock(e.mutex);
  short ready = 0;
  if (e.counter > 0) ready |= events & (POLLIN | POLLRDNORM);
  if (!e.read_only && e.counter < EventFd::kMax) ready |= events & (POLLOUT | POLLWRNORM);
  if (wake != nullptr) {
    // One of the two is always ready, so at most one needs waiting for.
    *wake = ready != 0                               ? nullptr
            : (events & (POLLIN | POLLRDNORM)) != 0  ? e.readable
            : (events & (POLLOUT | POLLWRNORM)) != 0 && !e.read_only ? e.writable
                                                     : nullptr;
  }
  return ready;
}

bool IsNonBlocking(EventFd& e) {
  std::lock_guard<std::mutex> lock(e.mutex);
  return e.nonblocking;
}

void SetNonBlocking(EventFd& e, bool on) {
  std::lock_guard<std::mutex> lock(e.mutex);
  e.nonblocking = on;
  e.changed.notify_all();  // blocked callers keep waiting, as on Linux
}

bool ReadEventObject(int fd, void* buf, unsigned int count, int* result) {
  Object object;
  if (!Lookup(fd, &object)) return false;
  *result = object.inotify   ? ReadInotify(*object.inotify, buf, count)
            : object.eventfd ? ReadEventFd(*object.eventfd, buf, count)
                             : (errno = EINVAL, -1);
  return true;
}

bool WriteEventObject(int fd, const void* buf, unsigned int count, int* result) {
  Object object;
  if (!Lookup(fd, &object)) return false;
  *result = object.eventfd && !object.inotify ? WriteEventFd(*object.eventfd, buf, count)
                                              : (errno = EINVAL, -1);
  return true;
}

bool ForgetEventObject(int fd) {
  if (object_count.load(std::memory_order_acquire) == 0) return false;
  Object old;  // released after the lock
  std::lock_guard<std::mutex> lock(objects_mutex);
  auto it = objects.find(fd);
  if (it == objects.end()) return false;
  old = std::move(it->second);
  objects.erase(it);
  object_count.fetch_sub(1, std::memory_order_release);
  return true;
}

std::shared_ptr<EventFd> NewReadiness(bool nonblocking) {
  auto e = std::make_shared<EventFd>();
  e->read_only = true;
  e->nonblocking = nonblocking;
  e->readable = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  e->writable = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (e->readable == nullptr || e->writable == nullptr) return nullptr;
  std::lock_guard<std::mutex> lock(e->mutex);
  e->Update();
  return e;
}

void SetReadable(EventFd& e, bool readable) {
  std::lock_guard<std::mutex> lock(e.mutex);
  e.counter = readable ? 1 : 0;
  e.Update();
}

std::shared_ptr<Inotify> LookupInotify(int fd) {
  Object object;
  return Lookup(fd, &object) ? object.inotify : nullptr;
}

int RegisterInotify(std::shared_ptr<Inotify> inotify, std::shared_ptr<EventFd> readiness) {
  return Register({std::move(readiness), nullptr, std::move(inotify)});
}

void DupEventObject(int fd, int fd2) {
  Object object;
  if (Lookup(fd, &object)) Insert(fd2, std::move(object));
}

// Linux drops an entry when the last fd for its open file closes; here it
// goes when its own fd number does, so a number reused by a later open is
// never reported for the old registration.
void ForgetEpollEntries(int fd) {
  if (epoll_count.load(std::memory_order_acquire) == 0) return;
  std::vector<std::shared_ptr<Epoll>> live;
  {
    std::lock_guard<std::mutex> lock(epolls_mutex);
    size_t kept = 0;
    for (auto& weak : epolls) {
      if (auto ep = weak.lock()) {
        live.push_back(std::move(ep));
        epolls[kept++] = weak;
      }
    }
    epolls.resize(kept);
  }
  for (auto& ep : live) {
    std::lock_guard<std::mutex> lock(ep->mutex);
    ep->entries.erase(fd);
  }
}

}  // namespace gesher

using gesher::Epoll;
using gesher::EventFd;

extern "C" {

// --- eventfd ----------------------------------------------------------------

// Every fd gesher-base reserves is uninheritable, so EFD_CLOEXEC holds
// always; a child process could not use the counter anyway.
int eventfd(unsigned int initval, int flags) {
  if ((flags & ~(EFD_SEMAPHORE | EFD_CLOEXEC | EFD_NONBLOCK)) != 0) return errno = EINVAL, -1;
  auto e = std::make_shared<EventFd>();
  e->counter = initval;
  e->semaphore = (flags & EFD_SEMAPHORE) != 0;
  e->nonblocking = (flags & EFD_NONBLOCK) != 0;
  e->readable = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  e->writable = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (e->readable == nullptr || e->writable == nullptr) return gesher::FailWithLastError();
  e->Update();
  return gesher::Register({std::move(e), nullptr, nullptr});
}

int eventfd_read(int fd, eventfd_t* value) {
  return read(fd, value, sizeof(*value)) == sizeof(*value) ? 0 : -1;
}

int eventfd_write(int fd, eventfd_t value) {
  return write(fd, &value, sizeof(value)) == sizeof(value) ? 0 : -1;
}

// --- epoll ------------------------------------------------------------------

int epoll_create1(int flags) {
  if ((flags & ~EPOLL_CLOEXEC) != 0) return errno = EINVAL, -1;  // always CLOEXEC, as eventfd
  auto ep = std::make_shared<Epoll>();
  ep->changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (ep->changed == nullptr) return gesher::FailWithLastError();
  {
    std::lock_guard<std::mutex> lock(gesher::epolls_mutex);
    gesher::epolls.push_back(ep);
  }
  return gesher::Register({nullptr, std::move(ep), nullptr});
}

int epoll_create(int size) { return size <= 0 ? (errno = EINVAL, -1) : epoll_create1(0); }

// EPOLLET is accepted and treated as level-triggered: every epoll_wait
// reports what is ready, so a caller that drains the fd, as edge-triggered
// callers must, sees the same events, plus at worst a spurious repeat.
int epoll_ctl(int epfd, int op, int fd, struct epoll_event* event) {
  if (gesher::HandleFromFd(epfd) == INVALID_HANDLE_VALUE ||
      gesher::HandleFromFd(fd) == INVALID_HANDLE_VALUE) {
    return errno = EBADF, -1;
  }
  std::shared_ptr<Epoll> ep = gesher::LookupEpoll(epfd);
  if (ep == nullptr || fd == epfd) return errno = EINVAL, -1;
  if (op != EPOLL_CTL_ADD && op != EPOLL_CTL_MOD && op != EPOLL_CTL_DEL) return errno = EINVAL, -1;
  if (op != EPOLL_CTL_DEL && event == nullptr) return errno = EFAULT, -1;
  // Nested epolls, which Linux allows, are not supported.
  if (gesher::LookupEpoll(fd) != nullptr) return errno = EINVAL, -1;
  if (!gesher::Pollable(fd)) return errno = EPERM, -1;
  uint32_t events = event != nullptr ? event->events : 0;
  if (op == EPOLL_CTL_MOD && (events & EPOLLEXCLUSIVE) != 0) return errno = EINVAL, -1;
  {
    std::lock_guard<std::mutex> lock(ep->mutex);
    auto it = ep->entries.find(fd);
    switch (op) {
      case EPOLL_CTL_ADD:
        if (it != ep->entries.end()) return errno = EEXIST, -1;
        ep->entries[fd] = {events, event->data, ep->next_id++, true};
        break;
      case EPOLL_CTL_MOD:
        if (it == ep->entries.end()) return errno = ENOENT, -1;
        it->second = {events, event->data, ep->next_id++, true};
        break;
      case EPOLL_CTL_DEL:
        if (it == ep->entries.end()) return errno = ENOENT, -1;
        ep->entries.erase(it);
        break;
    }
  }
  SetEvent(ep->changed);  // a waiter takes the new list
  return 0;
}

int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout) {
  if (maxevents <= 0) return errno = EINVAL, -1;
  std::shared_ptr<Epoll> ep = gesher::LookupEpoll(epfd);
  if (ep == nullptr) {
    return errno = gesher::HandleFromFd(epfd) == INVALID_HANDLE_VALUE ? EBADF : EINVAL, -1;
  }
  ULONGLONG deadline = gesher::DeadlineAfter(timeout);
  std::vector<pollfd> fds;
  std::vector<uint64_t> ids;
  for (;;) {
    fds.clear();
    ids.clear();
    {
      std::lock_guard<std::mutex> lock(ep->mutex);
      for (const auto& [fd, entry] : ep->entries) {
        if (!entry.armed) continue;
        fds.push_back({fd, static_cast<short>(entry.events & gesher::kPollable), 0});
        ids.push_back(entry.id);
      }
    }
    int wait_ms = -1;
    if (timeout >= 0) {
      wait_ms = gesher::MillisUntil(deadline);
    }
    bool interrupted = false;
    int ready = gesher::PollFds(fds.data(), fds.size(), wait_ms, ep->changed, &interrupted);
    if (ready < 0) return -1;

    int count = 0;
    if (ready > 0) {
      std::lock_guard<std::mutex> lock(ep->mutex);
      for (size_t i = 0; i < fds.size() && count < maxevents; ++i) {
        if (fds[i].revents == 0) continue;
        auto it = ep->entries.find(fds[i].fd);
        // Gone, replaced or fired (EPOLLONESHOT) since the list was taken.
        if (it == ep->entries.end() || it->second.id != ids[i] || !it->second.armed) continue;
        if ((fds[i].revents & POLLNVAL) != 0) {
          // Closed behind gesher-base's back (_close): Linux would have
          // dropped the entry with the file.
          ep->entries.erase(it);
          continue;
        }
        uint32_t r = gesher::ToEpoll(fds[i].revents, it->second.events);
        if (r == 0) continue;
        events[count].events = r;
        events[count].data = it->second.data;
        ++count;
        if ((it->second.events & EPOLLONESHOT) != 0) it->second.armed = false;
      }
    }
    if (count > 0) return count;
    if (timeout == 0 || (timeout > 0 && gesher::MonotonicMicros() >= deadline)) return 0;
    // The list changed, or what was ready is no longer reportable: again.
  }
}

// Signals are not delivered asynchronously here, so the mask has nothing to
// change while waiting (as ppoll, fd.cc).
int epoll_pwait(int epfd, struct epoll_event* events, int maxevents, int timeout,
                const sigset_t*) {
  return epoll_wait(epfd, events, maxevents, timeout);
}

}  // extern "C"
