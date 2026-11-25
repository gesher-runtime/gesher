// gesher-base: process creation for forked children (gesher/fork.h, ADR 0092).
//
// A process RtlCloneUserProcess made cannot call CreateProcess: kernelbase
// reads the CSR shared section (BaseStaticServerData) that csrss maps into a
// process it starts, and a clone has no copy of that mapping (an access
// violation reading it). The first process that forks (the root, which was
// started normally) serves a named pipe; a forked child (or its own forked
// children) sends it the CreateProcess request, and the root creates the
// process with PROC_THREAD_ATTRIBUTE_PARENT_PROCESS set to the requester:
// the new process is the requester's child (its parent pid, its inherited
// handles, interpreted in the requester's handle table, its standard
// handles), as if the requester had created it. The process and thread
// handles are duplicated into the requester.
//
// Who may ask. The boundaries are the user, the AppContainer and the
// integrity level: the pipe's DACL grants only this process's user (and its
// AppContainer, in the sandbox), at its integrity level. Code of the same
// user, container and level could open the root itself with full access, so
// the broker gives it nothing it does not have already; what the broker keeps
// out is confusion, not that code. The root serves only a requester that is a
// forked descendant of it (each fork in its tree records the child's pid and
// creation time in a table the tree shares) and that sends a 128-bit secret
// the root drew, which its clones inherit in memory. The pipe's name carries
// another random value (names can be listed, so the name proves nothing),
// its first instance is made with FILE_FLAG_FIRST_PIPE_INSTANCE, and the pipe
// allows eight instances, all of which the root makes at once: no instance is
// left for another process to add, so no other process can serve the name.
// (The DACL then also drops the right to add instances; the owner keeps
// WRITE_DAC, so that is not the barrier, the instance limit is.) A client
// checks that the pipe's server is the root (its pid, while the client holds a
// handle to the root, so the pid cannot be another process's). A process of
// the same user can still keep the instances busy (denial of service); that is
// outside this model.
//
// When the root ends. Clones do not inherit the broker's pipe instances
// (CloneProcess leaves IsBrokerHandle's out), so the pipe ends with the root;
// a client waits for its reply and for the root together, and a request after
// the root ended fails with ERROR_NOT_SUPPORTED (ENOTSUP).
#include <gesher/fork.h>

