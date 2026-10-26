// Android NDK raw DNS queries over Windows' configured per-interface servers.
// A socketpair publishes one bounded worker's result, including failures, so
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
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "linux_socket.h"
#include "socket_internal.h"

namespace {
namespace ws = gesher::ws;
using Clock = std::chrono::steady_clock;
constexpr int kTimeoutMs = 3000;

struct Query {
  std::atomic<bool> cancelled{false};
  std::mutex mutex;
  int active = -1;  // shutdown may interrupt it; only its worker closes it
  int writer = -1;
  int error = -EIO;
  std::vector<uint8_t> answer;
};
std::mutex g_queries_mutex;
std::map<int, std::shared_ptr<Query>> g_queries;

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

int Open(const std::shared_ptr<Query>& query, const ws::Address& server, uint32_t index, bool tcp) {
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
  Timeout(fd, kTimeoutMs);
  return fd;
}

void Close(const std::shared_ptr<Query>& query, int fd) {
  std::lock_guard<std::mutex> lock(query->mutex);
  query->active = -1;
  close(fd);
}

int Connect(int fd, const ws::Address& server, bool tcp) {
  sockaddr_storage address{};
  socklen_t length = sizeof(address);
  gesher::sock::FromAddress(server, reinterpret_cast<sockaddr*>(&address), &length);
  if (!tcp) return connect(fd, reinterpret_cast<sockaddr*>(&address), length) == 0 ? 0 : -errno;
  int mode = fcntl(fd, F_GETFL);
  if (mode < 0 || fcntl(fd, F_SETFL, mode | O_NONBLOCK) != 0) return -errno;
  int result = connect(fd, reinterpret_cast<sockaddr*>(&address), length);
  int error = result == 0 ? 0 : errno;
  if (result != 0 && error == EINPROGRESS) {
    pollfd pending{fd, POLLOUT, 0};
    int ready = poll(&pending, 1, kTimeoutMs);
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
             const std::vector<uint8_t>& packet, std::vector<uint8_t>* answer, bool tcp) {
  int fd = Open(query, server, index, tcp);
  if (fd < 0) return -errno;
  int error = Connect(fd, server, tcp);
  if (error == 0) {
    if (tcp) {
      uint8_t length[2] = {static_cast<uint8_t>(packet.size() >> 8), static_cast<uint8_t>(packet.size())};
      if (!Transfer(fd, length, 2, true) || !Transfer(fd, const_cast<uint8_t*>(packet.data()), packet.size(), true) ||
          !Transfer(fd, length, 2, false)) {
        error = -(errno != 0 ? errno : EIO);
      } else {
        answer->resize(static_cast<size_t>(length[0]) << 8 | length[1]);
        if (!Transfer(fd, answer->data(), answer->size(), false)) error = -(errno != 0 ? errno : EIO);
      }
    } else if (send(fd, packet.data(), packet.size(), 0) != static_cast<ssize_t>(packet.size())) {
      error = -errno;
    } else {
      auto deadline = Clock::now() + std::chrono::milliseconds(kTimeoutMs);
      answer->resize(65535);
      while (true) {
        int remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - Clock::now()).count());
        if (remaining <= 0) { error = -ETIMEDOUT; break; }
        Timeout(fd, remaining);
        ssize_t size = recv(fd, answer->data(), answer->size(), 0);
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
}
}  // namespace

extern "C" int gesher_res_nsend(uint32_t index, const uint8_t* bytes, size_t size, uint32_t flags) {
  if (bytes == nullptr || size < 12 || size > 65535 || (flags & ~7U) != 0 || (bytes[2] & 0x80)) return -EINVAL;
  if (!gesher_inet_allowed()) return -EACCES;
  if (index == 0) index = ws::ProcessInterface();
  std::vector<ws::Address> servers;
  std::shared_ptr<Query> query;
  std::vector<uint8_t> packet;
  try {
    if (ws::DnsServers(index, &servers, &index) != 0) return -errno;
    query = std::make_shared<Query>();
    packet.assign(bytes, bytes + size);
  } catch (...) { return -ENOMEM; }
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) return -errno;
  query->writer = pair[1];
  try {
    std::lock_guard<std::mutex> lock(g_queries_mutex);
    g_queries[pair[0]] = query;
  } catch (...) { close(pair[0]); close(pair[1]); return -ENOMEM; }
  try {
    std::thread([query, servers, index, packet = std::move(packet), flags] {
      std::vector<uint8_t> answer;
      int error = -EIO;
      try {
        size_t count = (flags & 1) != 0 ? 1 : std::min<size_t>(servers.size(), 3);
        for (size_t i = 0; i < count && !query->cancelled; ++i) {
          error = Exchange(query, servers[i], index, packet, &answer, false);
          if (error == 0 && (answer[2] & 2) != 0) {
            error = Exchange(query, servers[i], index, packet, &answer, true);
          }
          if (error == 0) break;
        }
      } catch (...) { error = -ENOMEM; }
      Publish(query, query->cancelled ? -ECANCELED : error, answer);
    }).detach();
  } catch (...) {
    gesher_res_cancel(pair[0]);
    close(pair[1]);
    return -EAGAIN;
  }
  return pair[0];
}

extern "C" int gesher_res_nquery(uint32_t index, const char* name, int dns_class, int type, uint32_t flags) {
  if (name == nullptr || *name == 0 || dns_class < 1 || dns_class > 65535 || type < 1 || type > 65535) return -EINVAL;
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
  return gesher_res_nsend(index, packet.data(), packet.size(), flags);
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
  {
    std::lock_guard<std::mutex> lock(query->mutex);
    if (query->active >= 0) shutdown(query->active, SHUT_RDWR);
  }
  close(fd);
}
