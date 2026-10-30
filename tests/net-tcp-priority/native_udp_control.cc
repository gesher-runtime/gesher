// HOST fixture only: real native UDP error input to the production fd/poll path.
// The product's existing unconnected UDP reset-reporting policy is unchanged.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <mswsock.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <algorithm>
#include <pthread.h>
#include "../../compat/posix/src/socket_internal.h"
#undef pthread_mutex_lock  // log the canonical Winpthreads function, not errno wrapper

namespace {
void require(bool value, const char* label) {
  if (!value) { fprintf(stderr, "FAIL TCP native UDP control: %s wsa=%d\n", label, WSAGetLastError()); exit(1); }
}
template<class Fn> Fn native(const char* name) {
  FARPROC symbol = GetProcAddress(GetModuleHandleW(L"ws2_32.dll"), name);
  Fn function = nullptr; static_assert(sizeof(function) == sizeof(symbol)); memcpy(&function, &symbol, sizeof(function));
  require(function != nullptr, "canonical native entry"); return function;
}
struct EmptyPthread {
  HANDLE ready = nullptr;
  std::atomic<HANDLE> self{nullptr};
  std::atomic<DWORD> error{0};
};
void* empty_pthread(void* raw) {
  auto& ticket = *static_cast<EmptyPthread*>(raw);
  HANDLE self = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, SYNCHRONIZE, FALSE, 0)) ticket.error.store(GetLastError());
  ticket.self.store(self);
  SetEvent(ticket.ready);
  return nullptr;  // normal provider wrapper/TLS retirement; no TCP/C++ ownership
}
}
namespace gesher::test {
void DumpOwnHandleTypes(int mode, uintptr_t caller, uintptr_t observer) {
  // CI-only observation, not descriptor or object identity authority. Never
  // query another process, retain snapshot handles, or ask for file/pipe names.
  using Query = NTSTATUS (NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
  Query process_query = nullptr, object_query = nullptr;
  HMODULE module = GetModuleHandleW(L"ntdll.dll"); require(module != nullptr, "native query module already loaded");
  FARPROC process_symbol = GetProcAddress(module, "NtQueryInformationProcess"), object_symbol = GetProcAddress(module, "NtQueryObject");
  static_assert(sizeof(process_query) == sizeof(process_symbol));
  memcpy(&process_query, &process_symbol, sizeof(process_query)); memcpy(&object_query, &object_symbol, sizeof(object_query));
  require(process_query && object_query, "actual own-process handle query APIs");
  using Lookup = decltype(&::RtlLookupFunctionEntry);
  Lookup lookup = nullptr; FARPROC lookup_symbol = GetProcAddress(module, "RtlLookupFunctionEntry");
  static_assert(sizeof(lookup) == sizeof(lookup_symbol)); memcpy(&lookup, &lookup_symbol, sizeof(lookup));
  require(lookup != nullptr, "actual native unwind function lookup");
  auto code = [&](const char* label, uintptr_t pc) {
    DWORD64 base = 0; auto* function = lookup(static_cast<DWORD64>(pc), &base, nullptr);
    HMODULE image = nullptr;
    require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(pc), &image) != FALSE, "known function actual module");
    require(!function || base == reinterpret_cast<uintptr_t>(image), "known function unwind and module bases match");
    wchar_t path[1024]{}; const DWORD size = GetModuleFileNameW(image, path, 1024);
    require(size > 0 && size < 1024 && pc >= reinterpret_cast<uintptr_t>(image), "bounded known function module name");
    const wchar_t* name = wcsrchr(path, L'\\'); name = name ? name + 1 : path;
    printf("TCP_PRIORITY_KNOWN_CODE name=%s module=%ls range_valid=%d rva=%llu begin=%lu end=%lu\n", label, name, function ? 1 : 0,
        static_cast<unsigned long long>(pc - reinterpret_cast<uintptr_t>(image)), function ? function->BeginAddress : 0UL, function ? function->EndAddress : 0UL);
  };
  static std::vector<uintptr_t> previous_values;
  if (mode == -1 || mode == -2) {
    using Set = NTSTATUS (NTAPI*)(HANDLE, ULONG, PVOID, ULONG);
    FARPROC symbol = GetProcAddress(module, "NtSetInformationProcess"); Set set = nullptr;
    static_assert(sizeof(set) == sizeof(symbol)); memcpy(&set, &symbol, sizeof(set));
    require(set != nullptr, "own-process handle tracing API");
    DWORD before = 0, after = 0; ULONG flags = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE, "pre-tracing handle count");
    const NTSTATUS enabled = set(GetCurrentProcess(), 32, &flags, sizeof(flags));
    require(GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE, "post-tracing handle count");
    printf("TCP_PRIORITY_HANDLE_TRACE_ENABLE status=%lu before=%lu after=%lu process_only=1\n", static_cast<unsigned long>(enabled), before, after); fflush(stdout);
    require(enabled >= 0 && before == after, "actual handle tracing enabled without new owned handles");
    code(mode == -2 ? "empty_pthread" : "submission_connect", caller);
    code(mode == -2 ? "NativeSupplierRetirement" : "submitted_barrier", observer);
    code("pthread_self", reinterpret_cast<uintptr_t>(&pthread_self));
    code("pthread_create", reinterpret_cast<uintptr_t>(&pthread_create));
    code("pthread_exit", reinterpret_cast<uintptr_t>(&pthread_exit));
    code("pthread_cond_wait", reinterpret_cast<uintptr_t>(&pthread_cond_wait));
    code("pthread_mutex_lock", reinterpret_cast<uintptr_t>(&pthread_mutex_lock));
    previous_values.clear(); return;
  }
  // Native ProcessHandleInformation (51), as defined by phnt/ntpsapi.h.
  // https://github.com/winsiderss/phnt/blob/master/ntpsapi.h
  struct Header { uintptr_t count, reserved; };
  struct Entry { HANDLE handle; uintptr_t handles, pointers; ULONG access, type, attributes, reserved; };
  static_assert(sizeof(void*) == 8 && sizeof(Header) == 16 && sizeof(Entry) == 40);
  DWORD before = 0; require(GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE, "pre-query actual handle count");
  std::vector<unsigned char> snapshot(4096); ULONG returned = 0;
  NTSTATUS status = process_query(GetCurrentProcess(), 51, snapshot.data(), static_cast<ULONG>(snapshot.size()), &returned);
  if (status == static_cast<NTSTATUS>(0xc0000004L) && returned > snapshot.size() && returned <= 1024 * 1024) {
    snapshot.resize(returned);
    status = process_query(GetCurrentProcess(), 51, snapshot.data(), static_cast<ULONG>(snapshot.size()), &returned);
  }
  printf("TCP_PRIORITY_HANDLE_SNAPSHOT mode=%d status=%lu returned=%lu before=%lu retained_aliases=0 object_names_queried=0\n", mode, static_cast<unsigned long>(status), returned, before); fflush(stdout);
  require(status >= 0 && returned >= sizeof(Header) && returned <= snapshot.size(), "bounded actual process handle snapshot");
  const auto* header = reinterpret_cast<const Header*>(snapshot.data());
  require(header->count <= (returned - sizeof(Header)) / sizeof(Entry), "actual handle entries bounded by returned bytes");
  const auto* entries = reinterpret_cast<const Entry*>(snapshot.data() + sizeof(Header));
  std::vector<uintptr_t> current_values;
  for (uintptr_t i = 0; i < header->count; ++i) {
    const uintptr_t value = reinterpret_cast<uintptr_t>(entries[i].handle); current_values.push_back(value);
    alignas(UNICODE_STRING) unsigned char type_buffer[4096]{}; ULONG type_returned = 0;
    const NTSTATUS queried = object_query(entries[i].handle, 2, type_buffer, sizeof(type_buffer), &type_returned);
    const auto* type = reinterpret_cast<const UNICODE_STRING*>(type_buffer);
    const uintptr_t begin = reinterpret_cast<uintptr_t>(type_buffer), text = reinterpret_cast<uintptr_t>(type->Buffer);
    const bool valid = queried >= 0 && type_returned >= sizeof(UNICODE_STRING) && type_returned <= sizeof(type_buffer) &&
        type->Length % sizeof(wchar_t) == 0 && type->Length <= type->MaximumLength && text >= begin &&
        text % alignof(wchar_t) == 0 && text - begin <= type_returned && type->MaximumLength <= type_returned - (text - begin);
    if (mode < 100) {
      printf("TCP_PRIORITY_HANDLE_TYPE mode=%d observed_value=%llu index=%lu access=%lu status=%lu type=%.*ls unretained=1\n", mode,
          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(entries[i].handle)), entries[i].type, entries[i].access,
          static_cast<unsigned long>(queried), valid ? static_cast<int>(type->Length / sizeof(wchar_t)) : 7, valid ? type->Buffer : L"unknown");
    }
    if (mode > 0 && valid && type->Length == 5 * sizeof(wchar_t) && memcmp(type->Buffer, L"Event", type->Length) == 0 &&
        std::find(previous_values.begin(), previous_values.end(), value) == previous_values.end()) {
      // A numeric difference selects a diagnostic query, never an ownership
      // claim. The kernel's OPEN/CLOSE history supplies the actual creator stack.
      struct TraceHeader { HANDLE filter; ULONG count; };
      struct TraceEntry { HANDLE handle, process, thread; ULONG kind; void* stack[16]; };
      static_assert(sizeof(TraceHeader) == 16 && sizeof(TraceEntry) == 160 && offsetof(TraceEntry, stack) == 32);
      std::vector<unsigned char> history(65536); auto* trace = reinterpret_cast<TraceHeader*>(history.data()); trace->filter = entries[i].handle;
      ULONG traced_bytes = 0; const NTSTATUS traced = process_query(GetCurrentProcess(), 32, history.data(), static_cast<ULONG>(history.size()), &traced_bytes);
      printf("TCP_PRIORITY_HANDLE_TRACE mode=%d observed_value=%llu status=%lu returned=%lu\n", mode, static_cast<unsigned long long>(value), static_cast<unsigned long>(traced), traced_bytes); fflush(stdout);
      require(traced >= 0 && traced_bytes >= sizeof(TraceHeader) && traced_bytes <= history.size(), "bounded actual handle trace result");
      require(trace->count > 0 && trace->count <= (traced_bytes - sizeof(TraceHeader)) / sizeof(TraceEntry), "actual trace entries bounded by native return");
      const auto* records = reinterpret_cast<const TraceEntry*>(history.data() + sizeof(TraceHeader));
      for (ULONG j = 0; j < trace->count; ++j) {
        require(records[j].handle == entries[i].handle, "actual trace respects selected local handle filter");
        printf("TCP_PRIORITY_HANDLE_TRACE_RECORD mode=%d observed_value=%llu record=%lu kind=%lu process=%llu thread=%llu\n", mode, static_cast<unsigned long long>(value), j, records[j].kind,
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(records[j].process)), static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(records[j].thread)));
        for (int frame = 0; frame != 16; ++frame) {
          HMODULE image = nullptr; const uintptr_t pc = reinterpret_cast<uintptr_t>(records[j].stack[frame]);
          if (!pc || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        reinterpret_cast<LPCWSTR>(pc), &image)) continue;  // no raw kernel addresses
          wchar_t path[1024]{}; const DWORD size = GetModuleFileNameW(image, path, 1024);
          if (!size || size >= 1024 || pc < reinterpret_cast<uintptr_t>(image)) continue;
          const wchar_t* base = wcsrchr(path, L'\\'); base = base ? base + 1 : path;
          DWORD64 function_image = 0; auto* function = lookup(static_cast<DWORD64>(pc), &function_image, nullptr);
          require(!function || function_image == reinterpret_cast<uintptr_t>(image), "trace unwind and module bases match");
          printf("TCP_PRIORITY_HANDLE_TRACE_FRAME mode=%d record=%lu frame=%d module=%ls range_valid=%d rva=%llu begin=%lu end=%lu\n", mode, j, frame, base, function ? 1 : 0,
              static_cast<unsigned long long>(pc - reinterpret_cast<uintptr_t>(image)), function ? function->BeginAddress : 0UL, function ? function->EndAddress : 0UL);
        }
      }
    }
  }
  previous_values = std::move(current_values);
  DWORD after = 0; require(GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE, "post-query actual handle count");
  printf("TCP_PRIORITY_HANDLE_SNAPSHOT_END mode=%d entries=%llu after=%lu unchanged=%d\n", mode, static_cast<unsigned long long>(header->count), after, after == before ? 1 : 0); fflush(stdout);
  require(after == before, "diagnostic retains no additional handles");
}
void NativeSupplierRetirement() {
  auto one = [](int cycle, bool detached) {
    DWORD before = 0, after = 0; require(GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE, "supplier control initial handle count");
    EmptyPthread ticket; ticket.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr); require(ticket.ready != nullptr, "supplier control owned publication event");
    pthread_attr_t attributes;
    require(pthread_attr_init(&attributes) == 0, "supplier control attributes");
    require(pthread_attr_setdetachstate(&attributes, detached ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE) == 0, "supplier control selected retirement mode");
    pthread_t thread;
    require(pthread_create(&thread, &attributes, empty_pthread, &ticket) == 0, "actual empty supplier thread");
    require(pthread_attr_destroy(&attributes) == 0, "supplier control attributes retired");
    require(WaitForSingleObject(ticket.ready, 2000) == WAIT_OBJECT_0, "supplier actual self-object publication");
    HANDLE self = ticket.self.load(); require(self != nullptr && ticket.error.load() == 0, "supplier retains actual native self object, never a TID lookup");
    require(WaitForSingleObject(self, 2000) == WAIT_OBJECT_0, "supplier whole native thread really exited");
    if (!detached) require(pthread_join(thread, nullptr) == 0, "supplier real joinable record retired");
    const bool self_closed = CloseHandle(self) != FALSE, ready_closed = CloseHandle(ticket.ready) != FALSE;
    require(self_closed && ready_closed, "supplier both fixture-owned handles closed");
    require(GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE, "supplier control final handle count");
    printf("TCP_PRIORITY_SUPPLIER_RETIRE detached=%d cycle=%d before=%lu after=%lu delta=%lld native_exit=1 fixture_closed=2 no_tcp=1\n", detached ? 1 : 0, cycle,
        before, after, static_cast<long long>(after) - before); fflush(stdout);
    if (cycle >= 0) require(after == before, "private supplier joined and detached retirement returns to exact warm count");
  };
  // A fresh process isolates all provider pool/cache state from the ORIGINAL
  // strict product census. Bootstrap uses a real joined empty thread and logs
  // its own delta; subsequent cycles require exact counts, never a +1 allowance.
  one(-1, false);
  DumpOwnHandleTypes(-2, reinterpret_cast<uintptr_t>(&empty_pthread), reinterpret_cast<uintptr_t>(&NativeSupplierRetirement));
  for (int cycle = 0; cycle != 4; ++cycle) one(cycle, false);
  DumpOwnHandleTypes(0, 0, 0);
  for (int cycle = 0; cycle != 4; ++cycle) { one(cycle, true); if (cycle == 0 || cycle == 3) DumpOwnHandleTypes(100 + cycle, 0, 0); }
  puts("PASS TCP supplier control: exact joined and detached retirement; product census unchanged");
}
int NativeUdpErrorControl(bool blocking) {
  require(ws::Startup(), "Winsock startup");
  auto bind_native = native<decltype(&::bind)>("bind");
  auto name_native = native<decltype(&::getsockname)>("getsockname");
  auto connect_native = native<decltype(&::connect)>("connect");
  auto send_native = native<decltype(&::send)>("send");
  SOCKET target = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  SOCKET client = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  require(target != INVALID_SOCKET && client != INVALID_SOCKET, "owned actual native endpoints");
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind_native(target, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "target bind");
  int length = sizeof(address); require(name_native(target, reinterpret_cast<sockaddr*>(&address), &length) == 0, "target address");
  sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind_native(client, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0, "client bind");
  BOOL report = TRUE; DWORD bytes = 0;
  require(WSAIoctl(client, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &bytes, nullptr, nullptr) == 0, "positive native error reporting input");
  u_long mode = blocking ? 0 : 1;
  require(ioctlsocket(client, FIONBIO, &mode) == 0, "exact actual native blocking-mode input");
  if (blocking) {
    auto option_native = native<decltype(&::setsockopt)>("setsockopt");
    const DWORD timeout = 1000;
    require(option_native(client, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0, "bounded imported blocking native input");
  }
  require(connect_native(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && closesocket(target) == 0, "real closed peer port");
  require(send_native(client, "u", 1, 0) == 1, "real loopback ICMP-producing datagram");
  WSAPOLLFD p{}; p.fd = client; p.events = POLLRDNORM | POLLRDBAND;
  const int result = WSAPoll(&p, 1, 2000);
  const int native_error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
  printf("TCP_PRIORITY_NATIVE_UDP_INPUT poll=%d revents=%d native_error=%d reset_reporting=1 payload_or_SO_ERROR_consumed=0\n", result, p.revents, native_error); fflush(stdout);
  // Independent WinSDK410543 observed this exact ICMP input as native normal
  // read readiness, SO_ERROR0, and a persistent WSAECONNRESET on MSG_PEEK.
  // This raw prerequisite is distinct from the unchanged public POLLERR gate.
  require(result == 1 && p.revents == POLLRDNORM, "exact independently observed native UDP reset readiness");
  auto recv_native = native<decltype(&::recv)>("recv");
  for (int observation = 0; observation != 2; ++observation) {
    char byte{};
    int received = recv_native(client, &byte, 1, MSG_PEEK);
    int error = received == SOCKET_ERROR ? WSAGetLastError() : 0;
    require(received == SOCKET_ERROR && error == WSAECONNRESET, "actual native UDP error persists through bounded peek");
    WSAPOLLFD repeat{}; repeat.fd = client; repeat.events = p.events;
    int repeated = WSAPoll(&repeat, 1, 0);
    require(repeated == 1 && repeat.revents == POLLRDNORM, "native reset readiness remains after observation");
    printf("TCP_PRIORITY_NATIVE_UDP_RESET observation=%d peek=%d error=%d repoll=%d revents=%d SO_ERROR=not-read\n",
        observation, received, error, repeated, repeat.revents); fflush(stdout);
  }
  SocketFd state; state.socket = static_cast<ws::Socket>(client); state.initial_nonblocking = !blocking;
  const int fd = RegisterSocket(state); require(fd >= 0, "production fd owns native error endpoint");
  return fd;
}
}
