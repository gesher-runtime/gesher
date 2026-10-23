// gesher-base: /dev/binder, over binderd (compat/binder/binderd.cc).
//
// $GESHER_BINDER names a binder device served by a binderd; without it, or
// with no binderd serving it, /dev/binder does not exist (ENOENT), so
// libbinder runs in-process only, as before. open() connects the process to
// the broker; each thread's first ioctl connects that thread
// (binder_wire.h). BINDER_WRITE_READ sends the command stream with the
// transaction data the commands point to, and copies each transaction the
// broker returns into the process's binder mapping (the fd's mmap), as the
// kernel driver does, so libbinder sees the same pointers and the same
// BC_FREE_BUFFER protocol. See docs/decisions/0015-services-binder-ipc.md.
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <windows.h>

#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <linux/android/binder.h>

#include "binder_internal.h"
#include "binder_wire.h"
#include "internal.h"

namespace gesher {
namespace {

namespace wire = binder_wire;

// --- pipes -------------------------------------------------------------------

bool ReadExact(HANDLE pipe, void* buf, size_t n) {
  uint8_t* p = static_cast<uint8_t*>(buf);
  while (n > 0) {
    DWORD got = 0;
    DWORD want = n > 0x10000000 ? 0x10000000 : static_cast<DWORD>(n);
    if (!ReadFile(pipe, p, want, &got, nullptr) || got == 0) return false;
    p += got;
    n -= got;
  }
  return true;
}

bool WriteExact(HANDLE pipe, const void* buf, size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(buf);
  while (n > 0) {
    DWORD put = 0;
    DWORD want = n > 0x10000000 ? 0x10000000 : static_cast<DWORD>(n);
    if (!WriteFile(pipe, p, want, &put, nullptr) || put == 0) return false;
    p += put;
    n -= put;
  }
  return true;
}

struct Message {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(4);  // the length, filled in by Send
  void Write(const void* data, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(data);
    bytes.insert(bytes.end(), b, b + n);
  }
  template <typename T>
  void Put(const T& v) {
    Write(&v, sizeof(T));
  }
};

bool Send(HANDLE pipe, Message& m) {
  uint32_t len = static_cast<uint32_t>(m.bytes.size() - 4);
  memcpy(m.bytes.data(), &len, 4);
  return WriteExact(pipe, m.bytes.data(), m.bytes.size());
}

bool Receive(HANDLE pipe, std::vector<uint8_t>* out) {
  uint32_t len;
  if (!ReadExact(pipe, &len, sizeof(len)) || len > (64u << 20)) return false;
  out->resize(len);
  return ReadExact(pipe, out->data(), len);
}

struct Reader {
  const uint8_t* p;
  const uint8_t* end;
  bool Read(void* out, size_t n) {
    if (static_cast<size_t>(end - p) < n) return false;
    memcpy(out, p, n);
    p += n;
    return true;
  }
  template <typename T>
  bool Get(T* out) {
    return Read(out, sizeof(T));
  }
};

std::wstring PipeName() {
  const char* device = getenv("GESHER_BINDER");
  if (device == nullptr || device[0] == '\0') return {};
  return std::wstring(wire::kPipePrefix) + Widen(device);
}

HANDLE ConnectPipe(const std::wstring& name) {
  for (int attempt = 0; attempt < 50; ++attempt) {
    HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           0, nullptr);
    if (h != INVALID_HANDLE_VALUE) return h;
    if (GetLastError() != ERROR_PIPE_BUSY) return INVALID_HANDLE_VALUE;
    WaitNamedPipeW(name.c_str(), 100);
  }
  SetLastError(ERROR_PIPE_BUSY);
  return INVALID_HANDLE_VALUE;
}

// --- the open device ------------------------------------------------------------

struct Allocation {
  uint64_t id;          // the broker's buffer
  size_t offset;        // in the mapping, or kMalloced
  size_t size;
};
constexpr size_t kMalloced = SIZE_MAX;

struct BinderFile {
  std::wstring pipe_name;
  HANDLE proc_pipe = INVALID_HANDLE_VALUE;
  uint32_t proc_id = 0;
  uint64_t key = 0;
  bool non_block = false;

