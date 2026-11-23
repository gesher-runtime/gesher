// Android NDK raw DNS queries over Windows' configured per-interface servers.
// A socketpair publishes one admitted request's result, including failures, so
// poll/epoll observes completion even when the DNS server times out. The
// query's real UDP/TCP socket is bound to the selected outgoing interface.
#include <gesher/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

#include "linux_socket.h"
#include "socket_internal.h"

namespace {
namespace ws = gesher::ws;
using Clock = std::chrono::steady_clock;
// Android DnsResolver's res_nsend defaults (RES_TIMEOUT, RES_DFLRETRY):
// retry_count rounds over the servers, each server waiting the base timeout
// shifted by its index and divided by the server count (5s for the first),
// never less than one second. ANDROID_RESOLV_NO_RETRY makes one request to
// one server.
constexpr int kBaseTimeoutMs = 5000;
constexpr int kRetryCount = 2;
constexpr size_t kMaxServers = 3;
// Admit the issue #178 256-request workload, with the 32-request workload able
// to run concurrently. Larger bursts fail before allocating request resources.
constexpr size_t kMaxWorkers = 32;

int ServerTimeout(size_t server, size_t count) {
  int milliseconds = kBaseTimeoutMs << server;
  if (server > 0) milliseconds /= static_cast<int>(count);
  return std::max(milliseconds, 1000);
}

struct Query {
  static constexpr size_t kMaxOutstanding = 256;
  static std::atomic<size_t>& Outstanding() { static std::atomic<size_t> count{0}; return count; }
  static bool Reserve() {
    size_t count = Outstanding().load();
    while (count < kMaxOutstanding) {
      if (Outstanding().compare_exchange_weak(count, count + 1)) return true;
    }
    return false;
  }
  // Retain the lease through BOTH caller consumption/cancellation and worker
  // retirement. A canceled external call or unread answer still owns resources.
  bool admitted = false;
  ~Query() { if (admitted) --Outstanding(); }
  std::atomic<bool> cancelled{false};
  std::mutex mutex;
  int active = -1;  // shutdown may interrupt it; only its worker closes it
  int writer = -1;
  int error = -EIO;
  std::vector<uint8_t> answer;
};
std::mutex g_queries_mutex;
std::map<int, std::shared_ptr<Query>> g_queries;

struct Admission {
  bool owned = Query::Reserve();
  ~Admission() { if (owned) --Query::Outstanding(); }
};

bool Transfer(int fd, void* bytes, size_t size, bool writing) {
  auto* position = static_cast<uint8_t*>(bytes);
  while (size > 0) {
    ssize_t count = writing ? send(fd, position, size, 0) : recv(fd, position, size, 0);
    if (count <= 0) { if (count == 0) errno = ECONNRESET; return false; }
    position += count;
    size -= static_cast<size_t>(count);
  }
  return true;
}

void Timeout(int fd, int milliseconds) {
  timeval value{milliseconds / 1000, (milliseconds % 1000) * 1000};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
}

int Remaining(Clock::time_point deadline) {
  auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
  return static_cast<int>(std::max<int64_t>(0, remaining));
}

// A TCP peer cannot renew the request indefinitely by trickling one byte per
// socket timeout. The same monotonic request/exchange deadline covers all I/O.
bool TransferUntil(const std::shared_ptr<Query>& query, int fd, void* bytes, size_t size,
                   bool writing, Clock::time_point deadline) {
  auto* position = static_cast<uint8_t*>(bytes);
  while (size > 0) {
    if (query->cancelled) return errno = ECANCELED, false;
    int remaining = Remaining(deadline);
    if (remaining == 0) return errno = ETIMEDOUT, false;
    Timeout(fd, remaining);
    if (Remaining(deadline) == 0) return errno = ETIMEDOUT, false;
    ssize_t count = writing ? send(fd, position, size, 0) : recv(fd, position, size, 0);
    if (count <= 0) {
      if (count == 0) errno = ECONNRESET;
      if (errno == EAGAIN) errno = ETIMEDOUT;
      return false;
    }
    position += count;
    size -= static_cast<size_t>(count);
  }
  return true;
}

int Open(const std::shared_ptr<Query>& query, const ws::Address& server, uint32_t index, bool tcp,
         int timeout) {
  std::lock_guard<std::mutex> lock(query->mutex);
  if (query->cancelled) return errno = ECANCELED, -1;
  ws::Socket endpoint = ws::Open(server.family, tcp ? ws::Type::kStream : ws::Type::kDgram, 0, false);
  if (endpoint == ws::kInvalidSocket) return -1;
  if (index != 0 && ws::BindInterface(endpoint, index) != 0) {
    int saved = errno;
    ws::Close(endpoint);
    return errno = saved, -1;
  }
  gesher::SocketFd state;
  state.socket = endpoint;
  int fd = gesher::RegisterSocket(state);
  if (fd < 0) return -1;
  if (query->cancelled) { close(fd); return errno = ECANCELED, -1; }
  query->active = fd;
  Timeout(fd, timeout);
  return fd;
}

void Close(const std::shared_ptr<Query>& query, int fd) {
  std::lock_guard<std::mutex> lock(query->mutex);
  query->active = -1;
  close(fd);
}

int Connect(int fd, const ws::Address& server, bool tcp, Clock::time_point deadline) {
  sockaddr_storage address{};
  socklen_t length = sizeof(address);
  gesher::sock::FromAddress(server, reinterpret_cast<sockaddr*>(&address), &length);
  if (Remaining(deadline) == 0) return -ETIMEDOUT;
  if (!tcp) return connect(fd, reinterpret_cast<sockaddr*>(&address), length) == 0 ? 0 : -errno;
  int mode = fcntl(fd, F_GETFL);
  if (mode < 0 || fcntl(fd, F_SETFL, mode | O_NONBLOCK) != 0) return -errno;
  if (Remaining(deadline) == 0) { fcntl(fd, F_SETFL, mode); return -ETIMEDOUT; }
  int result = connect(fd, reinterpret_cast<sockaddr*>(&address), length);
  int error = result == 0 ? 0 : errno;
  if (result != 0 && error == EINPROGRESS) {
    pollfd pending{fd, POLLOUT, 0};
    int timeout = Remaining(deadline);
    int ready = timeout > 0 ? poll(&pending, 1, timeout) : 0;
    if (ready > 0) {
      socklen_t size = sizeof(error);
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0) error = errno;
    } else {
      error = ready == 0 ? ETIMEDOUT : errno;
    }
  }
  fcntl(fd, F_SETFL, mode);
  return -error;
}