#include <errno.h>
#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace gesher {
namespace {

constexpr uint32_t kMagic = 0x32535047;   // "GPS2"
constexpr DWORD kMaxRequest = 1u << 20;   // bytes; larger requests are E2BIG
constexpr int kInstances = 8;             // requests served at once
constexpr DWORD kRequestMs = 5000;        // for a client to send its request
constexpr DWORD kReplyMs = 5000;          // for it to take the reply and close
constexpr DWORD kConnectMs = 30000;       // for a client to find a free instance
constexpr DWORD kServeMs = 120000;        // for the root to answer

struct RequestHeader {
  uint32_t magic;
  uint32_t requester;  // the requester's pid (checked against the pipe's client)
  uint8_t secret[16];
  uint32_t flags;      // CreateProcess creation flags
  uint32_t application_chars, command_chars, cwd_chars, environment_chars;
  uint32_t reserved2_bytes;
  uint32_t inherit_count;
  uint32_t startup_flags;
  uint64_t std_handles[3];
};
struct Reply {
  uint32_t error;  // Win32 error, 0 on success
  uint32_t pid, tid;
  uint64_t process, thread;  // handle values in the requester
};

// The forked descendants of the root, in a section the root makes and its
// clones share (a shared view stays shared in a clone).
struct TreeEntry {
  volatile LONG pid;
  volatile LONG64 created;
};
constexpr size_t kTreeEntries = 8192;
struct Shared {
  volatile LONG broker_error;  // why the broker stopped (Win32), 0 while it serves
  volatile LONG reported;      // the stop was reported (once per tree)
  TreeEntry entries[kTreeEntries];
};

// What the root sets up once; its clones have copies (plain memory).
struct Broker {
  wchar_t pipe[160];
  uint8_t secret[16];
  DWORD root_pid;
  LONG64 root_created;
  HANDLE root;  // SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION; inherited by clones
  Shared* shared;
  // The root's pipe instances and their events (never inherited by a clone).
  HANDLE instance[kInstances];
  HANDLE event[kInstances];
};
Broker g_broker{};
INIT_ONCE g_broker_once = INIT_ONCE_STATIC_INIT;

LONG64 CreationTime(HANDLE process) {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
  return static_cast<LONG64>((static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime);
}

void Append(std::vector<uint8_t>* out, const void* data, size_t bytes) {
  if (bytes == 0) return;
  const size_t at = out->size();
  out->resize(at + bytes);
  std::memcpy(out->data() + at, data, bytes);
}

bool SameSecret(const uint8_t* a, const uint8_t* b) {
  uint8_t diff = 0;
  for (int i = 0; i < 16; ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  return diff == 0;
}

bool InTree(DWORD pid, LONG64 created) {
  if (g_broker.shared == nullptr || created == 0) return false;
  for (const TreeEntry& e : g_broker.shared->entries) {
    if (static_cast<DWORD>(e.pid) == pid && e.created == created) return true;
  }
  return false;
}

// Reports once (per tree) that the broker stopped: its spawns then fail with
// ENOTSUP, which alone would not say why.
void ReportStopped() {
  Shared* shared = g_broker.shared;
  if (shared == nullptr || InterlockedExchange(&shared->reported, 1) != 0) return;
  std::fprintf(stderr,
               "gesher: process creation for forked children is unavailable: the spawn broker of process %lu "
               "stopped (Windows error %ld)\n",
               static_cast<unsigned long>(g_broker.root_pid), static_cast<long>(shared->broker_error));
  std::fflush(stderr);
}

void Stopped(DWORD error) {
  if (g_broker.shared != nullptr) InterlockedCompareExchange(&g_broker.shared->broker_error, error ? error : 1, 0);
  ReportStopped();
}

// --- the root: serving requests ---------------------------------------------------

// Reads one byte range of a request: the offset and size are checked first.
bool Slice(const std::vector<uint8_t>& request, size_t* offset, size_t bytes, const uint8_t** out) {
  if (bytes > request.size() - *offset) return false;
  *out = request.data() + *offset;
  *offset += bytes;
  return true;
}

DWORD Serve(const std::vector<uint8_t>& request, HANDLE pipe, Reply* reply) {
  if (request.size() < sizeof(RequestHeader)) return ERROR_INVALID_DATA;
  RequestHeader h;
  std::memcpy(&h, request.data(), sizeof h);
  if (h.magic != kMagic || !SameSecret(h.secret, g_broker.secret)) return ERROR_ACCESS_DENIED;
  if (h.inherit_count > 4096) return ERROR_INVALID_DATA;
  // The requester: the pipe's client, opened before its creation time is read
  // (a pid reused meanwhile is another process, with another creation time).
  ULONG client = 0;
  if (!GetNamedPipeClientProcessId(pipe, &client) || client != h.requester) return ERROR_ACCESS_DENIED;
  HANDLE requester = OpenProcess(PROCESS_CREATE_PROCESS | PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION,
                                 FALSE, client);
  if (requester == nullptr) return ERROR_ACCESS_DENIED;
  if (!InTree(client, CreationTime(requester))) {
    CloseHandle(requester);
    return ERROR_ACCESS_DENIED;
  }
  size_t offset = sizeof h;
  const uint8_t* bytes = nullptr;
  auto take_wide = [&](uint32_t chars, std::wstring* out) {
    if (!Slice(request, &offset, size_t{chars} * sizeof(wchar_t), &bytes)) return false;
    out->assign(chars, L'\0');
    if (chars != 0) std::memcpy(out->data(), bytes, size_t{chars} * sizeof(wchar_t));
    return true;
  };
  std::wstring application, command, cwd, environment;
  std::vector<uint8_t> reserved2;
  std::vector<HANDLE> inherit(h.inherit_count);
  bool parsed = take_wide(h.application_chars, &application) && take_wide(h.command_chars, &command) &&
                take_wide(h.cwd_chars, &cwd) && take_wide(h.environment_chars, &environment) &&
                Slice(request, &offset, h.reserved2_bytes, &bytes);
  if (parsed) reserved2.assign(bytes, bytes + h.reserved2_bytes);
  for (uint32_t i = 0; parsed && i < h.inherit_count; ++i) {
    uint64_t v;
    parsed = Slice(request, &offset, sizeof v, &bytes);
    if (!parsed) break;
    std::memcpy(&v, bytes, sizeof v);
    inherit[i] = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(v));
  }
  if (!parsed || offset != request.size() || command.empty() || command.back() != L'\0' ||
      reserved2.size() > 0xFFFF) {
    CloseHandle(requester);
    return ERROR_INVALID_DATA;
  }
  SIZE_T list_bytes = 0;
  const DWORD attributes = inherit.empty() ? 1 : 2;
  InitializeProcThreadAttributeList(nullptr, attributes, 0, &list_bytes);
  std::vector<uint8_t> storage(list_bytes);
  auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  DWORD error = ERROR_SUCCESS;
  if (!InitializeProcThreadAttributeList(list, attributes, 0, &list_bytes) ||
      !UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &requester, sizeof requester,
                                 nullptr, nullptr) ||
      (!inherit.empty() &&
       !UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.data(),
                                  inherit.size() * sizeof(HANDLE), nullptr, nullptr))) {
    error = GetLastError();
    CloseHandle(requester);
    return error;
  }
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof startup;
  startup.StartupInfo.dwFlags = h.startup_flags;
  startup.StartupInfo.hStdInput = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(h.std_handles[0]));
  startup.StartupInfo.hStdOutput = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(h.std_handles[1]));
  startup.StartupInfo.hStdError = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(h.std_handles[2]));
  startup.StartupInfo.cbReserved2 = static_cast<WORD>(reserved2.size());
  startup.StartupInfo.lpReserved2 = reserved2.empty() ? nullptr : reserved2.data();
  startup.lpAttributeList = list;
  PROCESS_INFORMATION info{};
  if (!CreateProcessW(application.empty() ? nullptr : application.c_str(), command.data(), nullptr, nullptr,
                      inherit.empty() ? FALSE : TRUE, h.flags | EXTENDED_STARTUPINFO_PRESENT,
                      environment.empty() ? nullptr : environment.data(), cwd.empty() ? nullptr : cwd.c_str(),
                      &startup.StartupInfo, &info)) {
    error = GetLastError();
  } else {
    HANDLE process = nullptr, thread = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), info.hProcess, requester, &process, 0, FALSE,
                         DUPLICATE_SAME_ACCESS) ||
        !DuplicateHandle(GetCurrentProcess(), info.hThread, requester, &thread, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
      error = GetLastError();
      TerminateProcess(info.hProcess, 1);
    } else {
      reply->pid = info.dwProcessId;
      reply->tid = info.dwThreadId;
      reply->process = reinterpret_cast<uintptr_t>(process);
      reply->thread = reinterpret_cast<uintptr_t>(thread);
    }
    CloseHandle(info.hProcess);
    CloseHandle(info.hThread);
  }
  DeleteProcThreadAttributeList(list);
  CloseHandle(requester);
  return error;
}