  std::mutex mutex;  // guards what follows
  uint8_t* map = nullptr;
  size_t map_size = 0;
  std::map<size_t, size_t> free_blocks;  // offset -> size, in the mapping
  std::unordered_map<uintptr_t, Allocation> allocations;

  ~BinderFile() {
    // Closing the process's connection releases it in the broker.
    if (proc_pipe != INVALID_HANDLE_VALUE) CloseHandle(proc_pipe);
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& [ptr, a] : allocations) {
      if (a.offset == kMalloced) free(reinterpret_cast<void*>(ptr));
    }
  }

  // A block for a transaction the broker delivers: in the mapping when it
  // fits (as the kernel allocates it), else on the heap.
  uint8_t* Allocate(size_t size, uint64_t id) {
    size = (size + 7) & ~size_t{7};
    if (size == 0) size = 8;
    std::lock_guard<std::mutex> lock(mutex);
    for (auto it = free_blocks.begin(); it != free_blocks.end(); ++it) {
      if (it->second < size) continue;
      size_t offset = it->first;
      size_t rest = it->second - size;
      free_blocks.erase(it);
      if (rest > 0) free_blocks[offset + size] = rest;
      uint8_t* p = map + offset;
      allocations[reinterpret_cast<uintptr_t>(p)] = {id, offset, size};
      return p;
    }
    uint8_t* p = static_cast<uint8_t*>(malloc(size));
    if (p != nullptr) allocations[reinterpret_cast<uintptr_t>(p)] = {id, kMalloced, size};
    return p;
  }

  // BC_FREE_BUFFER: the broker's id for the block at ptr, which is freed.
  uint64_t Free(uintptr_t ptr) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = allocations.find(ptr);
    if (it == allocations.end()) return 0;
    Allocation a = it->second;
    allocations.erase(it);
    if (a.offset == kMalloced) {
      free(reinterpret_cast<void*>(ptr));
      return a.id;
    }
    // Coalesce with the neighbouring free blocks.
    size_t offset = a.offset;
    size_t size = a.size;
    auto next = free_blocks.lower_bound(offset);
    if (next != free_blocks.end() && next->first == offset + size) {
      size += next->second;
      next = free_blocks.erase(next);
    }
    if (next != free_blocks.begin()) {
      auto prev = std::prev(next);
      if (prev->first + prev->second == offset) {
        prev->second += size;
        return a.id;
      }
    }
    free_blocks[offset] = size;
    return a.id;
  }
};

std::mutex files_mutex;
std::unordered_map<int, std::shared_ptr<BinderFile>> files;

std::shared_ptr<BinderFile> Lookup(int fd) {
  std::lock_guard<std::mutex> lock(files_mutex);
  if (files.empty()) return nullptr;
  auto it = files.find(fd);
  return it == files.end() ? nullptr : it->second;
}

// --- per-thread connections -----------------------------------------------------

struct ThreadConnection {
  std::weak_ptr<BinderFile> file;
  BinderFile* key;
  HANDLE pipe;
};

struct ThreadConnections {
  std::vector<ThreadConnection> list;
  ~ThreadConnections() {
    for (ThreadConnection& c : list) CloseHandle(c.pipe);
  }
};

// Fiber-local storage, whose callback runs when any thread exits, closes the
// thread's connections: the broker then releases the binder thread
// (binder_thread_release), as the kernel does when a thread exits.
DWORD fls_index = FLS_OUT_OF_INDEXES;
std::once_flag fls_once;

void NTAPI FreeThreadConnections(void* data) { delete static_cast<ThreadConnections*>(data); }