int Exchange(const std::shared_ptr<Query>& query, const ws::Address& server, uint32_t index,
             const std::vector<uint8_t>& packet, std::vector<uint8_t>* answer, bool tcp, int timeout,
             Clock::time_point request_deadline) {
  auto deadline = std::min(request_deadline, Clock::now() + std::chrono::milliseconds(timeout));
  timeout = Remaining(deadline);
  if (timeout == 0) return -ETIMEDOUT;
  int fd = Open(query, server, index, tcp, timeout);
  if (fd < 0) return -errno;
  // Socket/interface setup can exhaust a queued request's remaining budget.
  // Carry the deadline through connect and refresh the UDP send timeout too.
  int error = Connect(fd, server, tcp, deadline);
  if (error == 0 && !tcp) {
    int remaining = Remaining(deadline);
    if (remaining == 0) error = -ETIMEDOUT;
    else Timeout(fd, remaining);
  }
  if (error == 0) {
    if (tcp) {
      uint8_t length[2] = {static_cast<uint8_t>(packet.size() >> 8), static_cast<uint8_t>(packet.size())};
      if (!TransferUntil(query, fd, length, 2, true, deadline) ||
          !TransferUntil(query, fd, const_cast<uint8_t*>(packet.data()), packet.size(), true, deadline) ||
          !TransferUntil(query, fd, length, 2, false, deadline)) {
        error = -(errno != 0 ? errno : EIO);
      } else {
        answer->resize(static_cast<size_t>(length[0]) << 8 | length[1]);
        if (!TransferUntil(query, fd, answer->data(), answer->size(), false, deadline))
          error = -(errno != 0 ? errno : EIO);
      }
    } else if (Remaining(deadline) == 0) {
      error = -ETIMEDOUT;
    } else if (send(fd, packet.data(), packet.size(), 0) != static_cast<ssize_t>(packet.size())) {
      error = -errno;
    } else {
      answer->resize(65535);
      while (true) {
        if (query->cancelled) { error = -ECANCELED; break; }
        int remaining = Remaining(deadline);
        if (remaining <= 0) { error = -ETIMEDOUT; break; }
        Timeout(fd, remaining);
        if (Remaining(deadline) == 0) { error = -ETIMEDOUT; break; }
        ssize_t size = recv(fd, answer->data(), answer->size(), 0);
        if (query->cancelled) { error = -ECANCELED; break; }
        if (size < 0) { error = errno == EAGAIN ? -ETIMEDOUT : -errno; break; }
        if (size >= 12 && (*answer)[0] == packet[0] && (*answer)[1] == packet[1] &&
            ((*answer)[2] & 0x80) != 0) {
          answer->resize(static_cast<size_t>(size));
          break;
        }
      }
    }
  }
  if (query->cancelled) error = -ECANCELED;
  if (error == 0 && (answer->size() < 12 || (*answer)[0] != packet[0] || (*answer)[1] != packet[1] ||
                    ((*answer)[2] & 0x80) == 0)) error = -EBADMSG;
  Close(query, fd);
  return error;
}

