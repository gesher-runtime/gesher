// gesher-base: eventfd and epoll, as libutils' Looper uses them: an eventfd
// wakes a thread blocked in epoll_wait on it and on other fds.
//
// An eventfd is a 64-bit counter with two manual-reset Win32 events, set
// while it can be read and while it can be written, so poll and epoll_wait
// wait on them with the real timeout; blocking reads and writes wait on a
// condition variable instead, since a writer may need more room than
// "writable" promises. An epoll is an interest list, fd -> (events, data),
// whose epoll_wait is poll() (fd.cc) over that list. Every waiter registers
// its own interrupt event before sampling the list, so changes cannot be
// consumed by another waiter or lost before generic poll-wake registration.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <windows.h>

#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#include "event_internal.h"
#include "internal.h"
#include "native_fence_internal.h"
#include "poll_scratch.h"
#include "socket_internal.h"
#include "record_lock_descriptors.h"

namespace gesher {

// A producer only takes this channel lock, never the epoll state lock.
// Registration and retirement protect the Win32 handle lifetime here too.
struct EpollWakeChannels {
  std::mutex mutex;
  std::vector<HANDLE> waiters;

  void Notify() {
    std::lock_guard<std::mutex> lock(mutex);
    for (HANDLE waiter : waiters) SetEvent(waiter);
  }
};

struct EventFdEdgeWatch {
  uint32_t events;  // protected by the owning EventFd mutex; zero after ONESHOT
  // Level-triggered entries only need a wake when the source becomes ready;
  // repeated writes to an already readable counter do not change the level.
  bool level = false;
  std::shared_ptr<EpollWakeChannels> wake;
};

struct EventFd {
  // The largest counter value; writes that would pass it block.
  static constexpr uint64_t kMax = ~uint64_t{0} - 1;

  std::mutex mutex;
  std::condition_variable changed;
  uint64_t counter = 0;
  bool semaphore = false;
  bool nonblocking = false;
  // Inotify/timerfd readiness is never writable.
  bool read_only = false;
  HANDLE readable = nullptr;  // set while counter > 0
  HANDLE writable = nullptr;  // set while counter < kMax, as Linux's POLLOUT
  bool readable_set = false;
  bool writable_set = false;
  uint64_t read_generation = 0;
  uint64_t write_generation = 0;
  std::vector<std::weak_ptr<EventFdEdgeWatch>> edge_watches;

  ~EventFd() {
    if (readable != nullptr) CloseHandle(readable);
    if (writable != nullptr) CloseHandle(writable);
  }