// Waits for an overlapped operation on `pipe` up to `ms`; cancels it on a
// timeout. Returns the bytes moved, or false.
bool Finish(HANDLE pipe, OVERLAPPED* ov, BOOL started, DWORD ms, DWORD* moved, DWORD* error) {
  if (!started) {
    *error = GetLastError();
    if (*error != ERROR_IO_PENDING) return false;
    if (WaitForSingleObject(ov->hEvent, ms) != WAIT_OBJECT_0) {
      CancelIoEx(pipe, ov);
      GetOverlappedResult(pipe, ov, moved, TRUE);
      *error = ERROR_TIMEOUT;
      return false;
    }
  }
  if (GetOverlappedResult(pipe, ov, moved, FALSE)) return true;
  *error = GetLastError();
  return false;
}

// One connected client: its request within kRequestMs, the reply, then the
// client's close within kReplyMs. A client that stalls holds only its instance.
void ServeClient(HANDLE pipe, HANDLE event) {
  std::vector<uint8_t> request;
  std::vector<uint8_t> chunk(65536);
  const ULONGLONG deadline = GetTickCount64() + kRequestMs;
  bool complete = false;
  for (;;) {
    OVERLAPPED ov{};
    ov.hEvent = event;
    DWORD n = 0, error = 0;
    const ULONGLONG now = GetTickCount64();
    const DWORD left = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
    const bool ok = Finish(pipe, &ov, ReadFile(pipe, chunk.data(), static_cast<DWORD>(chunk.size()), &n, &ov),
                           left, &n, &error);
    if (request.size() + n > kMaxRequest) break;
    request.insert(request.end(), chunk.begin(), chunk.begin() + n);
    if (ok) {
      complete = true;
      break;
    }
    if (error != ERROR_MORE_DATA) break;
  }
  if (!complete) return;  // garbage, too large or too slow: no reply
  Reply reply{};
  {
    std::lock_guard<std::mutex> helper(ForkHelperLock());  // not while a fork clones
    reply.error = Serve(request, pipe, &reply);
  }
  OVERLAPPED ov{};
  ov.hEvent = event;
  DWORD n = 0, error = 0;
  if (!Finish(pipe, &ov, WriteFile(pipe, &reply, sizeof reply, &n, &ov), kReplyMs, &n, &error)) return;
  // Wait for the client to close (it has read the reply), so that disconnecting
  // does not discard the reply; a client that never does is let go.
  uint8_t byte;
  OVERLAPPED done{};
  done.hEvent = event;
  Finish(pipe, &done, ReadFile(pipe, &byte, 1, &n, &done), kReplyMs, &n, &error);
}