void Publish(const std::shared_ptr<Query>& query, int error, std::vector<uint8_t>& answer) {
  {
    std::lock_guard<std::mutex> lock(query->mutex);
    query->error = error;
    query->answer = std::move(answer);
  }
  // Only the completion byte uses the socket buffer. A full-size answer
  // remains available until the caller consumes it, without blocking the
  // worker or imposing a timeout on when the caller must read it.
  uint8_t complete = 1;
  Transfer(query->writer, &complete, 1, true);
  close(query->writer);
  query->writer = -1;
}

struct Job {
  std::shared_ptr<Query> query;
  std::vector<ws::Address> servers;
  uint32_t index;
  std::vector<uint8_t> packet;
  uint32_t flags;
  Clock::time_point deadline;
};

void QueryWorker(const Job& job) {
  std::vector<uint8_t> answer;
  int error = -EIO;
  bool single = (job.flags & 1) != 0;  // ANDROID_RESOLV_NO_RETRY
  size_t count = single ? std::min(job.servers.size(), size_t{1}) : job.servers.size();
  int attempts = single ? 1 : kRetryCount;
  for (int attempt = 0; attempt < attempts && error != 0 && !job.query->cancelled; ++attempt) {
    for (size_t i = 0; i < count && !job.query->cancelled; ++i) {
      int timeout = ServerTimeout(i, count);
      error = Exchange(job.query, job.servers[i], job.index, job.packet, &answer, false, timeout, job.deadline);
      if (error == 0 && (answer[2] & 2) != 0) {
        error = Exchange(job.query, job.servers[i], job.index, job.packet, &answer, true, timeout, job.deadline);
      }
      if (error == 0) break;
    }
  }
  Publish(job.query, job.query->cancelled ? -ECANCELED : error, answer);
}

