// Independent WinSDK DLL-lifetime driver. No product code is linked or run.
#include "connect_owner_module.h"
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(bool value, const char* label) {
  if (!value) { fprintf(stderr, "FAIL OWNER_DRIVER %s win32=%lu wsa=%d\n", label, GetLastError(), WSAGetLastError()); exit(1); }
}
static SOCKET fresh() {
  SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  require(s != INVALID_SOCKET, "fresh native socket");
  sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0, "fresh native bind"); return s;
}
static DWORD handles() { DWORD value = 0; require(GetProcessHandleCount(GetCurrentProcess(), &value), "actual process handle count"); return value; }
static void run(int index, bool close_pending) {
  OwnerRecord record{}; SOCKET reserved = fresh(); int length = sizeof(record.address);
  require(getsockname(reserved, reinterpret_cast<sockaddr*>(&record.address), &length) == 0, "reserved target address");
  record.socket = fresh();
  record.submitted = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  record.allow_result = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  record.completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  require(record.submitted && record.allow_result && record.completed, "driver owns receipt gates");
  HMODULE module = LoadLibraryW(L"connect-owner-module.dll"); require(module != nullptr, "load independent module");
  FARPROC symbol = GetProcAddress(module, "StartNativeOwner"); StartOwner start = nullptr;
  static_assert(sizeof(start) == sizeof(symbol)); memcpy(&start, &symbol, sizeof(start)); require(start != nullptr, "fixture start export");
  HANDLE owner = nullptr, reaper = nullptr;
  require(start(&record, &owner, &reaper) && owner && reaper, "actual worker and reaper handles");
  require(WaitForSingleObject(record.submitted, 5000) == WAIT_OBJECT_0 && WaitForSingleObject(owner, 0) == WAIT_TIMEOUT, "native operation submitted by a live owner");
  require(record.submit_error == ERROR_IO_PENDING && record.provider_family == AF_INET && record.provider_type == SOCK_STREAM && record.provider_protocol == IPPROTO_TCP, "actual pending TCP provider");
  require(record.native_event && WaitForSingleObject(record.native_event, 0) == WAIT_TIMEOUT, "actual IO pending before caller unload");
  require(FreeLibrary(module), "caller releases its module reference"); module = nullptr; start = nullptr;
  HMODULE observed = nullptr;
  require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, L"connect-owner-module.dll", &observed), "owner pin survives caller unload");
  SOCKET replacement = INVALID_SOCKET;
  const SOCKET original = record.socket;
  if (close_pending) {
    require(WaitForSingleObject(record.native_event, 0) == WAIT_TIMEOUT, "actual IO still pending before last close");
    // Publication precedes close; the gated worker must never query this
    // numeric handle after it has become a different socket.
    InterlockedExchange(&record.native_closed, 1);
    require(closesocket(original) == 0, "actual last close cancels outstanding operation");
    replacement = fresh();
    printf("OWNER_MODULE_REUSE index=%d old=%llu replacement=%llu equal=%d\n", index, static_cast<unsigned long long>(original), static_cast<unsigned long long>(replacement), original == replacement ? 1 : 0); fflush(stdout);
    int value = -1; length = sizeof(value);
    require(getsockopt(replacement, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&value), &length) == 0 && value == 0, "replacement has independent error state");
  }
  require(SetEvent(record.allow_result), "release actual completion observation");
  require(WaitForSingleObject(record.completed, 5000) == WAIT_OBJECT_0 && WaitForSingleObject(owner, 5000) == WAIT_OBJECT_0, "context destruction and actual owning-thread exit");
  require(WaitForSingleObject(reaper, 5000) == WAIT_OBJECT_0, "module releasing thread actually exited");
  DWORD owner_code = 99, reaper_code = 99;
  require(GetExitCodeThread(owner, &owner_code) && GetExitCodeThread(reaper, &reaper_code) && owner_code == 0 && reaper_code == 0, "normal thread retirement");
  require(record.context_retired == 1 && record.tls_retired == 1 && record.completion_error == (close_pending ? ERROR_OPERATION_ABORTED : WSAECONNREFUSED), "exact context TLS and native completion");
  observed = nullptr;
  require(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, L"connect-owner-module.dll", &observed), "module really unloaded after owner exit");
  if (close_pending) require(closesocket(replacement) == 0, "replacement close");
  else require(closesocket(original) == 0, "original close after completion");
  require(closesocket(reserved) == 0 && CloseHandle(owner) && CloseHandle(reaper) && CloseHandle(record.submitted) && CloseHandle(record.allow_result) && CloseHandle(record.completed) && CloseHandle(record.native_event), "all retained native handles closed");
  printf("OWNER_MODULE_RESULT index=%d close_pending=%d submit=%d completion=%d internal=%llu owner_id=%lu context_retired=%lu tls_retired=%ld owner_exit=%lu reaper_exit=%lu unloaded=1 handles=%lu compiler=MSVC_CRT_not_product\n", index, close_pending ? 1 : 0, record.submit_error, record.completion_error, record.internal, record.owner_id, record.context_retired, record.tls_retired, owner_code, reaper_code, handles()); fflush(stdout);
}
int main() {
  WSADATA data{}; require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "startup");
  run(-1, false); const DWORD baseline = handles();
  for (int i = 0; i != 8; ++i) {
    run(i, (i & 1) != 0);
    const ULONGLONG deadline = GetTickCount64() + 2000;
    while (handles() != baseline && GetTickCount64() < deadline) Sleep(10);
    require(handles() == baseline, "repeat module ownership returns to original handle baseline");
  }
  require(WSACleanup() == 0, "cleanup");
  puts("PASS independent repeated module owner: real unload TLS handle baseline cancellation and native reuse observations; MinGW product remains unqualified");
}
