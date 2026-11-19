/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
// The complete production broker, including real Windows byte-mode pipes,
// connection handlers, transaction ownership and condition-variable waits.
#include <windows.h>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <new>
#include <type_traits>

namespace accounting {
struct Entry { void* pointer; size_t bytes; };
Entry entries[2048]{};
SRWLOCK lock = SRWLOCK_INIT;
std::atomic<bool> enabled{false};
size_t live = 0, bytes = 0;
}
void* operator new(size_t bytes) {
  void* p = std::malloc(bytes ? bytes : 1);
  if (!p) std::abort();
  if (accounting::enabled) {
    AcquireSRWLockExclusive(&accounting::lock);
    bool inserted = false;
    for (auto& entry : accounting::entries) if (!entry.pointer) {
      entry = {p, bytes}; ++accounting::live; accounting::bytes += bytes; inserted = true; break;
    }
    ReleaseSRWLockExclusive(&accounting::lock);
    if (!inserted) std::abort();
  }
  return p;
}
void operator delete(void* p) noexcept {
  if (!p) return;
  AcquireSRWLockExclusive(&accounting::lock);
  for (auto& entry : accounting::entries) if (entry.pointer == p) {
    --accounting::live; accounting::bytes -= entry.bytes; entry = {}; break;
  }
  ReleaseSRWLockExclusive(&accounting::lock);
  std::free(p);
}
void operator delete(void* p, size_t) noexcept { ::operator delete(p); }
void* operator new[](size_t bytes) { return ::operator new(bytes); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete[](void* p, size_t) noexcept { ::operator delete(p); }

#define main production_broker_main
#include "@BROKER_SOURCE@"
#undef main

namespace {
int checks = 0;
void Check(bool ok, const char* why) {
  ++checks;
  if (!ok) { std::fprintf(stderr, "FAIL broker lifetime: %s\n", why); std::exit(1); }
}
struct Connection {
  HANDLE client = INVALID_HANDLE_VALUE;
  std::thread server;
  Connection() {
    static std::atomic<unsigned> sequence{0};
    std::wstring name = L"\\\\.\\pipe\\gesher-life-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++sequence);
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 65536, 65536, 0, nullptr);
    Check(pipe != INVALID_HANDLE_VALUE, "create native pipe");
    client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(client != INVALID_HANDLE_VALUE, "connect native pipe");
    Check(ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED, "accept native pipe");
    server = std::thread([pipe] { Serve(pipe); });
  }
  void Close() {
    if (client != INVALID_HANDLE_VALUE) { CloseHandle(client); client = INVALID_HANDLE_VALUE; }
    if (server.joinable()) server.join();
  }
  ~Connection() { Close(); }
  std::vector<uint8_t> Read() {
    std::vector<uint8_t> reply;
    Check(ReadMessage(client, &reply), "read actual broker reply"); return reply;
  }
};
struct Process {
  Connection connection;
  uint32_t id;
  uint64_t key;
  Process() {
    WireReply hello;
    hello.Put<uint32_t>(wire::kHelloProc); hello.Put<uint32_t>(wire::kMagic);
    hello.Put<uint32_t>(wire::kVersion); hello.Put<uint32_t>(GetCurrentProcessId()); hello.Put<uint32_t>(10000);
    Check(WriteMessage(connection.client, hello), "send process hello");
    auto reply = connection.Read(); In in{reply.data(), reply.data() + reply.size()}; int32_t status;
    Check(in.Get(&status) && status == 0 && in.Get(&id) && in.Get(&key), "actual process registered");
  }
  void ThreadHello(Connection& thread) {
    WireReply hello;
    hello.Put<uint32_t>(wire::kHelloThread); hello.Put<uint32_t>(wire::kMagic);
    hello.Put<uint32_t>(wire::kVersion); hello.Put(id); hello.Put(key); hello.Put<uint32_t>(12345);
    Check(WriteMessage(thread.client, hello), "send thread hello");
  }
};
void Cycle(bool close_process_first, bool abandon_handshake) {
  Process proc;
  Connection thread;
  proc.ThreadHello(thread);
  if (!abandon_handshake) {
    auto reply = thread.Read(); int32_t status = -1;
    Check(reply.size() == sizeof(status), "thread hello size"); std::memcpy(&status, reply.data(), sizeof(status));
    Check(status == 0, "actual thread registered");
  }
  if (close_process_first) proc.connection.Close();
  thread.Close();
  proc.connection.Close();
  std::lock_guard<std::mutex> lock(g_lock);
  Check(g_procs.empty(), "process retired from registry");
}
void BlockedRead() {
  Process proc; Connection thread; proc.ThreadHello(thread); thread.Read();
  WireReply enter;
  enter.Put<uint32_t>(wire::kWriteRead); enter.Put<uint64_t>(0);
  enter.Put<uint32_t>(0); enter.Put<uint32_t>(0); enter.Put<uint32_t>(BC_ENTER_LOOPER);
  Check(WriteMessage(thread.client, enter), "enter actual looper"); thread.Read();
  WireReply wait;
  wait.Put<uint32_t>(wire::kWriteRead); wait.Put<uint64_t>(1024);
  wait.Put<uint32_t>(0); wait.Put<uint32_t>(0);
  Check(WriteMessage(thread.client, wait), "start actual blocking read");
  bool waiting = false;
  for (int attempt = 0; attempt < 1000 && !waiting; ++attempt) {
    {
      std::lock_guard<std::mutex> lock(g_lock);
      auto found = g_procs.find(proc.id);
      waiting = found != g_procs.end() && !found->second->waiting_threads.empty();
    }
    if (!waiting) Sleep(1);
  }
  Check(waiting, "production ThreadRead admitted its condition-variable wait");
  proc.connection.Close();
  auto reply = thread.Read(); int32_t status = 0;
  Check(reply.size() >= sizeof(status), "read retirement reply"); std::memcpy(&status, reply.data(), sizeof(status));
  Check(status == -EBADF, "process retirement wakes live thread with EBADF");
  thread.Close();
}
void FailedHello() {
  Process proc;
  Out hello;
  hello.Put<uint32_t>(wire::kVersion); hello.Put(proc.id); hello.Put(proc.key); hello.Put<uint32_t>(12345);
  // Deterministic OS write failure after production thread admission. The
  // remaining churn cases use real pipes, including clients closing early.
  ServeThread(INVALID_HANDLE_VALUE, In{hello.bytes.data(), hello.bytes.data() + hello.bytes.size()});
  std::lock_guard<std::mutex> lock(g_lock);
  auto found = g_procs.find(proc.id);
  Check(found != g_procs.end() && found->second->threads.empty(), "failed hello left a registered thread");
}
// A transaction can still name dead participants after their pipes disappear.
// Exercise its actual owning fields separately from native pipe churn.
template<class Owner, class ThreadType = Thread, class TransactionType = Transaction> void TransactionPins() {
  if constexpr (!std::is_pointer_v<Owner>) {
    auto proc = std::make_shared<Proc>();
    auto caller = std::make_shared<ThreadType>(), receiver = std::make_shared<ThreadType>();
    caller->proc = proc; receiver->proc = proc;
    std::weak_ptr<Proc> weak_proc = proc;
    std::weak_ptr<ThreadType> weak_caller = caller, weak_receiver = receiver;
    auto transaction = std::make_unique<TransactionType>();
    transaction->from = caller; transaction->to_thread = receiver;
    caller.reset(); receiver.reset(); proc.reset();
    Check(!weak_proc.expired() && !weak_caller.expired() && !weak_receiver.expired(), "in-flight transaction pins participants");
    transaction.reset();
    Check(weak_proc.expired() && weak_caller.expired() && weak_receiver.expired(), "transaction destruction releases participant graph");
  }
}
}
int main() {
  Cycle(false, false); // Initialize the CRT/thread machinery outside accounting.
  accounting::enabled = true;
  for (int n = 0; n < 100; ++n) Cycle(n % 2, n % 3 == 0);
  BlockedRead();
  TransactionPins<decltype(g_procs)::mapped_type>();
  std::printf("broker churn: retained allocations=%zu bytes=%zu\n", accounting::live, accounting::bytes);
  Check(accounting::live == 0, "retired broker process/thread allocations were never destroyed");
  FailedHello();
  accounting::enabled = false;
  std::printf("broker lifetime: %d checks; retained allocations=%zu bytes=%zu\n", checks, accounting::live, accounting::bytes);
  Check(accounting::live == 0, "retired broker process/thread allocations were never destroyed");
  return 0;
}