class Dispatcher {
 public:
  static Dispatcher& Get() {
    // Process lifetime, like the DNS descriptor registry. Windows owns retirement
    // of idle pool threads; no permanent polling thread or callback per request.
    alignas(Dispatcher) static unsigned char storage[sizeof(Dispatcher)];
    static Dispatcher* instance = new (storage) Dispatcher;
    return *instance;
  }
  bool Submit(std::unique_ptr<Job>& job) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (work_ == nullptr) {
      pool_ = CreateThreadpool(nullptr);
      if (pool_ == nullptr) return false;
      SetThreadpoolThreadMaximum(pool_, static_cast<DWORD>(kMaxWorkers));
      if (!SetThreadpoolThreadMinimum(pool_, 0)) {
        CloseThreadpool(pool_); pool_ = nullptr; return false;
      }
      InitializeThreadpoolEnvironment(&environment_);
      SetThreadpoolCallbackPool(&environment_, pool_);
      SetThreadpoolCallbackRunsLong(&environment_); // DNS waits are blocking I/O
      work_ = CreateThreadpoolWork(Dispatch, this, &environment_);
      if (work_ == nullptr) {
        DestroyThreadpoolEnvironment(&environment_);
        CloseThreadpool(pool_);
        pool_ = nullptr;
        return false;
      }
    }
    jobs_.push_back(std::move(job));
    // At most kMaxWorkers scheduled/active callback tickets. Cancel/re-admit
    // churn must not build an unbounded Windows thread-pool callback queue.
    if (tickets_ < kMaxWorkers) {
      ++tickets_;
      SubmitThreadpoolWork(work_);
    }
    return true;
  }
  bool Cancel(const std::shared_ptr<Query>& query) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = std::find_if(jobs_.begin(), jobs_.end(),
        [&](const auto& job) { return job->query == query; });
    if (found == jobs_.end()) return false; // the worker owns retirement
    jobs_.erase(found);
    close(query->writer);
    query->writer = -1;
    return true;
  }
 private:
  static void CALLBACK Dispatch(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) {
    auto* self = static_cast<Dispatcher*>(context);
    for (;;) {
      std::unique_ptr<Job> job;
      {
        std::lock_guard<std::mutex> lock(self->mutex_);
        if (self->jobs_.empty()) { --self->tickets_; return; }
        job = std::move(self->jobs_.front());
        self->jobs_.pop_front();
      }
      QueryWorker(*job);
    }
  }
  std::mutex mutex_;
  std::deque<std::unique_ptr<Job>> jobs_;
  size_t tickets_ = 0;
  TP_CALLBACK_ENVIRON environment_{};
  PTP_POOL pool_ = nullptr;
  PTP_WORK work_ = nullptr;
};

bool CancelPendingQuery(const std::shared_ptr<Query>& query) { return Dispatcher::Get().Cancel(query); }

bool ValidQuestion(const char* name) {
  size_t size = 12;
  const char* label = name;
  while (*label != 0) {
    const char* end = label;
    while (*end != 0 && *end != '.' && end - label <= 63) ++end;
    size_t length = static_cast<size_t>(end - label);
    if (length == 0 && *end == '.' && end[1] == 0) break;
    if (length == 0 || length > 63 || size + length + 1 > 266) return false;
    size += length + 1;
    if (*end == 0) break;
    label = end + 1;
  }
  return true;
}

int SendAdmitted(uint32_t index, const uint8_t* bytes, size_t size, uint32_t flags,
                 Admission& admission, Clock::time_point submitted) {
  if (index == 0) index = ws::ProcessInterface();
  std::vector<ws::Address> servers;
  if (ws::DnsServers(index, &servers, &index) != 0) return -errno;
  if (servers.empty()) return -ENONET;
  if (servers.size() > kMaxServers) servers.resize(kMaxServers);
  size_t count = (flags & 1) != 0 ? 1 : servers.size();
  // The established retry/server budget, including one possible TCP fallback
  // per UDP exchange, now also covers queueing and all TCP transfers.
  int deadline_ms = 0;
  for (size_t i = 0; i < count; ++i) deadline_ms += 2 * ServerTimeout(i, count);
  if ((flags & 1) == 0) deadline_ms *= kRetryCount;
  auto query = std::make_shared<Query>();
  query->admitted = true;
  admission.owned = false;
  std::vector<uint8_t> packet(bytes, bytes + size);
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) return -errno;
  query->writer = pair[1];
  {
    std::lock_guard<std::mutex> lock(g_queries_mutex);
    g_queries[pair[0]] = query;
  }
  std::unique_ptr<Job> job(new (std::nothrow) Job{query, std::move(servers), index, std::move(packet), flags,
      submitted + std::chrono::milliseconds(deadline_ms)});
  if (job == nullptr || !Dispatcher::Get().Submit(job)) {
    {
      std::lock_guard<std::mutex> lock(g_queries_mutex);
      g_queries.erase(pair[0]);
    }
    close(pair[0]);
    close(pair[1]);
    return job == nullptr ? -ENOMEM : -EAGAIN;
  }
  return pair[0];
}
}  // namespace

