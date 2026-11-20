// gesher-base: SCM_RIGHTS on Gesher's AF_UNIX socketpairs, through binderd's
// descriptor broker (ADR 0088 section 3; binder_wire.h has the protocol).
//
// A sender deposits the Windows handles behind its descriptors with the broker
// and gets a token; a datagram carries the token in its header
// (winsock.cc). On a stream pair the group also waits in the connection's
// queue at the broker, before the bytes it was sent with are written, so a
// receiver that reads those bytes finds it there; the broker's doorbell tells
// recvmsg when to look. Received handles become new descriptors of the same
// open files and sockets.
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <mutex>
#include <string>
#include <vector>

#include "binder_internal.h"
#include "binder_wire.h"
#include "event_internal.h"
#include "internal.h"
#include "socket_internal.h"

namespace gesher {
namespace {

namespace wire = binder_wire;

bool ReadExact(HANDLE pipe, void* buf, size_t n) {
  uint8_t* p = static_cast<uint8_t*>(buf);
  while (n > 0) {
    DWORD got = 0;
    if (!ReadFile(pipe, p, static_cast<DWORD>(n), &got, nullptr) || got == 0) return false;
    p += got;
    n -= got;
  }
  return true;
}

bool WriteExact(HANDLE pipe, const void* buf, size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(buf);
  while (n > 0) {
    DWORD put = 0;
    if (!WriteFile(pipe, p, static_cast<DWORD>(n), &put, nullptr) || put == 0) return false;
    p += put;
    n -= put;
  }
  return true;
}

struct Request {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(4);
  template <typename T>
  void Put(const T& v) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&v);
    bytes.insert(bytes.end(), b, b + sizeof(T));
  }
};

struct Reply {
  std::vector<uint8_t> bytes;
  size_t at = 0;
  template <typename T>
  bool Get(T* out) {
    if (bytes.size() - at < sizeof(T)) return false;
    memcpy(out, bytes.data() + at, sizeof(T));
    at += sizeof(T);
    return true;
  }
};

std::string Device() {
  const char* device = getenv("GESHER_BINDER");
  return device != nullptr ? device : "";
}

// One connection per process, one request at a time.
std::mutex g_mutex;
HANDLE g_pipe = INVALID_HANDLE_VALUE;

HANDLE Connect() {
  const std::string device = Device();
  if (device.empty()) return INVALID_HANDLE_VALUE;
  const std::wstring name = std::wstring(wire::kPipePrefix) + Widen(device.c_str());
  HANDLE pipe = INVALID_HANDLE_VALUE;
  for (int attempt = 0; attempt < 50 && pipe == INVALID_HANDLE_VALUE; ++attempt) {
    pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0,
                       nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      if (GetLastError() != ERROR_PIPE_BUSY) return INVALID_HANDLE_VALUE;
      WaitNamedPipeW(name.c_str(), 100);
    }
  }
  if (pipe == INVALID_HANDLE_VALUE) return pipe;
  Request hello;
  hello.Put<uint32_t>(wire::kHelloRights);
  hello.Put<uint32_t>(wire::kMagic);
  hello.Put<uint32_t>(wire::kVersion);
  const uint32_t len = static_cast<uint32_t>(hello.bytes.size() - 4);
  memcpy(hello.bytes.data(), &len, 4);
  uint32_t reply_len = 0;
  int32_t status = -1;
  if (!WriteExact(pipe, hello.bytes.data(), hello.bytes.size()) ||
      !ReadExact(pipe, &reply_len, 4) || reply_len != 4 || !ReadExact(pipe, &status, 4) ||
      status != 0) {
    CloseHandle(pipe);
    return INVALID_HANDLE_VALUE;
  }
  return pipe;
}

// One request and its reply; false (errno set) if the broker cannot be reached.
// g_mutex held.
bool Call(Request& request, Reply* reply) {
  const uint32_t len = static_cast<uint32_t>(request.bytes.size() - 4);
  memcpy(request.bytes.data(), &len, 4);
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (g_pipe == INVALID_HANDLE_VALUE) g_pipe = Connect();
    if (g_pipe == INVALID_HANDLE_VALUE) return errno = EOPNOTSUPP, false;
    uint32_t reply_len = 0;
    if (WriteExact(g_pipe, request.bytes.data(), request.bytes.size()) &&
        ReadExact(g_pipe, &reply_len, 4) && reply_len <= (16u << 20)) {
      reply->bytes.resize(reply_len);
      reply->at = 0;
      if (ReadExact(g_pipe, reply->bytes.data(), reply_len)) return true;
    }
    // The broker restarted, or the request was not delivered: a fresh connection.
    CloseHandle(g_pipe);
    g_pipe = INVALID_HANDLE_VALUE;
  }
  return errno = EIO, false;
}