  // Brings the events in line with the counter and wakes blocked readers
  // and writers. Called with `mutex` held.
  void Update(uint32_t notification = 0) {
    bool r = counter > 0, w = counter < kMax;
    // A poll waiting in WSAPoll, next to sockets, sees no event.
    const uint32_t became_ready = (r && !readable_set ? uint32_t{EPOLLIN} : 0) |
                                 (w && !writable_set && !read_only ? uint32_t{EPOLLOUT} : 0);
    if (r != readable_set) {
      if (r) SetEvent(readable); else ResetEvent(readable);
      readable_set = r;
    }
    if (w != writable_set) {
      if (w) SetEvent(writable); else ResetEvent(writable);
      writable_set = w;
    }
    // Linux eventfd publishes a read notification on every successful write,
    // including zero writes and writes while already readable. A successful
    // read similarly publishes a write notification. Levels alone lose those
    // notifications, and lose a drain/refill entirely between epoll waits.
    // Read-only producers publish when they enqueue a new inotify record or
    // expire an armed timer; an unread periodic timer is not a new producer.
    bool edge_notified = false;
    if (notification != 0) {
      if ((notification & EPOLLIN) != 0) ++read_generation;
      if ((notification & EPOLLOUT) != 0) ++write_generation;
    }
    const uint32_t wake_notification = notification | became_ready;
    if (wake_notification != 0) {
      size_t kept = 0;
      for (auto& weak : edge_watches) {
        if (auto watch = weak.lock()) {
          edge_watches[kept++] = weak;
          const uint32_t match = watch->level ? became_ready : wake_notification;
          if ((watch->events & match) != 0) {
            watch->wake->Notify();
            edge_notified = true;
          }
        }
      }
      edge_watches.resize(kept);
    }
    if (became_ready || edge_notified) WakePollers();
    changed.notify_all();
  }
};

namespace {

struct Epoll;
struct ObjectOwner;
// The number supplied to epoll_ctl is only half of Linux's registration key.
// Weak ownership compares control blocks even after retirement, without keeping
// the watched description alive or confusing a later allocation at its address.
struct EntryKey {
  int fd;
  std::weak_ptr<ObjectOwner> owner;
  EntryKey(int descriptor = -1, std::weak_ptr<ObjectOwner> description = {})
      : fd(descriptor), owner(std::move(description)) {}
  bool operator<(const EntryKey& other) const {
    return fd < other.fd || (fd == other.fd && owner.owner_before(other.owner));
  }
  bool IsVirtual() const {
    return owner.owner_before(std::weak_ptr<ObjectOwner>{}) ||
           std::weak_ptr<ObjectOwner>{}.owner_before(owner);
  }
};

struct OwnerWatch {
  std::weak_ptr<Epoll> epoll;
  std::shared_ptr<EpollWakeChannels> wake;
  EntryKey key;
  uint64_t id;
};

struct Entry {
  uint32_t events;
  epoll_data_t data;
  uint64_t id;  // tells a re-added fd from the entry a wait started with
  bool armed;   // false once an EPOLLONESHOT entry fired, until EPOLL_CTL_MOD
  std::weak_ptr<Epoll> child;
  std::shared_ptr<OwnerWatch> owner_watch = nullptr;
  bool ordinary_eventfd = false;
  uint32_t readable_events = EPOLLIN;
  // Eventfd, timerfd and inotify publish producer notifications. Sockets,
  // pipes and nested targets retain their separate readiness path.
  std::shared_ptr<EventFd> edge_source = nullptr;
  std::shared_ptr<EventFdEdgeWatch> edge_watch = nullptr;
  std::shared_ptr<EventFd> watch_source = nullptr;
  uint64_t read_generation = 0;
  uint64_t write_generation = 0;
  bool edge_initial = true;
};

std::atomic<size_t> epoll_count{0};

struct Epoll {
  std::mutex mutex;
  std::map<EntryKey, Entry> entries;
  uint64_t next_id = 1;
  // Shared by all aliases and waiters. A descriptor number survives removal
  // without retaining an invalidated iterator or a stale Entry generation.
  EntryKey last_ready;
  std::shared_ptr<EpollWakeChannels> wake = std::make_shared<EpollWakeChannels>();

  void NotifyLocked() {
    wake->Notify();
  }

  Epoll() { epoll_count.fetch_add(1, std::memory_order_release); }
  ~Epoll() {
    epoll_count.fetch_sub(1, std::memory_order_release);
  }
};

struct EpollPollScratch {
  std::vector<pollfd> fds;
  std::vector<uint64_t> ids;
  std::vector<EntryKey> keys;
  // Only populated during nonblocking delivery, never across the actual wait.
  std::vector<std::shared_ptr<ObjectOwner>> owners;

  void Reset() {
    ClearPollScratch(owners);
    ClearPollScratch(keys);
    ClearPollScratch(ids);
    ClearPollScratch(fds);
  }
};

class EpollWaiter {
 public:
  explicit EpollWaiter(std::shared_ptr<Epoll> ep) : ep_(std::move(ep)) {
    changed_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (changed_ == nullptr) return;
    std::lock_guard<std::mutex> lock(ep_->wake->mutex);
    ep_->wake->waiters.push_back(changed_);
  }
  ~EpollWaiter() {
    if (changed_ == nullptr) return;
    {
      std::lock_guard<std::mutex> lock(ep_->wake->mutex);
      std::erase(ep_->wake->waiters, changed_);
    }
    CloseHandle(changed_);
  }
  EpollWaiter(const EpollWaiter&) = delete;
  EpollWaiter& operator=(const EpollWaiter&) = delete;
  HANDLE changed() const { return changed_; }