ThreadConnections* Connections() {
  std::call_once(fls_once, [] { fls_index = FlsAlloc(FreeThreadConnections); });
  if (fls_index == FLS_OUT_OF_INDEXES) return nullptr;
  auto* conns = static_cast<ThreadConnections*>(FlsGetValue(fls_index));
  if (conns == nullptr) {
    conns = new ThreadConnections;
    FlsSetValue(fls_index, conns);
  }
  return conns;
}

// The calling thread's connection for `file`, opened on first use. Returns
// INVALID_HANDLE_VALUE with errno set on failure.
HANDLE ThreadPipe(const std::shared_ptr<BinderFile>& file) {
  ThreadConnections* conns = Connections();
  if (conns == nullptr) return errno = ENOMEM, INVALID_HANDLE_VALUE;
  for (auto it = conns->list.begin(); it != conns->list.end();) {
    if (it->file.expired()) {  // a closed fd's
      CloseHandle(it->pipe);
      it = conns->list.erase(it);
      continue;
    }
    if (it->key == file.get()) return it->pipe;
    ++it;
  }
  HANDLE pipe = ConnectPipe(file->pipe_name);
  if (pipe == INVALID_HANDLE_VALUE) return errno = EBADF, INVALID_HANDLE_VALUE;
  Message hello;
  hello.Put<uint32_t>(wire::kHelloThread);
  hello.Put<uint32_t>(wire::kMagic);
  hello.Put<uint32_t>(wire::kVersion);
  hello.Put<uint32_t>(file->proc_id);
  hello.Put<uint64_t>(file->key);
  hello.Put<uint32_t>(GetCurrentThreadId());
  std::vector<uint8_t> reply;
  int32_t status = -EBADF;
  if (!Send(pipe, hello) || !Receive(pipe, &reply) || reply.size() < 4) {
    CloseHandle(pipe);
    return errno = EBADF, INVALID_HANDLE_VALUE;
  }
  memcpy(&status, reply.data(), 4);
  if (status != 0) {
    CloseHandle(pipe);
    return errno = -status, INVALID_HANDLE_VALUE;
  }
  conns->list.push_back({file, file.get(), pipe});
  return pipe;
}

void CloseThreadPipe(BinderFile* file) {
  ThreadConnections* conns = Connections();
  if (conns == nullptr) return;
  for (auto it = conns->list.begin(); it != conns->list.end(); ++it) {
    if (it->key == file) {
      CloseHandle(it->pipe);
      conns->list.erase(it);
      return;
    }
  }
}

// --- ioctls ---------------------------------------------------------------------------

// File descriptors in transactions (BINDER_TYPE_FD). The kernel installs a
// new fd for the same open file in the receiver. Here the sender puts the
// fd's Windows handle in the object (its 64-bit pad_binder, which shares the
// fd's union), binderd duplicates it into the receiving process
// (DuplicateHandle), and the receiver wraps the duplicate in a new fd. Only
// fds backed by a Windows handle (files, memfds, pipes) travel; any other
// goes as handle 0, which binderd refuses (BR_FAILED_REPLY).
template <typename F>
void ForEachFdObject(uint8_t* data, size_t data_size, const void* offsets, size_t offsets_size,
                     F f) {
  const uint8_t* offs = static_cast<const uint8_t*>(offsets);
  for (size_t i = 0; i + sizeof(binder_size_t) <= offsets_size; i += sizeof(binder_size_t)) {
    binder_size_t off;
    memcpy(&off, offs + i, sizeof(off));
    if (off > data_size || data_size - off < sizeof(binder_fd_object)) continue;
    binder_fd_object obj;
    memcpy(&obj, data + off, sizeof(obj));
    if (obj.hdr.type != BINDER_TYPE_FD) continue;
    f(obj);
    memcpy(data + off, &obj, sizeof(obj));
  }
}

