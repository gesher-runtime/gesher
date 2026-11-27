// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Exercise the actual private framing implementation, not a second decoder.
#include "../../compat/binder/loopback_relay.cc"
#include <cstdlib>

namespace relay = gesher::loopback_relay;
namespace {
void Require(bool ok, const char* label) {
  if (!ok) {
    std::fprintf(stderr, "FAIL frame control: %s (Win32=%lu)\n", label, GetLastError());
    std::exit(1);
  }
}
struct Pipe {
  HANDLE server = INVALID_HANDLE_VALUE, client = INVALID_HANDLE_VALUE;
  Pipe() {
    static unsigned sequence = 0;
    std::wstring name = L"\\\\.\\pipe\\GesherRelayFrames-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++sequence);
    server = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 4096, 0, nullptr);
    Require(server != INVALID_HANDLE_VALUE, "owned pipe creation");
    client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, 0, nullptr);
    Require(client != INVALID_HANDLE_VALUE, "owned pipe connection");
    Require(ConnectNamedPipe(server, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED,
        "accepted exact pipe");
  }
  ~Pipe() {
    if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
    DisconnectNamedPipe(server);
    CloseHandle(server);
  }
  void Write(const void* bytes, DWORD size) {
    DWORD sent = 0;
    Require(WriteFile(client, bytes, size, &sent, nullptr) && sent == size,
        "exact test frame write");
  }
};
}  // namespace

int main() {
  unsigned checks = 0;
  for (const relay::Header header : {relay::Header{UINT32_MAX, 0, 0},
       {relay::kData, 0, 0}, {relay::kData, 2049, 0},
       {relay::kData, 1, WSAECONNRESET}, {relay::kWriteEnd, 1, 0},
       {relay::kWriteEnd, 0, 1}, {relay::kError, 0, 1}}) {
    Pipe pipe; relay::Channel channel(pipe.server);
    pipe.Write(&header, sizeof(header)); bool progress = false;
    Require(!channel.Step(&progress) && !channel.received,
        "malformed header rejected before payload publication");
    ++checks;
  }
  {
    Pipe pipe; relay::Channel channel(pipe.server);
    relay::Header header{relay::kData, 3, 0}; bool progress = false;
    pipe.Write(&header, 5);
    Require(channel.Step(&progress) && !channel.received && channel.read == 5,
        "partial header retained without data");
    pipe.Write(reinterpret_cast<const char*>(&header) + 5, sizeof(header) - 5);
    pipe.Write("abc", 3);
    Require(channel.Step(&progress) && channel.received && channel.read == sizeof(header) + 3 &&
        std::memcmp(channel.incoming.bytes.data(), "abc", 3) == 0,
        "partial frame reassembles exact bytes");
    channel.Pop(); relay::Header bad{relay::kData, 2049, 0};
    pipe.Write(&bad, sizeof(bad));
    Require(!channel.Step(&progress) && !channel.received,
        "later malformed frame cannot reuse prior payload");
    checks += 3;
  }
  {
    Pipe pipe; relay::Channel channel(pipe.server);
    relay::Header header{relay::kError, 0, WSAECONNRESET}; bool progress = false;
    pipe.Write(&header, sizeof(header));
    Require(channel.Step(&progress) && channel.received &&
        channel.incoming.header.error == WSAECONNRESET, "real Winsock error preserved");
    ++checks;
  }
  {
    Pipe pipe; relay::Channel channel(pipe.server);
    CloseHandle(pipe.client); pipe.client = INVALID_HANDLE_VALUE; bool progress = false;
    Require(!channel.Step(&progress) && !channel.received,
        "closed transport is failure rather than fabricated FIN");
    ++checks;
  }
  // Keep all twelve original transport controls above. New typed controls
  // must reject malformed envelopes before publishing any payload.
  for (const uint32_t kind : {relay::kOption, relay::kOptionReply, relay::kCredit}) {
    const uint32_t expected = kind == relay::kCredit ? sizeof(uint32_t) : sizeof(relay::OptionMessage);
    for (const relay::Header header : {relay::Header{kind, expected - 1, 0},
         {kind, expected + 1, 0}, {kind, expected, WSAEINVAL}}) {
      Pipe pipe; relay::Channel channel(pipe.server); bool progress = false;
      pipe.Write(&header, sizeof(header));
      Require(!channel.Step(&progress) && !channel.received,
          "typed control rejects wrong length or envelope error");
      ++checks;
    }
    Pipe pipe; relay::Channel channel(pipe.server); bool progress = false;
    relay::Header header{kind, expected, 0};
    const relay::OptionMessage option{1, 0, 1, 1, 0};
    const uint32_t credit = 2048;
    const void* payload = kind == relay::kCredit ? static_cast<const void*>(&credit) : &option;
    pipe.Write(&header, sizeof(header));
    pipe.Write(payload, expected - 1);
    Require(channel.Step(&progress) && !channel.received,
        "partial typed control is not published");
    pipe.Write(static_cast<const char*>(payload) + expected - 1, 1);
    Require(channel.Step(&progress) && channel.received &&
        std::memcmp(channel.incoming.bytes.data(), payload, expected) == 0,
        "typed control preserves exact complete payload");
    checks += 2;
  }
  std::printf("PASS actual Win32 production relay framing controls: %u checks\n", checks);
}