 private:
  std::shared_ptr<Epoll> ep_;  // survives a concurrent close of the descriptor
  HANDLE changed_ = nullptr;
};

// Caller holds the epoll state lock. Producers only take the source and wake
// channel locks, in that order, so they never invert this state -> source order.
// Delivery resamples here instead of consuming an older waiter's snapshot.
uint32_t EventFdEdgeReady(Entry& entry, bool consume) {
  EventFd& source = *entry.edge_source;
  std::lock_guard<std::mutex> lock(source.mutex);
  // Inotify also reports RDNORM; eventfd and timerfd report canonical IN.
  const uint32_t input = entry.events & entry.readable_events;
  const uint32_t output = source.read_only ? 0 : entry.events & EPOLLOUT;
  if (!entry.edge_initial &&
      (input == 0 || entry.read_generation == source.read_generation) &&
      (output == 0 || entry.write_generation == source.write_generation)) return 0;
  uint32_t ready = (source.counter > 0 ? input : 0) |
                   (source.counter < EventFd::kMax ? output : 0);
  if (ready != 0 && consume) {
    entry.read_generation = source.read_generation;
    entry.write_generation = source.write_generation;
    entry.edge_initial = false;
    if ((entry.events & EPOLLONESHOT) != 0) entry.edge_watch->events = 0;
  }
  return ready;
}

void ConfigureEventFdEntry(Entry& entry, const std::shared_ptr<EventFd>& source,
                      const std::shared_ptr<EpollWakeChannels>& wake, bool inotify) {
  entry.ordinary_eventfd = source && !source->read_only;
  entry.readable_events = EPOLLIN | (inotify ? uint32_t{EPOLLRDNORM} : 0);
  if (!source) return;
  const bool edge = (entry.events & EPOLLET) != 0;
  auto watch = std::make_shared<EventFdEdgeWatch>();
  // Edge entries match the bits their file reports (inotify adds RDNORM).
  // Level entries match the generic level path, which reports IN and RDNORM
  // for every read-only readiness and only canonical IN for an eventfd.
  const uint32_t input = edge ? entry.readable_events
                              : EPOLLIN | (source->read_only ? uint32_t{EPOLLRDNORM} : 0);
  watch->events = ((entry.events & input) != 0 ? uint32_t{EPOLLIN} : 0) |
                  (source->read_only ? 0 : entry.events & EPOLLOUT);
  // Eventfd, timerfd and inotify all publish producer notifications, so every
  // edge entry uses its source's generations; level entries wake on readiness.
  watch->level = !edge;
  watch->wake = wake;
  std::lock_guard<std::mutex> lock(source->mutex);
  std::erase_if(source->edge_watches, [](const auto& weak) { return weak.expired(); });
  source->edge_watches.push_back(watch);
  if (edge) entry.edge_source = source;
  entry.edge_watch = std::move(watch);
  entry.watch_source = source;
}

void DisarmEntry(Entry& entry) {
  entry.armed = false;
  if (entry.watch_source) {
    std::lock_guard<std::mutex> lock(entry.watch_source->mutex);
    entry.edge_watch->events = 0;
  }
}

// fd -> object, for each kind. Inotify and timerfd use a read-only EventFd
// for readiness, so poll and epoll_wait can wait on them too.
struct Object {
  std::shared_ptr<EventFd> eventfd;
  std::shared_ptr<Epoll> epoll;
  std::shared_ptr<Inotify> inotify;
  std::shared_ptr<TimerFd> timer;
};

struct ObjectOwner {
  Object object;
  // Registry aliases and subscriptions are protected by objects_mutex.
  std::set<int> aliases;
  std::vector<std::weak_ptr<OwnerWatch>> watches;
  explicit ObjectOwner(Object value) : object(std::move(value)) {}
  ~ObjectOwner();
};

std::mutex objects_mutex;
std::unordered_map<int, std::shared_ptr<ObjectOwner>> objects;
// Lets processes without eventfds or epolls skip the lock.
std::atomic<size_t> object_count{0};

// Every live epoll, for ForgetEpollEntries; expired ones are pruned there.
std::mutex epolls_mutex;
std::vector<std::weak_ptr<Epoll>> epolls;
// Serialize graph changes, including opposing ADDs from separate threads.
// Readiness sampling never holds this mutex while it recurses into a child.
std::mutex epoll_graph_mutex;

std::shared_ptr<ObjectOwner> Lookup(int fd) {
  if (object_count.load(std::memory_order_acquire) == 0) return nullptr;
  std::lock_guard<std::mutex> lock(objects_mutex);
  auto it = objects.find(fd);
  return it == objects.end() ? nullptr : it->second;
}

std::shared_ptr<Epoll> LookupEpoll(int fd) {
  auto owner = Lookup(fd);
  return owner && owner->object.epoll
             ? std::shared_ptr<Epoll>(owner, owner->object.epoll.get()) : nullptr;
}

// No parent state lock is taken while the registry lock is held. A wake channel
// owns no file description, so releasing it cannot trigger owner retirement.
void NotifyAliasesLocked(ObjectOwner& owner) {
  size_t kept = 0;
  for (auto& weak : owner.watches) {
    if (auto watch = weak.lock()) {
      owner.watches[kept++] = weak;
      watch->wake->Notify();
    }
  }
  owner.watches.resize(kept);
  if (kept != 0) WakePollers();
}

ObjectOwner::~ObjectOwner() {
  // Epoll observations retain weak identity only. Real in-flight I/O aliases
  // this owner and may outlive the final fd, just as a Linux file reference can.
  bool changed = false;
  for (auto& weak : watches) {
    if (auto watch = weak.lock()) {
      if (auto ep = watch->epoll.lock()) {
        std::lock_guard<std::mutex> lock(ep->mutex);
        auto found = ep->entries.find(watch->key);
        if (found != ep->entries.end() && found->second.id == watch->id) {
          ep->entries.erase(found);
          ep->NotifyLocked();
          changed = true;
        }
      }
    }
  }
  if (changed) WakePollers();
  if (object.epoll) {
    // Keep the nesting census bounded when only virtual descriptors are used.
    std::lock_guard<std::mutex> lock(epolls_mutex);
    std::erase_if(epolls, [](const auto& weak) { return weak.expired(); });
  }
}

void Insert(int fd, const std::shared_ptr<ObjectOwner>& owner) {
  std::shared_ptr<ObjectOwner> old;  // retire outside the registry lock
  {
    std::lock_guard<std::mutex> lock(objects_mutex);
    auto [it, inserted] = objects.try_emplace(fd);
    if (inserted) object_count.fetch_add(1, std::memory_order_release);
    old = std::move(it->second);
    if (old) {
      old->aliases.erase(fd);
      NotifyAliasesLocked(*old);
    }
    owner->aliases.insert(fd);
    it->second = owner;
  }
}

// Registers a new object under a fresh fd on NUL.
int Register(Object object) {
  int fd = ReserveFd();
  if (fd >= 0) {
    auto owner = std::make_shared<ObjectOwner>(std::move(object));
    if (owner->object.epoll) {
      std::lock_guard<std::mutex> lock(epolls_mutex);
      epolls.emplace_back(std::shared_ptr<Epoll>(owner, owner->object.epoll.get()));
    }
    Insert(fd, owner);
  }
  return fd;
}

void ConfigureOwnerEntry(Entry& entry, const EntryKey& key,
                         const std::shared_ptr<ObjectOwner>& owner,
                         const std::shared_ptr<Epoll>& ep) {
  if (!owner) return;
  auto watch = std::make_shared<OwnerWatch>(OwnerWatch{ep, ep->wake, key, entry.id});
  std::lock_guard<std::mutex> lock(objects_mutex);
  std::erase_if(owner->watches, [](const auto& weak) { return weak.expired(); });
  owner->watches.push_back(watch);
  entry.owner_watch = std::move(watch);
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
  e.Update(EPOLLOUT);
  memcpy(buf, &value, sizeof(value));
  return sizeof(value);
}

int WriteEventFd(EventFd& e, const void* buf, unsigned int count) {
  if (e.read_only) return errno = EINVAL, -1;
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
  e.Update(EPOLLIN);
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

short EpollPollInterest(const Entry& entry) {
  if (entry.edge_source) return 0;
  uint32_t events = entry.events & kPollable;
  // Normalize only epoll's ordinary-eventfd interest. General poll and the
  // pipe/socket/read-only readiness backends retain their existing behavior.
  if (entry.ordinary_eventfd) events &= ~(EPOLLRDNORM | EPOLLWRNORM);
  return static_cast<short>(events);
}

bool SnapshotEpoll(const std::shared_ptr<Epoll>& ep, EpollPollScratch& scratch) {
  // Dropping an actual I/O reference may retire its registrations. Do that
  // before taking any epoll state lock, and do not retain it across a wait.
  scratch.owners.clear();
  scratch.keys.clear();
  scratch.ids.clear();
  scratch.fds.clear();
  bool ready = false;
  std::lock_guard<std::mutex> lock(ep->mutex);
  for (auto& [key, entry] : ep->entries) {
    if (!entry.armed) continue;
    if (entry.edge_source && EventFdEdgeReady(entry, false) != 0) ready = true;
    scratch.keys.push_back(key);
    scratch.ids.push_back(entry.id);
    scratch.fds.push_back({key.fd, EpollPollInterest(entry), 0});
  }
  return ready;
}

// Linux permits at most five epoll instances on a nesting path. Validate the
// whole affected graph, so adding above OR below an existing chain cannot
// bypass the bound. Called with graph and descriptor transactions held.
bool CanNest(const std::shared_ptr<Epoll>& parent, const std::shared_ptr<Epoll>& child) {
  std::vector<std::shared_ptr<Epoll>> live;
  {
    std::lock_guard<std::mutex> lock(epolls_mutex);
    for (const auto& weak : epolls) if (auto ep = weak.lock()) live.push_back(std::move(ep));
  }
  std::map<Epoll*, std::vector<std::shared_ptr<Epoll>>> graph;
  for (const auto& ep : live) {
    std::lock_guard<std::mutex> lock(ep->mutex);
    for (const auto& [fd, entry] : ep->entries) {
      if (auto nested = entry.child.lock()) graph[ep.get()].push_back(std::move(nested));
    }
  }
  graph[parent.get()].push_back(child);
  // A bounded walk avoids unbounded recursion even if a corrupt old graph
  // were supplied. Repeated nodes on different DAG branches are allowed.
  std::map<Epoll*, int> heights;
  auto height = [&](auto&& self, Epoll* node, int depth) -> int {
    if (depth > 4) return 5;
    auto known = heights.find(node);
    if (known != heights.end()) return known->second;
    int result = 0;
    for (const auto& next : graph[node]) {
      result = std::max(result, 1 + self(self, next.get(), depth + 1));
      if (result > 4) break;
    }
    heights[node] = result;
    return result;
  };
  for (const auto& ep : live) if (height(height, ep.get(), 0) > 4) return false;
  return true;
}

// Regular files and directories cannot be polled on Linux, so epoll refuses
// them with EPERM. Pipes, sockets, eventfds and character devices can.
bool Pollable(int fd) {
  SocketFd socket;
  return LookupSocket(fd, &socket) || GetFileType(HandleFromFd(fd)) != FILE_TYPE_DISK;
}

}  // namespace

int SampleVirtualEntries(EpollPollScratch& scratch, bool prepare_wait);

// Observe the shared epoll directly: a reused numeric alias is never authority
// for an older registration, including when the target is a nested epoll.
bool PollEpollOwned(const std::shared_ptr<Epoll>& ep, short events, short* ready) {
  if (!ep) return false;
  *ready = 0;
  if ((events & (POLLIN | POLLRDNORM)) == 0) return true;
  // Concurrent graph reversals can expose different, individually acyclic
  // snapshots to successive recursive samples. Bound that historical walk
  // without retaining graph/parent locks across a child's poll.
  thread_local Epoll* active[5] = {};
  thread_local size_t depth = 0;
  if (depth == 5 || std::find(active, active + depth, ep.get()) != active + depth) return true;
  active[depth++] = ep.get();
  struct Leave {
    size_t& depth;
    ~Leave() { --depth; }
  } leave{depth};
  PollScratchLease<EpollPollScratch> lease;
  auto& scratch = lease.get();
  auto& fds = scratch.fds;
  auto& ids = scratch.ids;
  SnapshotEpoll(ep, scratch);
  SampleVirtualEntries(scratch, true);
  if (PollFds(fds.data(), fds.size(), 0, nullptr, nullptr) < 0) return true;
  SampleVirtualEntries(scratch, false);
  std::lock_guard<std::mutex> lock(ep->mutex);
  for (size_t i = 0; i < fds.size(); ++i) {
    if (scratch.keys[i].IsVirtual() && !scratch.owners[i]) continue;
    auto found = ep->entries.find(scratch.keys[i]);
    if (found != ep->entries.end() && found->second.id == ids[i] && found->second.armed &&
        (fds[i].revents & POLLNVAL) == 0 &&
        (found->second.edge_source ? EventFdEdgeReady(found->second, false)
                                  : ToEpoll(fds[i].revents, found->second.events)) != 0) {
      *ready = events & (POLLIN | POLLRDNORM);
      break;
    }
  }
  return true;
}

bool PollEpoll(int fd, short events, short* ready) {
  return PollEpollOwned(LookupEpoll(fd), events, ready);
}

std::shared_ptr<EventFd> LookupEventFd(int fd) {
  auto owner = Lookup(fd);
  if (!owner) return nullptr;
  if (owner->object.timer) RefreshTimerFd(*owner->object.timer);
  return owner->object.eventfd
             ? std::shared_ptr<EventFd>(owner, owner->object.eventfd.get()) : nullptr;
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

int SampleVirtualEntries(EpollPollScratch& scratch, bool prepare_wait) {
  scratch.owners.clear();
  if (!prepare_wait) scratch.owners.resize(scratch.keys.size());
  int result = 0;
  for (size_t i = 0; i < scratch.keys.size(); ++i) {
    const auto& key = scratch.keys[i];
    if (!key.IsVirtual()) continue;
    auto owner = key.owner.lock();
    auto& item = scratch.fds[i];
    item.revents = 0;
    if (prepare_wait) {
      item.fd = -1;
      // Generic poll pins LookupEventFd's result for its entire wait. Giving it
      // a virtual readiness alias would let mere epoll observation keep the
      // file alive after final close. Its source subscription wakes our own
      // channel instead. Nested epolls are sampled without a retained owner
      // by generic poll and still need its pipe/socket fallback.
      if (owner && owner->object.epoll) {
        std::lock_guard<std::mutex> lock(objects_mutex);
        if (!owner->aliases.empty()) item.fd = *owner->aliases.begin();
      }
    } else {
      scratch.owners[i] = owner;
    }
    if (!owner) continue;
    const Object& object = owner->object;
    if (object.timer) RefreshTimerFd(*object.timer);
    if (object.eventfd) item.revents = PollEventFd(*object.eventfd, item.events, nullptr);
    else if (object.epoll) PollEpollOwned(object.epoll, item.events, &item.revents);
    if (item.revents != 0) result |= 1;
    // A genuine in-flight epoll operation can retain a target after all its
    // fds close. There is then no numeric poll target for its generic pipe or
    // socket children. Keep the same bounded fallback cadence as mixed poll.
    if (prepare_wait && item.fd < 0 && object.epoll && item.events != 0) result |= 2;
  }
  return result;
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
  auto owner = Lookup(fd);
  if (!owner) return false;
  const Object& object = owner->object;
  *result = object.timer     ? ReadTimerFd(*object.timer, buf, count)
            : object.inotify ? ReadInotify(*object.inotify, buf, count)
            : object.eventfd ? ReadEventFd(*object.eventfd, buf, count)
                             : (errno = EINVAL, -1);
  return true;
}

bool WriteEventObject(int fd, const void* buf, unsigned int count, int* result) {
  auto owner = Lookup(fd);
  if (!owner) return false;
  const Object& object = owner->object;
  *result = object.inotify ? (errno = EBADF, -1)
            : object.eventfd ? WriteEventFd(*object.eventfd, buf, count)
                             : (errno = EINVAL, -1);
  return true;
}

int EventObjectWriteError(int fd) {
  if (LookupNativeFence(fd)) return EINVAL;
  auto owner = Lookup(fd);
  if (!owner) return 0;
  const Object& object = owner->object;
  if (object.inotify) return EBADF;
  return object.eventfd && !object.eventfd->read_only ? 0 : EINVAL;
}

bool ForgetEventObject(int fd) {
  // Completed native fences are NUL-backed virtual objects as well. Every
  // close, dup2 replacement and CRT stream retirement reaches this point.
  ForgetNativeFence(fd);
  if (object_count.load(std::memory_order_acquire) == 0) return false;
  std::shared_ptr<ObjectOwner> old;  // final-reference cleanup is outside the lock
  {
    std::lock_guard<std::mutex> lock(objects_mutex);
    auto it = objects.find(fd);
    if (it == objects.end()) return false;
    old = std::move(it->second);
    old->aliases.erase(fd);
    NotifyAliasesLocked(*old);
    objects.erase(it);
    object_count.fetch_sub(1, std::memory_order_release);
  }
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
  e.Update(readable ? uint32_t{EPOLLIN} : 0);
}

std::shared_ptr<Inotify> LookupInotify(int fd) {
  auto owner = Lookup(fd);
  return owner && owner->object.inotify
             ? std::shared_ptr<Inotify>(owner, owner->object.inotify.get()) : nullptr;
}

int RegisterInotify(std::shared_ptr<Inotify> inotify, std::shared_ptr<EventFd> readiness) {
  return Register({std::move(readiness), nullptr, std::move(inotify), nullptr});
}

std::shared_ptr<TimerFd> LookupTimerFd(int fd) {
  auto owner = Lookup(fd);
  return owner && owner->object.timer
             ? std::shared_ptr<TimerFd>(owner, owner->object.timer.get()) : nullptr;
}

int RegisterTimerFd(std::shared_ptr<TimerFd> timer, std::shared_ptr<EventFd> readiness) {
  return Register({std::move(readiness), nullptr, nullptr, std::move(timer)});
}

void DupEventObject(int fd, int fd2) {
  if (auto owner = Lookup(fd)) Insert(fd2, owner);
}

// Virtual registrations retire with their shared owner, independently of the
// original number. Other descriptor backends retain their existing retirement.
void ForgetEpollEntries(int fd) {
  if (Lookup(fd)) return;
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
    if (ep->entries.erase(EntryKey(fd)) != 0) ep->NotifyLocked();
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
  return gesher::Register({std::move(e), nullptr, nullptr, nullptr});
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
  return gesher::Register({nullptr, std::move(ep), nullptr, nullptr});
}

int epoll_create(int size) { return size <= 0 ? (errno = EINVAL, -1) : epoll_create1(0); }

// Eventfds, timerfds and inotify publish edge notifications. Other descriptor
// backends still use level readiness; their edge/exclusive contracts remain open.
int epoll_ctl(int epfd, int op, int fd, struct epoll_event* event) {
  gesher::record_locks::DescriptorGuard descriptors;
  if (gesher::HandleFromFd(epfd) == INVALID_HANDLE_VALUE ||
      gesher::HandleFromFd(fd) == INVALID_HANDLE_VALUE) {
    return errno = EBADF, -1;
  }
  std::shared_ptr<Epoll> ep = gesher::LookupEpoll(epfd);
  auto child = gesher::LookupEpoll(fd);
  if (ep == nullptr || fd == epfd || child == ep) return errno = EINVAL, -1;
  if (op != EPOLL_CTL_ADD && op != EPOLL_CTL_MOD && op != EPOLL_CTL_DEL) return errno = EINVAL, -1;
  if (op != EPOLL_CTL_DEL && event == nullptr) return errno = EFAULT, -1;
  if (!gesher::Pollable(fd)) return errno = EPERM, -1;
  uint32_t events = event != nullptr ? event->events : 0;
  if (op == EPOLL_CTL_MOD && (events & EPOLLEXCLUSIVE) != 0) return errno = EINVAL, -1;
  if (child && (events & EPOLLEXCLUSIVE) != 0 && op != EPOLL_CTL_DEL) return errno = EINVAL, -1;
  auto owner = gesher::Lookup(fd);
  gesher::EntryKey key(fd, owner);
  std::lock_guard<std::mutex> graph(gesher::epoll_graph_mutex);
  if (op == EPOLL_CTL_ADD && child) {
    {
      std::lock_guard<std::mutex> lock(ep->mutex);
      if (ep->entries.count(key) != 0) return errno = EEXIST, -1;
    }
    if (!gesher::CanNest(ep, child)) return errno = ELOOP, -1;
  }
  {
    std::lock_guard<std::mutex> lock(ep->mutex);
    auto it = ep->entries.find(key);
    switch (op) {
      case EPOLL_CTL_ADD:
        if (it != ep->entries.end()) return errno = EEXIST, -1;
        ep->entries[key] = {events, event->data, ep->next_id++, true, child};
        gesher::ConfigureOwnerEntry(ep->entries[key], key, owner, ep);
        gesher::ConfigureEventFdEntry(ep->entries[key], owner ? owner->object.eventfd : nullptr,
                                      ep->wake, owner && owner->object.inotify != nullptr);
        break;
      case EPOLL_CTL_MOD:
        if (it == ep->entries.end()) return errno = ENOENT, -1;
        if ((it->second.events & EPOLLEXCLUSIVE) != 0) return errno = EINVAL, -1;
        it->second = {events, event->data, ep->next_id++, true, child};
        gesher::ConfigureOwnerEntry(it->second, key, owner, ep);
        gesher::ConfigureEventFdEntry(it->second, owner ? owner->object.eventfd : nullptr,
                                      ep->wake, owner && owner->object.inotify != nullptr);
        break;
      case EPOLL_CTL_DEL:
        if (it == ep->entries.end()) return errno = ENOENT, -1;
        ep->entries.erase(it);
        break;
    }
    ep->NotifyLocked();
  }
  gesher::WakePollers();  // nested epoll/poll and waiters in WSAPoll too
  return 0;
}

static int EpollWaitImpl(int epfd, struct epoll_event* events, int maxevents, int timeout,
                         void (*observer)(void*), void* context) {
  if (maxevents <= 0) return errno = EINVAL, -1;
  std::shared_ptr<Epoll> ep = gesher::LookupEpoll(epfd);
  if (ep == nullptr) {
    return errno = gesher::HandleFromFd(epfd) == INVALID_HANDLE_VALUE ? EBADF : EINVAL, -1;
  }
  // Register before the first snapshot. A topology change in the gap before
  // PollFds registers its generic wake channel stays pending on this handle.
  gesher::EpollWaiter waiter(ep);
  if (waiter.changed() == nullptr) return gesher::FailWithLastError();
  ULONGLONG deadline = gesher::DeadlineAfter(timeout);
  gesher::PollScratchLease<gesher::EpollPollScratch> lease;
  auto& scratch = lease.get();
  auto& fds = scratch.fds;
  auto& ids = scratch.ids;
  for (;;) {
    bool edge_ready = gesher::SnapshotEpoll(ep, scratch);
    if (observer != nullptr) {
      auto call = observer;
      observer = nullptr;
      call(context);
    }
    const int virtual_sample = gesher::SampleVirtualEntries(scratch, true);
    int wait_ms = -1;
    if (timeout >= 0) {
      wait_ms = gesher::MillisUntil(deadline);
    }
    if (edge_ready || (virtual_sample & 1) != 0) wait_ms = 0;
    else if ((virtual_sample & 2) != 0 && (wait_ms < 0 || wait_ms > 10)) wait_ms = 10;
    bool interrupted = false;
    int ready = gesher::PollFds(fds.data(), fds.size(), wait_ms, waiter.changed(), &interrupted);
    if (ready < 0) return -1;
    const int virtual_ready = gesher::SampleVirtualEntries(scratch, false);

    int count = 0;
    if (ready > 0 || edge_ready || interrupted || (virtual_ready & 1) != 0) {
      std::lock_guard<std::mutex> lock(ep->mutex);
      // A continuously ready low-numbered fd must not starve the rest when
      // the caller's output buffer is smaller than the ready set. Consult the
      // current cursor under the delivery lock, including another waiter's
      // progress since this snapshot was taken.
      auto next = std::upper_bound(scratch.keys.begin(), scratch.keys.end(), ep->last_ready);
      size_t i = static_cast<size_t>(next - scratch.keys.begin());
      for (size_t scanned = 0; scanned < fds.size() && count < maxevents; ++scanned, ++i) {
        if (i == fds.size()) i = 0;
        if (scratch.keys[i].IsVirtual() && !scratch.owners[i]) continue;
        auto it = ep->entries.find(scratch.keys[i]);
        // Gone, replaced or fired (EPOLLONESHOT) since the list was taken.
        if (it == ep->entries.end() || it->second.id != ids[i] || !it->second.armed) continue;
        if ((fds[i].revents & POLLNVAL) != 0) {
          // Closed behind gesher-base's back (_close): Linux would have
          // dropped the entry with the file.
          ep->entries.erase(it);
          continue;
        }
        uint32_t r = it->second.edge_source ? gesher::EventFdEdgeReady(it->second, true)
                                          : gesher::ToEpoll(fds[i].revents, it->second.events);
        if (r == 0) continue;
        events[count].events = r;
        events[count].data = it->second.data;
        ++count;
        ep->last_ready = scratch.keys[i];
        if ((it->second.events & EPOLLONESHOT) != 0) gesher::DisarmEntry(it->second);
      }
    }
    if (count > 0) return count;
    if (timeout == 0 || (timeout > 0 && gesher::MonotonicMicros() >= deadline)) return 0;
    // The list changed, or what was ready is no longer reportable: again.
  }
}

int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout) {
  return EpollWaitImpl(epfd, events, maxevents, timeout, nullptr, nullptr);
}

// Signals are not delivered asynchronously here, so the mask has nothing to
// change while waiting (as ppoll, fd.cc).
int epoll_pwait(int epfd, struct epoll_event* events, int maxevents, int timeout,
                const sigset_t*) {
  return epoll_wait(epfd, events, maxevents, timeout);
}

}  // extern "C"

namespace gesher {
int EpollWaitWithObserver(int epfd, epoll_event* events, int maxevents, int timeout,
                          void (*observer)(void*), void* context) {
  return EpollWaitImpl(epfd, events, maxevents, timeout, observer, context);
}
}  // namespace gesher