DWORD WINAPI InstanceThread(void* raw) {
  const int i = static_cast<int>(reinterpret_cast<intptr_t>(raw));
  HANDLE pipe = g_broker.instance[i];
  HANDLE event = g_broker.event[i];
  for (;;) {
    OVERLAPPED ov{};
    ov.hEvent = event;
    DWORD n = 0;
    bool connected = ConnectNamedPipe(pipe, &ov) != FALSE;
    if (!connected) {
      DWORD error = GetLastError();
      if (error == ERROR_IO_PENDING) {
        if (WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0) {
          Stopped(GetLastError());
          return 1;
        }
        connected = GetOverlappedResult(pipe, &ov, &n, FALSE) != FALSE;
        if (!connected) error = GetLastError();
      }
      if (error == ERROR_PIPE_CONNECTED) connected = true;
      if (!connected && error != ERROR_NO_DATA && error != ERROR_IO_PENDING && error != ERROR_BROKEN_PIPE &&
          error != ERROR_OPERATION_ABORTED) {
        Stopped(error);  // the instance itself is unusable
        return 1;
      }
    }
    if (connected) ServeClient(pipe, event);
    DisconnectNamedPipe(pipe);  // a client that came and went, or one served
  }
}

// The pipe's DACL: this process's user and, in an AppContainer, the container.
// `instances`: also the right to make pipe instances (FILE_CREATE_PIPE_INSTANCE).
bool MakeDacl(bool instances, std::vector<uint8_t>* acl_storage) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  std::vector<uint8_t> user(256), container(256);
  DWORD size = 0;
  bool ok = GetTokenInformation(token, TokenUser, user.data(), static_cast<DWORD>(user.size()), &size) != FALSE;
  DWORD is_container = 0;
  GetTokenInformation(token, TokenIsAppContainer, &is_container, sizeof is_container, &size);
  PSID container_sid = nullptr;
  if (ok && is_container &&
      GetTokenInformation(token, TokenAppContainerSid, container.data(), static_cast<DWORD>(container.size()),
                          &size)) {
    container_sid = reinterpret_cast<TOKEN_APPCONTAINER_INFORMATION*>(container.data())->TokenAppContainer;
  }
  CloseHandle(token);
  if (!ok) return false;
  PSID user_sid = reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;
  // Making an instance opens the pipe with the server's full access; a client
  // needs only to read, write and set its read mode.
  const DWORD access = instances ? FILE_GENERIC_READ | FILE_GENERIC_WRITE | WRITE_DAC
                                 : FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES |
                                       FILE_WRITE_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE;
  const DWORD acl_bytes = sizeof(ACL) + 2 * (sizeof(ACCESS_ALLOWED_ACE) + SECURITY_MAX_SID_SIZE);
  acl_storage->assign(acl_bytes, 0);
  auto* acl = reinterpret_cast<PACL>(acl_storage->data());
  return InitializeAcl(acl, acl_bytes, ACL_REVISION) && AddAccessAllowedAce(acl, ACL_REVISION, access, user_sid) &&
         (container_sid == nullptr || AddAccessAllowedAce(acl, ACL_REVISION, access, container_sid));
}