void SendFds(uint8_t* data, size_t data_size, const void* offsets, size_t offsets_size) {
  ForEachFdObject(data, data_size, offsets, offsets_size, [](binder_fd_object& obj) {
    intptr_t h = _get_osfhandle(static_cast<int>(obj.fd));
    obj.pad_binder = h == -1 || h == -2 ? 0 : static_cast<binder_uintptr_t>(h);
  });
}

void ReceiveFds(uint8_t* data, size_t data_size, const void* offsets, size_t offsets_size) {
  ForEachFdObject(data, data_size, offsets, offsets_size, [](binder_fd_object& obj) {
    HANDLE h = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(obj.pad_binder));
    int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_RDWR | _O_BINARY);
    if (fd < 0) CloseHandle(h);
    obj.pad_binder = 0;
    obj.fd = static_cast<__u32>(fd);
  });
}

bool IsTransactionCommand(uint32_t cmd) {
  return cmd == BC_TRANSACTION || cmd == BC_REPLY || cmd == BC_TRANSACTION_SG ||
         cmd == BC_REPLY_SG;
}

int WriteRead(const std::shared_ptr<BinderFile>& file, binder_write_read* bwr) {
  HANDLE pipe = ThreadPipe(file);
  if (pipe == INVALID_HANDLE_VALUE) return -1;

  Message m;
  m.Put<uint32_t>(wire::kWriteRead);
  uint64_t read_size = bwr->read_size > bwr->read_consumed ? bwr->read_size - bwr->read_consumed : 0;
  m.Put<uint64_t>(read_size);
  m.Put<uint32_t>(bwr->read_consumed == 0 ? 1 : 0);
  m.Put<uint32_t>(file->non_block ? 1 : 0);

  // The command stream, with the data each transaction points to.
  std::vector<size_t> starts;  // each command's offset in the write buffer
  const uint8_t* wbase = reinterpret_cast<const uint8_t*>(bwr->write_buffer);
  size_t wpos = bwr->write_consumed;
  while (wpos < bwr->write_size) {
    if (bwr->write_size - wpos < 4) return errno = EINVAL, -1;
    starts.push_back(wpos);
    uint32_t cmd;
    memcpy(&cmd, wbase + wpos, 4);
    size_t size = _IOC_SIZE(cmd);
    if (bwr->write_size - wpos - 4 < size) return errno = EINVAL, -1;
    const uint8_t* arg = wbase + wpos + 4;
    m.Put(cmd);
    if (IsTransactionCommand(cmd)) {
      binder_transaction_data tr;
      memcpy(&tr, arg, sizeof(tr));
      const void* data = reinterpret_cast<const void*>(tr.data.ptr.buffer);
      const void* offsets = reinterpret_cast<const void*>(tr.data.ptr.offsets);
      tr.data.ptr.buffer = 0;
      tr.data.ptr.offsets = 0;
      m.Put(tr);
      if (size > sizeof(tr)) m.Write(arg + sizeof(tr), size - sizeof(tr));  // buffers_size
      if (tr.data_size > 0) {
        size_t at = m.bytes.size();
        m.Write(data, tr.data_size);
        SendFds(m.bytes.data() + at, tr.data_size, offsets, tr.offsets_size);
      }
      if (tr.offsets_size > 0) m.Write(offsets, tr.offsets_size);
    } else if (cmd == BC_FREE_BUFFER) {
      binder_uintptr_t ptr;
      memcpy(&ptr, arg, sizeof(ptr));
      m.Put<uint64_t>(file->Free(ptr));
    } else {
      m.Write(arg, size);
    }
    wpos += 4 + size;
  }

  std::vector<uint8_t> reply;
  if (!Send(pipe, m) || !Receive(pipe, &reply)) return errno = EBADF, -1;
  Reader r{reply.data(), reply.data() + reply.size()};
  int32_t status;
  uint32_t consumed;
  if (!r.Get(&status) || !r.Get(&consumed)) return errno = EBADF, -1;
  bwr->write_consumed = consumed >= starts.size() ? bwr->write_size : starts[consumed];
  if (status < 0) {
    bwr->read_consumed = 0;
    return errno = -status, -1;
  }

  // The return stream, each transaction's data copied into the mapping.
  uint8_t* rbase = reinterpret_cast<uint8_t*>(bwr->read_buffer);
  size_t rpos = bwr->read_consumed;
  while (r.p < r.end) {
    uint32_t cmd;
    if (!r.Get(&cmd)) return errno = EBADF, -1;
    size_t size = _IOC_SIZE(cmd);
    if (rpos + 4 + size > bwr->read_size) return errno = EFAULT, -1;
    memcpy(rbase + rpos, &cmd, 4);
    if (cmd == BR_TRANSACTION || cmd == BR_REPLY) {
      binder_transaction_data tr;
      uint64_t id;
      if (!r.Get(&tr) || !r.Get(&id)) return errno = EBADF, -1;
      size_t data_size = static_cast<size_t>(tr.data_size);
      size_t aligned = (data_size + 7) & ~size_t{7};
      uint8_t* block = file->Allocate(aligned + tr.offsets_size, id);
      if (block == nullptr) return errno = ENOMEM, -1;
      if (!r.Read(block, data_size) || !r.Read(block + aligned, tr.offsets_size)) {
        return errno = EBADF, -1;
      }
      ReceiveFds(block, data_size, block + aligned, tr.offsets_size);
      tr.data.ptr.buffer = reinterpret_cast<binder_uintptr_t>(block);
      tr.data.ptr.offsets = reinterpret_cast<binder_uintptr_t>(block + aligned);
      memcpy(rbase + rpos + 4, &tr, sizeof(tr));
    } else if (!r.Read(rbase + rpos + 4, size)) {
      return errno = EBADF, -1;
    }
    rpos += 4 + size;
  }
  bwr->read_consumed = rpos;
  return 0;
}

