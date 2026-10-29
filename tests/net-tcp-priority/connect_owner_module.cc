// Independent WinSDK/CRT DLL. No Gesher symbols, libraries or mutable hooks.
#include "connect_owner_module.h"
#include <mswsock.h>
#include <process.h>
#include <memory>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(bool value, const char* label) {
  if (!value) { fprintf(stderr, "FAIL OWNER_MODULE %s win32=%lu wsa=%d\n", label, GetLastError(), WSAGetLastError()); fflush(stderr); ExitProcess(71); }
}
struct Context {
  OwnerRecord* record;
  sockaddr_in address;
  OVERLAPPED overlap{};
  std::mutex mutex;
  explicit Context(OwnerRecord* r) : record(r), address(r->address) {
    overlap.hEvent = WSACreateEvent(); require(overlap.hEvent != WSA_INVALID_EVENT, "owned overlap event");
  }
  ~Context() { require(WSACloseEvent(overlap.hEvent), "retired overlap event"); record->context_retired = 1; }
};
struct TlsWitness {
  OwnerRecord* record = nullptr;
  ~TlsWitness() { if (record) InterlockedIncrement(&record->tls_retired); }
};
static thread_local TlsWitness tls;
static unsigned __stdcall owner(void* raw) {
  OwnerRecord* record = static_cast<OwnerRecord*>(raw);
  tls.record = record;
  {
    // Exercise normal C++ object and owning CRT thread cleanup. A reaper waits
    // for actual thread exit, rather than a user event preceding these actions.
    auto context = std::make_shared<Context>(record);
    std::unique_lock<std::mutex> lock(context->mutex);
    record->owner_id = GetCurrentThreadId();
    WSAPROTOCOL_INFOW info{}; int length = sizeof(info);
    require(getsockopt(record->socket, SOL_SOCKET, SO_PROTOCOL_INFOW, reinterpret_cast<char*>(&info), &length) == 0, "actual provider identity");
    record->provider_family = static_cast<DWORD>(info.iAddressFamily);
    record->provider_type = static_cast<DWORD>(info.iSocketType);
    record->provider_protocol = static_cast<DWORD>(info.iProtocol);
    GUID id = WSAID_CONNECTEX; LPFN_CONNECTEX connect_ex = nullptr; DWORD bytes = 0;
    require(WSAIoctl(record->socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &id, sizeof(id), &connect_ex, sizeof(connect_ex), &bytes, nullptr, nullptr) == 0 && bytes == sizeof(connect_ex), "provider extension");
    const BOOL started = connect_ex(record->socket, reinterpret_cast<sockaddr*>(&context->address), sizeof(context->address), nullptr, 0, &bytes, &context->overlap);
    record->submit_error = started ? 0 : WSAGetLastError();
    require(!started && record->submit_error == ERROR_IO_PENDING, "strict actual pending submission");
    require(DuplicateHandle(GetCurrentProcess(), context->overlap.hEvent, GetCurrentProcess(), &record->native_event, SYNCHRONIZE, FALSE, 0), "driver retained native completion proof");
    require(SetEvent(record->submitted), "publish actual submission");
    lock.unlock();
    require(WaitForSingleObject(context->overlap.hEvent, 5000) == WAIT_OBJECT_0, "actual kernel completion retained");
    require(WaitForSingleObject(record->allow_result, 5000) == WAIT_OBJECT_0, "bounded result gate");
    lock.lock();
    record->internal = static_cast<unsigned long long>(context->overlap.Internal);
    if (InterlockedCompareExchange(&record->native_closed, 0, 0)) {
      using MapStatus = ULONG(WINAPI*)(LONG);
      FARPROC symbol = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError");
      MapStatus map = nullptr; static_assert(sizeof(map) == sizeof(symbol)); memcpy(&map, &symbol, sizeof(map));
      require(map != nullptr, "completion status mapper");
      record->completion_error = static_cast<int>(map(static_cast<LONG>(context->overlap.Internal)));
    } else {
      DWORD flags = 0;
      record->completion_error = WSAGetOverlappedResult(record->socket, &context->overlap, &bytes, FALSE, &flags) ? 0 : WSAGetLastError();
    }
    require(record->completion_error != WSA_IO_INCOMPLETE, "never retire uncompleted context");
    lock.unlock();
  }
  require(SetEvent(record->completed), "context destruction before completion receipt");
  return 0;  // _beginthreadex wrapper performs CRT/TLS exit before signaled HANDLE
}
struct ReapTicket { HANDLE owner; HMODULE module; };
static DWORD WINAPI reaper(void* raw) {
  // WinAPI only: no C++ owners, CRT or pthread attachment in the releasing
  // thread. Do not execute module code after its final reference is released.
  auto* ticket = static_cast<ReapTicket*>(raw);
  HANDLE thread = ticket->owner; HMODULE module = ticket->module;
  if (WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0) ExitProcess(72);
  if (!CloseHandle(thread)) ExitProcess(73);
  if (!HeapFree(GetProcessHeap(), 0, ticket)) ExitProcess(74);
  FreeLibraryAndExitThread(module, 0);
}
extern "C" __declspec(dllexport) BOOL WINAPI StartNativeOwner(OwnerRecord* record, HANDLE* thread, HANDLE* release_thread) {
  require(record && record->abi == 1 && record->size == static_cast<DWORD>(sizeof(OwnerRecord)), "exact fixture ABI");
  HMODULE module = nullptr;
  require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&StartNativeOwner), &module), "worker module reference");
  auto* ticket = static_cast<ReapTicket*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ReapTicket)));
  require(ticket != nullptr, "owned reaper ticket"); ticket->module = module;
  uintptr_t started = _beginthreadex(nullptr, 0, owner, record, CREATE_SUSPENDED, nullptr);
  require(started != 0, "owning CRT thread creation"); ticket->owner = reinterpret_cast<HANDLE>(started);
  require(DuplicateHandle(GetCurrentProcess(), ticket->owner, GetCurrentProcess(), thread, SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0), "retained exact native thread handle");
  *release_thread = CreateThread(nullptr, 0, reaper, ticket, 0, nullptr);
  require(*release_thread != nullptr, "WinAPI-only module reaper");
  require(ResumeThread(ticket->owner) == 1, "resume exact suspended owner once");
  return TRUE;
}