// Creates the pipe's instances with a fresh secret in its name. false with the
// Win32 error if it cannot.
DWORD CreateInstances() {
  std::vector<uint8_t> acl;
  if (!MakeDacl(true, &acl)) return GetLastError();
  SECURITY_DESCRIPTOR sd;
  if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) ||
      !SetSecurityDescriptorDacl(&sd, TRUE, reinterpret_cast<PACL>(acl.data()), FALSE)) {
    return GetLastError();
  }
  SECURITY_ATTRIBUTES sa{sizeof sa, &sd, FALSE};
  DWORD error = ERROR_SUCCESS;
  if (BCryptGenRandom(nullptr, g_broker.secret, sizeof g_broker.secret, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
    return ERROR_GEN_FAILURE;
  }
  for (int attempt = 0; attempt < 4; ++attempt) {
    // The name's own random value, independent of the secret (the name can be
    // listed). \\.\pipe\LOCAL\ is the pipe namespace an AppContainer may
    // create in; outside one it is an ordinary name.
    uint8_t nonce[16];
    if (BCryptGenRandom(nullptr, nonce, sizeof nonce, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
      return ERROR_GEN_FAILURE;
    }
    wchar_t hex[33];
    for (int i = 0; i < 16; ++i) std::swprintf(hex + 2 * i, 3, L"%02x", nonce[i]);
    std::swprintf(g_broker.pipe, sizeof g_broker.pipe / sizeof g_broker.pipe[0],
                  L"\\\\.\\pipe\\LOCAL\\gesher-spawn-%lu-%ls", static_cast<unsigned long>(g_broker.root_pid), hex);
    int made = 0;
    for (; made < kInstances; ++made) {
      const DWORD open = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | WRITE_DAC |
                         (made == 0 ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
      HANDLE pipe = CreateNamedPipeW(g_broker.pipe, open,
                                     PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
                                         PIPE_REJECT_REMOTE_CLIENTS,
                                     kInstances, 4096, 65536, 0, &sa);
      HANDLE event = pipe != INVALID_HANDLE_VALUE ? CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr;
      if (event == nullptr) {
        error = GetLastError();
        if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
        break;
      }
      g_broker.instance[made] = pipe;
      g_broker.event[made] = event;
    }
    if (made == kInstances) {
      // From now on no one, the root included, may add an instance: a process
      // of the same user that finds the name cannot serve it.
      std::vector<uint8_t> tight;
      if (!MakeDacl(false, &tight) ||
          SetSecurityInfo(g_broker.instance[0], SE_KERNEL_OBJECT,
                          DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr,
                          reinterpret_cast<PACL>(tight.data()), nullptr) != ERROR_SUCCESS) {
        error = GetLastError();
      } else {
        return ERROR_SUCCESS;
      }
    }
    for (int i = 0; i < made; ++i) {
      CloseHandle(g_broker.instance[i]);
      CloseHandle(g_broker.event[i]);
      g_broker.instance[i] = g_broker.event[i] = nullptr;
    }
    // ERROR_ACCESS_DENIED or ERROR_PIPE_BUSY: the name exists (another
    // process made it first); draw another name.
  }
  return error ? error : ERROR_ALREADY_EXISTS;
}

BOOL CALLBACK StartOnce(PINIT_ONCE, void*, void**) {
  g_broker.root_pid = GetCurrentProcessId();
  g_broker.root = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, g_broker.root_pid);
  g_broker.root_created = CreationTime(GetCurrentProcess());
  HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), nullptr);
  if (section != nullptr) {
    g_broker.shared = static_cast<Shared*>(MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
  }
  if (g_broker.root == nullptr || g_broker.shared == nullptr) {
    // No table: forked children's spawns fail (ENOTSUP), reported once.
    std::fprintf(stderr, "gesher: the spawn broker could not start (Windows error %lu)\n", GetLastError());
    std::fflush(stderr);
    g_broker.shared = nullptr;
    return TRUE;
  }
  DWORD error;
  {
    std::lock_guard<std::mutex> helper(ForkHelperLock());  // IsBrokerHandle's set
    error = CreateInstances();
  }
  for (int i = 0; error == ERROR_SUCCESS && i < kInstances; ++i) {
    if (!StartHelperThread(InstanceThread, reinterpret_cast<void*>(static_cast<intptr_t>(i)), 64 * 1024)) {
      error = GetLastError();
    }
  }
  if (error != ERROR_SUCCESS) Stopped(error);
  return TRUE;
}