// An ioctl the broker answers: `in` bytes of the argument go, `out` come back.
int Forward(const std::shared_ptr<BinderFile>& file, uint32_t request, void* arg, size_t in,
            size_t out) {
  HANDLE pipe = ThreadPipe(file);
  if (pipe == INVALID_HANDLE_VALUE) return -1;
  Message m;
  m.Put<uint32_t>(wire::kIoctl);
  m.Put<uint32_t>(request);
  if (in > 0) m.Write(arg, in);
  std::vector<uint8_t> reply;
  if (!Send(pipe, m) || !Receive(pipe, &reply) || reply.size() < 4) return errno = EBADF, -1;
  int32_t status;
  memcpy(&status, reply.data(), 4);
  if (status < 0) return errno = -status, -1;
  if (out > 0 && reply.size() >= 4 + out) memcpy(arg, reply.data() + 4, out);
  return 0;
}

}  // namespace

bool BinderDeviceExists() {
  std::wstring name = PipeName();
  if (name.empty()) return false;
  // WaitNamedPipe fails at once with ERROR_FILE_NOT_FOUND when no server
  // has the pipe; any other answer means one does.
  if (WaitNamedPipeW(name.c_str(), 1)) return true;
  return GetLastError() != ERROR_FILE_NOT_FOUND;
}