// The delivered handles of a claim or take, as new descriptors (-1 for one the
// broker could not deliver, or this process could not adopt).
bool ReadDelivered(Reply& reply, std::vector<int>* fds, bool adopt) {
  int32_t status;
  uint32_t count;
  if (!reply.Get(&status) || !reply.Get(&count)) return errno = EIO, false;
  if (status != 0) return errno = -status, false;
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t handle;
    uint32_t metadata;
    if (!reply.Get(&handle) || !reply.Get(&metadata)) return errno = EIO, false;
    HANDLE h = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle));
    if (!adopt) {
      if (h != nullptr) CloseHandle(h);
      continue;
    }
    fds->push_back(h == nullptr ? -1 : AdoptTransferred(h, metadata));
  }
  return true;
}

volatile LONG* Doorbell() {
  static volatile LONG* doorbell = [] () -> volatile LONG* {
    const std::string device = Device();
    if (device.empty()) return nullptr;
    const std::wstring name = std::wstring(wire::kRightsDoorbellPrefix) + Widen(device.c_str());
    HANDLE section = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (section == nullptr) return nullptr;
    void* view = MapViewOfFile(section, FILE_MAP_READ, 0, 0,
                               wire::kRightsDoorbellSlots * sizeof(LONG));
    CloseHandle(section);  // the view keeps the section
    return static_cast<volatile LONG*>(view);
  }();
  return doorbell;
}

}  // namespace

bool DescribeForTransfer(int fd, uintptr_t* handle, uint32_t* metadata) {
  if (TransferSocket(fd, handle, metadata)) return true;
  gesher::Device device;
  short ready = 0;
  // Descriptors gesher-base serves itself have no handle another process
  // could use: they cannot travel (ADR 0088: EINVAL, nothing is sent).
  if (IsBinderFd(fd) || DeviceFromFd(fd, &device) || LookupEventFd(fd) != nullptr ||
      LookupInotify(fd) != nullptr || LookupTimerFd(fd) != nullptr || PollEpoll(fd, 0, &ready)) {
    return errno = EINVAL, false;
  }
  const intptr_t h = _get_osfhandle(fd);
  if (h == -1 || h == -2) return errno = EBADF, false;
  *handle = static_cast<uintptr_t>(h);
  *metadata = 0;
  return true;
}

int AdoptTransferred(HANDLE owned, uint32_t metadata) {
  if ((metadata & kTransferredSocket) != 0) {
    return AdoptSocket(reinterpret_cast<uintptr_t>(owned), metadata);
  }
  const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(owned), _O_RDWR | _O_BINARY);
  if (fd < 0) {
    CloseHandle(owned);
    return -1;
  }
  // A memfd keeps its seals on the file; this process must obey them too.
  RegisterMemfdIfSealed(fd, owned);
  return fd;
}

bool UnixRightsAvailable() {
  return Doorbell() != nullptr;
}

bool DepositRights(const std::vector<int>& fds, uint64_t queue, ws::RightsToken* token) {
  if (fds.empty() || fds.size() > wire::kRightsMaxPerMessage) return errno = EINVAL, false;
  Request request;
  request.Put<uint32_t>(wire::kRightsDeposit);
  request.Put<uint64_t>(queue);
  request.Put<uint32_t>(static_cast<uint32_t>(fds.size()));
  for (int fd : fds) {
    uintptr_t handle;
    uint32_t metadata;
    if (!DescribeForTransfer(fd, &handle, &metadata)) return false;
    request.Put<uint64_t>(handle);
    request.Put<uint32_t>(metadata);
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  Reply reply;
  if (!Call(request, &reply)) return false;
  int32_t status;
  if (!reply.Get(&status) || !reply.Get(&token->value[0]) || !reply.Get(&token->value[1])) {
    return errno = EIO, false;
  }
  if (status != 0) return errno = -status, false;
  token->count = static_cast<uint32_t>(fds.size());
  return true;
}

bool ClaimRights(const ws::RightsToken& token, std::vector<int>* fds) {
  Request request;
  request.Put<uint32_t>(wire::kRightsClaim);
  request.Put<uint64_t>(token.value[0]);
  request.Put<uint64_t>(token.value[1]);
  std::lock_guard<std::mutex> lock(g_mutex);
  Reply reply;
  return Call(request, &reply) && ReadDelivered(reply, fds, fds != nullptr);
}

bool TakeRights(uint64_t queue, std::vector<int>* fds) {
  Request request;
  request.Put<uint32_t>(wire::kRightsTake);
  request.Put<uint64_t>(queue);
  std::lock_guard<std::mutex> lock(g_mutex);
  Reply reply;
  return Call(request, &reply) && ReadDelivered(reply, fds, true);
}

void CancelRights(const ws::RightsToken& token) {
  Request request;
  request.Put<uint32_t>(wire::kRightsCancel);
  request.Put<uint64_t>(token.value[0]);
  request.Put<uint64_t>(token.value[1]);
  std::lock_guard<std::mutex> lock(g_mutex);
  Reply reply;
  Call(request, &reply);
}

bool RightsWaiting(uint16_t port) {
  volatile LONG* doorbell = Doorbell();
  return doorbell != nullptr && doorbell[port] > 0;
}

}  // namespace gesher