// --- a forked child: asking the root ------------------------------------------------

// The root runs (its exit code is set when it starts ending, before its
// process object is signalled).
bool RootAlive() {
  DWORD code = 0;
  return g_broker.root != nullptr && WaitForSingleObject(g_broker.root, 0) == WAIT_TIMEOUT &&
         GetExitCodeProcess(g_broker.root, &code) && code == STILL_ACTIVE &&
         GetProcessId(g_broker.root) == g_broker.root_pid && CreationTime(g_broker.root) == g_broker.root_created;
}

// Waits for `ov` and for the root together; false (with the error) if the
// root ended or `ms` passed first.
bool Await(HANDLE pipe, OVERLAPPED* ov, BOOL started, DWORD ms, DWORD* moved, DWORD* error) {
  if (!started) {
    *error = GetLastError();
    if (*error != ERROR_IO_PENDING) return false;
    HANDLE waits[2] = {ov->hEvent, g_broker.root};
    const DWORD r = WaitForMultipleObjects(2, waits, FALSE, ms);
    if (r != WAIT_OBJECT_0) {
      CancelIoEx(pipe, ov);
      GetOverlappedResult(pipe, ov, moved, TRUE);
      *error = r == WAIT_OBJECT_0 + 1 ? ERROR_NOT_SUPPORTED : ERROR_TIMEOUT;
      return false;
    }
  }
  if (GetOverlappedResult(pipe, ov, moved, FALSE)) return true;
  *error = GetLastError();
  return false;
}

// Connects to an instance the root serves. INVALID_HANDLE_VALUE with the error.
HANDLE Connect(DWORD* error) {
  const ULONGLONG deadline = GetTickCount64() + kConnectMs;
  for (;;) {
    if (!RootAlive()) return *error = ERROR_NOT_SUPPORTED, INVALID_HANDLE_VALUE;
    if (g_broker.shared->broker_error != 0) {
      ReportStopped();
      return *error = ERROR_NOT_SUPPORTED, INVALID_HANDLE_VALUE;
    }
    HANDLE pipe = CreateFileW(g_broker.pipe,
                              FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES |
                                  SYNCHRONIZE,
                              0, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) {
      ULONG server = 0;
      // The server must be the root: while this process holds a handle to the
      // root (RootAlive), no other process has its pid.
      if (GetNamedPipeServerProcessId(pipe, &server) && server == g_broker.root_pid && RootAlive()) {
        DWORD mode = PIPE_READMODE_MESSAGE;
        if (SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) return pipe;
      }
      // Not the root's (it cannot be another's: no one else may add an
      // instance), or the root is ending: the next round says which.
      CloseHandle(pipe);
      if (GetTickCount64() >= deadline) return *error = ERROR_TIMEOUT, INVALID_HANDLE_VALUE;
      Sleep(10);
      continue;
    }
    const DWORD e = GetLastError();
    if (e != ERROR_PIPE_BUSY && e != ERROR_FILE_NOT_FOUND) return *error = e, INVALID_HANDLE_VALUE;
    if (GetTickCount64() >= deadline) return *error = ERROR_TIMEOUT, INVALID_HANDLE_VALUE;
    // Every instance is busy (or, with the root gone, none exists: RootAlive
    // above says so on the next round).
    if (e == ERROR_PIPE_BUSY) WaitNamedPipeW(g_broker.pipe, 100);
    else Sleep(10);
  }
}