int OpenBinder(int flags) {
  std::wstring name = PipeName();
  if (name.empty()) return errno = ENOENT, -1;
  HANDLE pipe = ConnectPipe(name);
  if (pipe == INVALID_HANDLE_VALUE) return errno = ENOENT, -1;
  auto file = std::make_shared<BinderFile>();
  file->pipe_name = name;
  file->proc_pipe = pipe;
  file->non_block = (flags & O_NONBLOCK) != 0;
  Message hello;
  hello.Put<uint32_t>(wire::kHelloProc);
  hello.Put<uint32_t>(wire::kMagic);
  hello.Put<uint32_t>(wire::kVersion);
  hello.Put<uint32_t>(GetCurrentProcessId());
  hello.Put<uint32_t>(getuid());
  std::vector<uint8_t> reply;
  if (!Send(pipe, hello) || !Receive(pipe, &reply)) return errno = EIO, -1;
  Reader r{reply.data(), reply.data() + reply.size()};
  int32_t status;
  if (!r.Get(&status) || status != 0 || !r.Get(&file->proc_id) || !r.Get(&file->key)) {
    return errno = EIO, -1;
  }
  int fd = ReserveFd();
  if (fd < 0) return -1;
  std::lock_guard<std::mutex> lock(files_mutex);
  files[fd] = std::move(file);
  return fd;
}

bool IsBinderFd(int fd) { return Lookup(fd) != nullptr; }

void SetBinderMapping(int fd, void* addr, size_t size) {
  std::shared_ptr<BinderFile> file = Lookup(fd);
  if (file == nullptr) return;
  std::lock_guard<std::mutex> lock(file->mutex);
  if (file->map != nullptr) return;  // the first mapping is the receive buffer
  file->map = static_cast<uint8_t*>(addr);
  file->map_size = size;
  file->free_blocks.clear();
  file->free_blocks[0] = size;
}

bool BinderIoctl(int fd, unsigned long request, void* arg, int* result) {
  std::shared_ptr<BinderFile> file = Lookup(fd);
  if (file == nullptr) return false;
  uint32_t req = static_cast<uint32_t>(request);
  switch (req) {
    case BINDER_WRITE_READ:
      *result = WriteRead(file, static_cast<binder_write_read*>(arg));
      break;
    case BINDER_VERSION:
      static_cast<binder_version*>(arg)->protocol_version = BINDER_CURRENT_PROTOCOL_VERSION;
      *result = 0;
      break;
    case BINDER_SET_MAX_THREADS:
      *result = Forward(file, req, arg, sizeof(uint32_t), 0);
      break;
    case BINDER_SET_CONTEXT_MGR:
      *result = Forward(file, req, arg, 0, 0);
      break;
    case BINDER_SET_CONTEXT_MGR_EXT:
      *result = Forward(file, req, arg, sizeof(flat_binder_object), 0);
      break;
    case BINDER_GET_NODE_INFO_FOR_REF:
      *result = Forward(file, req, arg, sizeof(binder_node_info_for_ref),
                        sizeof(binder_node_info_for_ref));
      break;
    case BINDER_THREAD_EXIT:
      *result = Forward(file, req, arg, 0, 0);
      CloseThreadPipe(file.get());
      break;
    case BINDER_ENABLE_ONEWAY_SPAM_DETECTION:
    case BINDER_SET_IDLE_TIMEOUT:
    case BINDER_SET_IDLE_PRIORITY:
      *result = 0;
      break;
    case BINDER_GET_EXTENDED_ERROR: {
      binder_extended_error* ee = static_cast<binder_extended_error*>(arg);
      ee->id = 0;
      ee->command = BR_OK;
      ee->param = 0;
      *result = 0;
      break;
    }
    default:
      errno = EINVAL;
      *result = -1;
      break;
  }
  return true;
}

bool ForgetBinder(int fd) {
  std::shared_ptr<BinderFile> file;
  {
    std::lock_guard<std::mutex> lock(files_mutex);
    if (files.empty()) return false;
    auto it = files.find(fd);
    if (it == files.end()) return false;
    file = std::move(it->second);
    files.erase(it);
  }
  return true;  // the last reference closes the process's connection
}

void DupBinder(int fd, int fd2) {
  std::shared_ptr<BinderFile> file = Lookup(fd);
  if (file == nullptr) return;
  std::lock_guard<std::mutex> lock(files_mutex);
  files[fd2] = std::move(file);
}

}  // namespace gesher