extern "C" int gesher_res_nsend(uint32_t index, const uint8_t* bytes, size_t size, uint32_t flags) {
  if (bytes == nullptr || size < 12 || size > 65535 || (flags & ~7U) != 0 || (bytes[2] & 0x80)) return -EINVAL;
  if (!gesher_inet_allowed()) return -EACCES;
  Admission admission;
  if (!admission.owned) return -EAGAIN;
  return SendAdmitted(index, bytes, size, flags, admission, Clock::now());
}

extern "C" int gesher_res_nquery(uint32_t index, const char* name, int dns_class, int type, uint32_t flags) {
  if (name == nullptr || *name == 0 || dns_class < 1 || dns_class > 65535 || type < 1 || type > 65535) return -EINVAL;
  if (!ValidQuestion(name)) return -EINVAL;
  if ((flags & ~7U) != 0) return -EINVAL;
  if (!gesher_inet_allowed()) return -EACCES;
  Admission admission;
  if (!admission.owned) return -EAGAIN;
  auto submitted = Clock::now();
  std::vector<uint8_t> packet(12);
  if (BCryptGenRandom(nullptr, packet.data(), 2, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return -EIO;
  packet[2] = 1;  // Recursion desired.
  packet[5] = 1;  // One question.
  const char* label = name;
  while (*label != 0) {
    const char* dot = std::strchr(label, '.');
    size_t length = dot != nullptr ? static_cast<size_t>(dot - label) : std::strlen(label);
    if (length == 0 && dot != nullptr && dot[1] == 0) break;  // Root name.
    if (length == 0 || length > 63 || packet.size() + length + 1 > 266) return -EINVAL;
    packet.push_back(static_cast<uint8_t>(length));
    packet.insert(packet.end(), label, label + length);
    if (dot == nullptr) break;
    label = dot + 1;
  }
  packet.push_back(0);
  packet.push_back(static_cast<uint8_t>(type >> 8)); packet.push_back(static_cast<uint8_t>(type));
  packet.push_back(static_cast<uint8_t>(dns_class >> 8)); packet.push_back(static_cast<uint8_t>(dns_class));
  return SendAdmitted(index, packet.data(), packet.size(), flags, admission, submitted);
}

extern "C" int gesher_res_nresult(int fd, int* rcode, uint8_t* answer, size_t capacity) {
  std::shared_ptr<Query> query;
  {
    std::lock_guard<std::mutex> lock(g_queries_mutex);
    auto found = g_queries.find(fd);
    if (found == g_queries.end()) return -EBADF;
    query = found->second;
    g_queries.erase(found);
  }
  uint8_t complete = 0;
  int error = 0;
  if (rcode == nullptr || answer == nullptr) error = -EINVAL;
  else if (!Transfer(fd, &complete, 1, false)) error = -EIO;
  close(fd);
  if (error != 0) return error;
  std::lock_guard<std::mutex> lock(query->mutex);
  if (query->error != 0) return query->error;
  if (query->answer.size() < 12 || query->answer.size() > 65535) return -EBADMSG;
  if (capacity < query->answer.size()) return -EMSGSIZE;
  std::memcpy(answer, query->answer.data(), query->answer.size());
  *rcode = answer[3] & 15;
  return static_cast<int>(query->answer.size());
}

extern "C" void gesher_res_cancel(int fd) {
  std::shared_ptr<Query> query;
  {
    std::lock_guard<std::mutex> lock(g_queries_mutex);
    auto found = g_queries.find(fd);
    if (found == g_queries.end()) return;
    query = found->second;
    g_queries.erase(found);
  }
  query->cancelled = true;
  CancelPendingQuery(query);
  {
    std::lock_guard<std::mutex> lock(query->mutex);
    if (query->active >= 0) shutdown(query->active, SHUT_RDWR);
  }
  close(fd);
}