// The parent records a forked child in the tree's table just after the clone,
// while the child already runs: a request it sent before that would be
// refused. So the child's first request waits (up to 5 s) for its entry; if
// the table had no room it never comes, and the spawn fails with EAGAIN.
DWORD g_registered_pid = 0;  // this process's pid once its entry was seen
bool AwaitRegistered() {
  const DWORD self = GetCurrentProcessId();
  if (g_registered_pid == self) return true;
  const LONG64 created = CreationTime(GetCurrentProcess());
  const ULONGLONG deadline = GetTickCount64() + 5000;
  for (;;) {
    if (InTree(self, created)) {
      g_registered_pid = self;
      return true;
    }
    if (!RootAlive() || GetTickCount64() >= deadline) return !RootAlive();  // root gone: Connect says so
    Sleep(1);
  }
}

}  // namespace

void StartSpawnBroker() {
  if (InForkedChild()) return;
  InitOnceExecuteOnce(&g_broker_once, StartOnce, nullptr, nullptr);
}

void RegisterForkedChild(DWORD pid, HANDLE process) {
  Shared* shared = g_broker.shared;
  if (shared == nullptr) return;
  const LONG64 created = CreationTime(process);
  for (int pass = 0; pass < 2; ++pass) {
    for (TreeEntry& e : shared->entries) {
      if (InterlockedCompareExchange(&e.pid, static_cast<LONG>(pid), 0) == 0) {
        InterlockedExchange64(&e.created, created);
        return;
      }
    }
    // Full: free the entries of processes that have ended (or whose pid
    // another process now has).
    for (TreeEntry& e : shared->entries) {
      const DWORD old = static_cast<DWORD>(e.pid);
      HANDLE h = old != 0 ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, old) : nullptr;
      const bool live = h != nullptr && WaitForSingleObject(h, 0) == WAIT_TIMEOUT && CreationTime(h) == e.created;
      if (h != nullptr) CloseHandle(h);
      if (old != 0 && !live) {
        InterlockedExchange64(&e.created, 0);
        InterlockedCompareExchange(&e.pid, 0, static_cast<LONG>(old));
      }
    }
  }
}

void ForgetForkedChild(DWORD pid) {
  Shared* shared = g_broker.shared;
  if (shared == nullptr) return;
  for (TreeEntry& e : shared->entries) {
    if (static_cast<DWORD>(e.pid) != pid) continue;
    InterlockedExchange64(&e.created, 0);
    InterlockedCompareExchange(&e.pid, 0, static_cast<LONG>(pid));
  }
}

bool IsBrokerHandle(HANDLE handle) {
  if (InForkedChild()) return false;  // a clone has none (the values are the root's)
  for (int i = 0; i < kInstances; ++i) {
    if (handle != nullptr && (g_broker.instance[i] == handle || g_broker.event[i] == handle)) return true;
  }
  return false;
}

