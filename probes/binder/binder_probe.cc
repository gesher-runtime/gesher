// /dev/binder across processes (gesher-base's binder.cc over binderd), at
// the driver's ABI, as libbinder's IPCThreadState speaks it. Run in CI on
// Windows.
//
//   binder_probe              starts binderd (next to this program) on a
//                             device of its own, then a server and a client
//                             process, and reports
//   binder_probe server|client  one side (GESHER_BINDER names the device)
//
// The server becomes the context manager. The client calls it (handle 0)
// with a binder object of its own; the server, while serving that call,
// calls the object back (a nested call, which must reach the client's
// waiting thread), then replies with an object of its own and the client's
// object passed back, which must arrive as the client's own pointer. The
// client then asks for a death notification on the server's object and
// tells the server to exit: BR_DEAD_BINDER must follow, and a call to the
// dead object must fail with BR_DEAD_REPLY.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <windows.h>

#include <string>
#include <vector>

#include <linux/android/binder.h>

namespace {

int failures = 0;
const char* role = "probe";

#define EXPECT(cond)                                                          \
  do {                                                                        \
    if (!(cond)) {                                                            \
      fprintf(stderr, "%s: FAILED %s (line %d)\n", role, #cond, __LINE__);    \
      fflush(stderr);                                                         \
      ++failures;                                                             \
    }                                                                         \
  } while (0)

#define REQUIRE(cond)                                                         \
  do {                                                                        \
    if (!(cond)) {                                                            \
      fprintf(stderr, "%s: FAILED %s (line %d), giving up\n", role, #cond,   \
              __LINE__);                                                      \
      fflush(stderr);                                                         \
      exit(1);                                                                \
    }                                                                         \
  } while (0)

constexpr uint64_t kClientPtr = 0xC0FFEE00;
constexpr uint64_t kClientCookie = 0xC00C1E00;
constexpr uint64_t kServerPtr = 0x5E4E4000;
constexpr uint64_t kServerCookie = 0x5E4E4C00;
constexpr uint64_t kDeathCookie = 0xDEAD0000;
constexpr uint32_t kCodeHello = 1;
constexpr uint32_t kCodePing = 2;
constexpr uint32_t kCodeExit = 3;

// One returned command, its argument copied.
struct Command {
  uint32_t cmd;
  binder_transaction_data tr;  // BR_TRANSACTION, BR_REPLY
  binder_ptr_cookie pc;        // BR_INCREFS ... BR_DECREFS
  uint64_t cookie;             // BR_DEAD_BINDER, BR_CLEAR_DEATH_NOTIFICATION_DONE
};

struct Binder {
  int fd = -1;
  std::vector<uint8_t> out;

  void Open() {
    fd = open("/dev/binder", O_RDWR | O_CLOEXEC);
    REQUIRE(fd >= 0);
    binder_version version = {};
    REQUIRE(ioctl(fd, BINDER_VERSION, &version) == 0);
    EXPECT(version.protocol_version == BINDER_CURRENT_PROTOCOL_VERSION);
    void* map = mmap(nullptr, 1 << 20, PROT_READ, MAP_PRIVATE | MAP_NORESERVE, fd, 0);
    REQUIRE(map != MAP_FAILED);
    uint32_t max_threads = 0;
    EXPECT(ioctl(fd, BINDER_SET_MAX_THREADS, &max_threads) == 0);
  }

  void Put32(uint32_t v) { Write(&v, 4); }
  void Put64(uint64_t v) { Write(&v, 8); }
  void Write(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    out.insert(out.end(), b, b + n);
  }

  void Transaction(uint32_t cmd, uint32_t handle, uint32_t code, uint32_t flags,
                   const std::vector<uint8_t>& data, const std::vector<binder_size_t>& offsets) {
    binder_transaction_data tr = {};
    tr.target.handle = handle;
    tr.code = code;
    tr.flags = flags;
    tr.data_size = data.size();
    tr.offsets_size = offsets.size() * sizeof(binder_size_t);
    // The driver copies the data during the ioctl; these live until then.
    pending_data = data;
    pending_offsets = offsets;
    tr.data.ptr.buffer = reinterpret_cast<binder_uintptr_t>(pending_data.data());
    tr.data.ptr.offsets = reinterpret_cast<binder_uintptr_t>(pending_offsets.data());
    Put32(cmd);
    Write(&tr, sizeof(tr));
  }
  std::vector<uint8_t> pending_data;
  std::vector<binder_size_t> pending_offsets;

  // One BINDER_WRITE_READ: writes what was queued, reads what comes.
  std::vector<Command> Talk(bool read = true) {
    uint8_t in[512];
    binder_write_read bwr = {};
    bwr.write_buffer = reinterpret_cast<binder_uintptr_t>(out.data());
    bwr.write_size = out.size();
    bwr.read_buffer = reinterpret_cast<binder_uintptr_t>(in);
    bwr.read_size = read ? sizeof(in) : 0;
    int r = ioctl(fd, BINDER_WRITE_READ, &bwr);
    REQUIRE(r == 0);
    REQUIRE(bwr.write_consumed == out.size());
    out.clear();
    std::vector<Command> cmds;
    size_t pos = 0;
    while (pos < bwr.read_consumed) {
      Command c = {};
      memcpy(&c.cmd, in + pos, 4);
      size_t size = _IOC_SIZE(c.cmd);
      if (c.cmd == BR_TRANSACTION || c.cmd == BR_REPLY) memcpy(&c.tr, in + pos + 4, sizeof(c.tr));
      if (c.cmd == BR_INCREFS || c.cmd == BR_ACQUIRE || c.cmd == BR_RELEASE ||
          c.cmd == BR_DECREFS) {
        memcpy(&c.pc, in + pos + 4, sizeof(c.pc));
      }
      if (c.cmd == BR_DEAD_BINDER || c.cmd == BR_CLEAR_DEATH_NOTIFICATION_DONE) {
        memcpy(&c.cookie, in + pos + 4, 8);
      }
      pos += 4 + size;
      if (c.cmd != BR_NOOP) cmds.push_back(c);
    }
    return cmds;
  }

  // Answers reference count commands as IPCThreadState does.
  bool Refcount(const Command& c) {
    if (c.cmd == BR_INCREFS || c.cmd == BR_ACQUIRE) {
      Put32(c.cmd == BR_INCREFS ? BC_INCREFS_DONE : BC_ACQUIRE_DONE);
      Write(&c.pc, sizeof(c.pc));
      return true;
    }
    return c.cmd == BR_RELEASE || c.cmd == BR_DECREFS;
  }

  void FreeBuffer(const binder_transaction_data& tr) {
    Put32(BC_FREE_BUFFER);
    Put64(tr.data.ptr.buffer);
  }
};

std::vector<uint8_t> Payload(const char* text) {
  return std::vector<uint8_t>(text, text + strlen(text) + 1);
}

// A transaction's data, with its objects.
struct Data {
  std::vector<uint8_t> bytes;
  std::vector<binder_size_t> offsets;
  void Object(uint32_t type, uint64_t binder_or_handle, uint64_t cookie) {
    flat_binder_object obj = {};
    obj.hdr.type = type;
    obj.flags = FLAT_BINDER_FLAG_ACCEPTS_FDS | 0x7f;
    if (type == BINDER_TYPE_HANDLE || type == BINDER_TYPE_WEAK_HANDLE) {
      obj.handle = static_cast<uint32_t>(binder_or_handle);
    } else {
      obj.binder = binder_or_handle;
    }
    obj.cookie = cookie;
    offsets.push_back(bytes.size());
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&obj);
    bytes.insert(bytes.end(), b, b + sizeof(obj));
  }
};

// Set by the server once it is the context manager.
HANDLE ReadyEvent() {
  std::string name = "gesher-binder-probe-ready-";
  const char* device = getenv("GESHER_BINDER");
  name += device != nullptr ? device : "";
  return CreateEventA(nullptr, TRUE, FALSE, name.c_str());
}

flat_binder_object ObjectAt(const binder_transaction_data& tr, size_t i) {
  const binder_size_t* offsets = reinterpret_cast<const binder_size_t*>(tr.data.ptr.offsets);
  flat_binder_object obj;
  memcpy(&obj, reinterpret_cast<const uint8_t*>(tr.data.ptr.buffer) + offsets[i], sizeof(obj));
  return obj;
}

// --- the server ------------------------------------------------------------------

int Server() {
  role = "server";
  Binder b;
  b.Open();
  flat_binder_object ctx = {};
  ctx.flags = FLAT_BINDER_FLAG_TXN_SECURITY_CTX;
  REQUIRE(ioctl(b.fd, BINDER_SET_CONTEXT_MGR_EXT, &ctx) == 0);
  EXPECT(ioctl(b.fd, BINDER_SET_CONTEXT_MGR_EXT, &ctx) == -1 && errno == EBUSY);
  SetEvent(ReadyEvent());
  b.Put32(BC_ENTER_LOOPER);
  bool exiting = false;
  for (;;) {
    std::vector<Command> cmds = b.Talk();
    for (const Command& c : cmds) {
      if (b.Refcount(c)) continue;
      if (c.cmd == BR_TRANSACTION_COMPLETE) continue;
      if (c.cmd != BR_TRANSACTION) {
        fprintf(stderr, "server: unexpected command %08x\n", c.cmd);
        ++failures;
        continue;
      }
      const binder_transaction_data& tr = c.tr;
      EXPECT(tr.target.ptr == 0);  // the context manager's node
      EXPECT(tr.sender_pid != 0 && tr.sender_pid != static_cast<pid_t>(GetCurrentProcessId()));
      if (tr.code == kCodeHello) {
        EXPECT(tr.offsets_size == sizeof(binder_size_t));
        flat_binder_object obj = ObjectAt(tr, 0);
        EXPECT(obj.hdr.type == BINDER_TYPE_HANDLE);
        uint32_t client = obj.handle;
        EXPECT(client != 0);
        EXPECT(strcmp(reinterpret_cast<const char*>(tr.data.ptr.buffer) + sizeof(obj),
                      "hello") == 0);
        b.Put32(BC_INCREFS);
        b.Put32(client);
        b.Put32(BC_ACQUIRE);
        b.Put32(client);
        // The nested call: back to the client, whose thread waits for this
        // call's reply.
        b.Transaction(BC_TRANSACTION, client, kCodePing, 0, Payload("ping"), {});
        bool replied = false;
        while (!replied) {
          for (const Command& n : b.Talk()) {
            if (b.Refcount(n) || n.cmd == BR_TRANSACTION_COMPLETE) continue;
            REQUIRE(n.cmd == BR_REPLY);
            EXPECT(strcmp(reinterpret_cast<const char*>(n.tr.data.ptr.buffer), "pong") == 0);
            b.FreeBuffer(n.tr);
            replied = true;
          }
        }
        // The reply: an object of the server's, and the client's own.
        Data reply;
        reply.Object(BINDER_TYPE_BINDER, kServerPtr, kServerCookie);
        reply.Object(BINDER_TYPE_HANDLE, client, 0);
        b.FreeBuffer(tr);
        b.Transaction(BC_REPLY, 0, 0, 0, reply.bytes, reply.offsets);
        b.Put32(BC_RELEASE);
        b.Put32(client);
        b.Put32(BC_DECREFS);
        b.Put32(client);
      } else if (tr.code == kCodeExit) {
        b.FreeBuffer(tr);
        b.Transaction(BC_REPLY, 0, 0, 0, Payload("bye"), {});
        exiting = true;
      } else {
        fprintf(stderr, "server: unexpected code %u\n", tr.code);
        ++failures;
      }
    }
    if (exiting) {
      b.Talk(false);  // send the reply
      break;
    }
  }
  printf("server: %d failure(s)\n", failures);
  fflush(stdout);
  return failures == 0 ? 0 : 1;
}

// --- the client --------------------------------------------------------------------

int Client() {
  role = "client";
  Binder b;
  b.Open();
  b.Put32(BC_INCREFS);
  b.Put32(0);
  b.Put32(BC_ACQUIRE);
  b.Put32(0);

  // hello, with an object of the client's.
  Data hello;
  hello.Object(BINDER_TYPE_BINDER, kClientPtr, kClientCookie);
  std::vector<uint8_t> text = Payload("hello");
  hello.bytes.insert(hello.bytes.end(), text.begin(), text.end());
  b.Transaction(BC_TRANSACTION, 0, kCodeHello, TF_ACCEPT_FDS, hello.bytes, hello.offsets);
  bool got_ping = false, got_acquire = false, got_increfs = false;
  binder_transaction_data reply = {};
  bool replied = false;
  while (!replied) {
    for (const Command& c : b.Talk()) {
      if (c.cmd == BR_INCREFS || c.cmd == BR_ACQUIRE) {
        EXPECT(c.pc.ptr == kClientPtr && c.pc.cookie == kClientCookie);
        (c.cmd == BR_INCREFS ? got_increfs : got_acquire) = true;
      }
      if (b.Refcount(c) || c.cmd == BR_TRANSACTION_COMPLETE) continue;
      if (c.cmd == BR_TRANSACTION) {
        // The nested call, on this thread.
        EXPECT(c.tr.code == kCodePing);
        EXPECT(c.tr.target.ptr == kClientPtr && c.tr.cookie == kClientCookie);
        EXPECT(strcmp(reinterpret_cast<const char*>(c.tr.data.ptr.buffer), "ping") == 0);
        got_ping = true;
        b.FreeBuffer(c.tr);
        b.Transaction(BC_REPLY, 0, 0, 0, Payload("pong"), {});
        continue;
      }
      REQUIRE(c.cmd == BR_REPLY);
      reply = c.tr;
      replied = true;
    }
  }
  EXPECT(got_increfs && got_acquire);
  EXPECT(got_ping);
  EXPECT(reply.offsets_size == 2 * sizeof(binder_size_t));
  flat_binder_object server = ObjectAt(reply, 0);
  flat_binder_object mine = ObjectAt(reply, 1);
  EXPECT(server.hdr.type == BINDER_TYPE_HANDLE && server.handle != 0);
  EXPECT(mine.hdr.type == BINDER_TYPE_BINDER && mine.binder == kClientPtr &&
         mine.cookie == kClientCookie);
  uint32_t handle = server.handle;
  b.Put32(BC_INCREFS);
  b.Put32(handle);
  b.Put32(BC_ACQUIRE);
  b.Put32(handle);
  b.FreeBuffer(reply);
  // A death notification on the server's object.
  b.Put32(BC_REQUEST_DEATH_NOTIFICATION);
  binder_handle_cookie hc = {handle, kDeathCookie};
  b.Write(&hc, sizeof(hc));
  b.Talk(false);

  // exit: the server replies and exits.
  b.Transaction(BC_TRANSACTION, 0, kCodeExit, 0, Payload("exit"), {});
  replied = false;
  while (!replied) {
    for (const Command& c : b.Talk()) {
      if (b.Refcount(c) || c.cmd == BR_TRANSACTION_COMPLETE) continue;
      REQUIRE(c.cmd == BR_REPLY);
      EXPECT(strcmp(reinterpret_cast<const char*>(c.tr.data.ptr.buffer), "bye") == 0);
      b.FreeBuffer(c.tr);
      replied = true;
    }
  }

  // BR_DEAD_BINDER, delivered to a looper; the client becomes one.
  b.Put32(BC_ENTER_LOOPER);
  bool dead = false;
  for (int i = 0; i < 100 && !dead; ++i) {
    for (const Command& c : b.Talk()) {
      if (b.Refcount(c)) continue;
      if (c.cmd == BR_DEAD_BINDER) {
        EXPECT(c.cookie == kDeathCookie);
        dead = true;
        b.Put32(BC_DEAD_BINDER_DONE);
        b.Put64(c.cookie);
      }
    }
  }
  EXPECT(dead);

  // A call to the dead object fails, and so does one to handle 0.
  b.Put32(BC_EXIT_LOOPER);
  b.Transaction(BC_TRANSACTION, handle, kCodePing, 0, Payload("ping"), {});
  bool dead_reply = false;
  for (const Command& c : b.Talk()) dead_reply |= c.cmd == BR_DEAD_REPLY;
  EXPECT(dead_reply);
  b.Transaction(BC_TRANSACTION, 0, kCodePing, 0, Payload("ping"), {});
  dead_reply = false;
  for (const Command& c : b.Talk()) dead_reply |= c.cmd == BR_DEAD_REPLY;
  EXPECT(dead_reply);

  b.Put32(BC_CLEAR_DEATH_NOTIFICATION);
  b.Write(&hc, sizeof(hc));
  bool cleared = false;
  for (const Command& c : b.Talk()) cleared |= c.cmd == BR_CLEAR_DEATH_NOTIFICATION_DONE;
  EXPECT(cleared);
  EXPECT(ioctl(b.fd, BINDER_THREAD_EXIT, 0) == 0);
  close(b.fd);
  printf("client: %d failure(s)\n", failures);
  fflush(stdout);
  return failures == 0 ? 0 : 1;
}

// --- the parent ------------------------------------------------------------------

HANDLE Spawn(const std::wstring& exe, const std::wstring& args) {
  std::wstring cmdline = L"\"" + exe + L"\" " + args;
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                      &si, &pi)) {
    fprintf(stderr, "probe: cannot start %ls (%lu)\n", exe.c_str(), GetLastError());
    return nullptr;
  }
  CloseHandle(pi.hThread);
  return pi.hProcess;
}

