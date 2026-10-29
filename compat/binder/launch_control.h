// Private inherited launcher/broker channel. Never reachable through Binder hello.
#pragma once
#include <windows.h>
#include <sddl.h>
#include <stdint.h>
#include <cstring>
#include <string>

namespace gesher { namespace launch_control {
enum : uint32_t { kPublish = 1, kRevoke = 2, kHandoff = 3, kCancel = 4 };
struct Request {
  uint32_t magic = 0x474c4350, version = 1, op = 0, uid = 0;
  uint64_t process = 0, birth = 0;
  uint8_t generation[16]{};
  char package[256]{};
  uint32_t restricted = 0;
};
inline std::wstring Profile(const uint8_t (&generation)[16]) {
  std::wstring name = L"Gesher.install.";
  for (auto byte : generation) { name += L"0123456789abcdef"[byte >> 4]; name += L"0123456789abcdef"[byte & 15]; }
  return name;
}
inline std::wstring GuestDevice(const std::wstring& device, const uint8_t (&generation)[16]) {
  return device + L"-guest-" + Profile(generation).substr(15);
}
inline HANDLE Handle(uint64_t value) { return reinterpret_cast<HANDLE>(uintptr_t(value)); }
inline uint64_t Value(HANDLE value) { return uint64_t(reinterpret_cast<uintptr_t>(value)); }
inline uint64_t Birth(HANDLE process) {
  FILETIME a{}, b{}, c{}, d{};
  return GetProcessTimes(process, &a, &b, &c, &d)
      ? (uint64_t(a.dwHighDateTime) << 32) | a.dwLowDateTime : 0;
}
// Endpoints MUST be byte-mode PIPE_NOWAIT. No synchronous unbounded wait on a
// hostile peer. Partial reads/writes retain their cursor and one shared deadline.
inline bool Transfer(HANDLE pipe, void* data, DWORD size, bool write, uint64_t deadline) {
  auto* cursor = static_cast<unsigned char*>(data);
  while (size && GetTickCount64() < deadline) {
    DWORD count = 0;
    BOOL ok = write ? WriteFile(pipe, cursor, size, &count, nullptr)
                    : ReadFile(pipe, cursor, size, &count, nullptr);
    if (count) { cursor += count; size -= count; continue; }
    if (!ok && GetLastError() != ERROR_NO_DATA && GetLastError() != ERROR_PIPE_LISTENING)
      return false;
    Sleep(1);
  }
  return size == 0;
}
inline bool Mode(HANDLE pipe, bool nonblock) {
  DWORD mode = PIPE_READMODE_BYTE | (nonblock ? PIPE_NOWAIT : PIPE_WAIT);
  return SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr) != FALSE;
}
inline bool Random(void* bytes, ULONG size) {
  HMODULE module = LoadLibraryW(L"advapi32.dll");
  if (!module) return false;
  using Fn = BOOLEAN (WINAPI*)(PVOID, ULONG);
  auto fn = reinterpret_cast<Fn>(GetProcAddress(module, "SystemFunction036"));
  bool ok = fn && fn(bytes, size);
  FreeLibrary(module);
  return ok;
}
// Random setup name is never published. Only the actual inherited client
// endpoint is the capability; FIRST_PIPE_INSTANCE prevents endpoint squatting.
inline bool Channel(HANDLE* server, HANDLE* client) {
  uint8_t nonce[32];
  if (!Random(nonce, sizeof(nonce))) return false;
  std::wstring name = L"\\\\.\\pipe\\gesher-private-launch-";
  for (uint8_t byte : nonce) {
    name += L"0123456789abcdef"[byte >> 4]; name += L"0123456789abcdef"[byte & 15];
  }
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
      L"D:P(A;;GA;;;OW)S:(ML;;NW;;;ME)", SDDL_REVISION_1, &descriptor, nullptr)) return false;
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
  *server = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
      1, 4096, 4096, 0, &security);
  LocalFree(descriptor);
  if (*server == INVALID_HANDLE_VALUE) return false;
  *client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                        OPEN_EXISTING, 0, nullptr);
  bool connected = *client != INVALID_HANDLE_VALUE &&
      (ConnectNamedPipe(*server, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED);
  if (!connected || !Mode(*client, true)) {
    if (*client != INVALID_HANDLE_VALUE) CloseHandle(*client);
    CloseHandle(*server); return false;
  }
  return true;
}
inline bool Generation(const std::u16string& text, uint8_t (&out)[16]) {
  if (text.size() != 36) return false;
  unsigned n = 0;
  for (unsigned i = 0; i < text.size();) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (text[i++] != u'-') return false;
      continue;
    }
    auto digit = [](char16_t c) -> int {
      return c >= u'0' && c <= u'9' ? c-u'0' : c >= u'a' && c <= u'f' ? c-u'a'+10 : -1;
    };
    int a = digit(text[i++]), b = digit(text[i++]);
    if (a < 0 || b < 0 || n >= 16) return false;
    out[n++] = uint8_t((a << 4) | b);
  }
  return n == 16;
}
} }