DWORD BrokeredCreateProcess(const wchar_t* application, const wchar_t* command_line, const wchar_t* environment,
                            const wchar_t* cwd, DWORD flags, const STARTUPINFOW& startup,
                            const HANDLE* inherit, size_t inherit_count, PROCESS_INFORMATION* info) {
  if (g_broker.shared == nullptr || g_broker.pipe[0] == 0) return ERROR_NOT_SUPPORTED;
  auto chars = [](const wchar_t* s) { return s == nullptr ? size_t{0} : wcslen(s); };
  size_t environment_chars = 0;
  if (environment != nullptr) {
    const wchar_t* e = environment;
    while (*e != 0) e += wcslen(e) + 1;
    environment_chars = static_cast<size_t>(e - environment) + 1;
  }
  const size_t command_chars = chars(command_line) + 1;  // with its terminator: CreateProcess may write it
  const size_t total = sizeof(RequestHeader) +
                       (chars(application) + command_chars + chars(cwd) + environment_chars) * sizeof(wchar_t) +
                       (startup.lpReserved2 != nullptr ? startup.cbReserved2 : 0) + inherit_count * sizeof(uint64_t);
  if (total > kMaxRequest) return ERROR_BAD_LENGTH;  // E2BIG, as an oversized argument list
  RequestHeader h{};
  h.magic = kMagic;
  h.requester = GetCurrentProcessId();
  std::memcpy(h.secret, g_broker.secret, sizeof h.secret);
  h.flags = flags & ~EXTENDED_STARTUPINFO_PRESENT;
  h.application_chars = static_cast<uint32_t>(chars(application));
  h.command_chars = static_cast<uint32_t>(command_chars);
  h.cwd_chars = static_cast<uint32_t>(chars(cwd));
  h.environment_chars = static_cast<uint32_t>(environment_chars);
  h.reserved2_bytes = startup.lpReserved2 != nullptr ? startup.cbReserved2 : 0;
  h.inherit_count = static_cast<uint32_t>(inherit_count);
  h.startup_flags = startup.dwFlags;
  h.std_handles[0] = reinterpret_cast<uintptr_t>(startup.hStdInput);
  h.std_handles[1] = reinterpret_cast<uintptr_t>(startup.hStdOutput);
  h.std_handles[2] = reinterpret_cast<uintptr_t>(startup.hStdError);
  std::vector<uint8_t> request;
  request.reserve(total);
  Append(&request, &h, sizeof h);
  Append(&request, application, size_t{h.application_chars} * sizeof(wchar_t));
  Append(&request, command_line, (command_chars - 1) * sizeof(wchar_t));
  const wchar_t zero = 0;
  Append(&request, &zero, sizeof zero);
  Append(&request, cwd, size_t{h.cwd_chars} * sizeof(wchar_t));
  Append(&request, environment, environment_chars * sizeof(wchar_t));
  Append(&request, startup.lpReserved2, h.reserved2_bytes);
  for (size_t i = 0; i < inherit_count; ++i) {
    const uint64_t v = reinterpret_cast<uintptr_t>(inherit[i]);
    Append(&request, &v, sizeof v);
  }
  DWORD error = ERROR_SUCCESS;
  if (!AwaitRegistered()) return ERROR_TIMEOUT;  // EAGAIN
  HANDLE pipe = Connect(&error);
  if (pipe == INVALID_HANDLE_VALUE) return error;
  HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (event == nullptr) {
    error = GetLastError();
    CloseHandle(pipe);
    return error;
  }
  Reply reply{};
  DWORD n = 0;
  OVERLAPPED write{};
  write.hEvent = event;
  bool ok = Await(pipe, &write, WriteFile(pipe, request.data(), static_cast<DWORD>(request.size()), &n, &write),
                  kRequestMs, &n, &error);
  if (ok) {
    OVERLAPPED read{};
    read.hEvent = event;
    ok = Await(pipe, &read, ReadFile(pipe, &reply, sizeof reply, &n, &read), kServeMs, &n, &error);
    if (ok && n != sizeof reply) ok = false, error = ERROR_INVALID_DATA;
    // The root dropped the connection without a reply: it ended meanwhile, or
    // refused the request outright.
    if (!ok && (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED)) {
      error = RootAlive() ? ERROR_ACCESS_DENIED : ERROR_NOT_SUPPORTED;
    }
  }
  CloseHandle(event);
  CloseHandle(pipe);
  if (!ok) return error;
  if (reply.error != 0) return reply.error;
  info->hProcess = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(reply.process));
  info->hThread = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(reply.thread));
  info->dwProcessId = reply.pid;
  info->dwThreadId = reply.tid;
  return ERROR_SUCCESS;
}

}  // namespace gesher