DWORD Wait(HANDLE process, DWORD ms) {
  if (WaitForSingleObject(process, ms) != WAIT_OBJECT_0) {
    TerminateProcess(process, 99);
    WaitForSingleObject(process, INFINITE);
    return 99;
  }
  DWORD code = 1;
  GetExitCodeProcess(process, &code);
  return code;
}

int Parent() {
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  std::wstring dir = self;
  dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
  char device[64];
  snprintf(device, sizeof(device), "probe-%lu", GetCurrentProcessId());
  std::string env = std::string("GESHER_BINDER=") + device;
  _putenv(env.c_str());  // the C runtime's environment, and the children's

  // No device until a binderd serves it.
  EXPECT(open("/dev/binder", O_RDWR) == -1 && errno == ENOENT);

  HANDLE binderd = Spawn(dir + L"binderd.exe", std::wstring(device, device + strlen(device)));
  REQUIRE(binderd != nullptr);
  int fd = -1;
  for (int i = 0; i < 100 && fd < 0; ++i) {
    fd = open("/dev/binder", O_RDWR);
    if (fd < 0) Sleep(50);
  }
  REQUIRE(fd >= 0);
  close(fd);

  HANDLE ready = ReadyEvent();
  HANDLE server = Spawn(self, L"server");
  REQUIRE(server != nullptr);
  // The client starts once the server is the context manager.
  REQUIRE(WaitForSingleObject(ready, 30000) == WAIT_OBJECT_0);
  HANDLE client = Spawn(self, L"client");
  REQUIRE(client != nullptr);
  DWORD client_code = Wait(client, 60000);
  DWORD server_code = Wait(server, 10000);
  TerminateProcess(binderd, 0);
  WaitForSingleObject(binderd, INFINITE);
  printf("probe: server exit %lu, client exit %lu\n", server_code, client_code);
  EXPECT(server_code == 0);
  EXPECT(client_code == 0);
  if (failures == 0) {
    printf("binder probe: all passed\n");
  } else {
    printf("binder probe: %d failure(s)\n", failures);
  }
  return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc > 1 && strcmp(argv[1], "server") == 0) return Server();
  if (argc > 1 && strcmp(argv[1], "client") == 0) return Client();
  return Parent();
}
