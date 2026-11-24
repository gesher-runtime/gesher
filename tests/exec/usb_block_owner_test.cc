// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Isolated in-memory protocol fixture. No physical disk, Windows storage
// endpoint, hardware capability or production synthetic device is involved.
#include <gesher/usb_block.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>
#include <thread>

#include "../../compat/usb_storage/block_broker.h"
#include "../../compat/usb_storage/block_channel.h"

namespace {
using namespace gesher::usb_storage;

bool Transfer(const BlockChannel& channel, void* bytes, DWORD size, bool writing) {
  OVERLAPPED pending{};
  pending.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!pending.hEvent) return false;
  DWORD transferred = 0;
  BOOL ok = writing ? WriteFile(channel.server(), bytes, size, &transferred, &pending)
                    : ReadFile(channel.server(), bytes, size, &transferred, &pending);
  if (!ok && GetLastError() == ERROR_IO_PENDING) {
    HANDLE waits[] = {channel.revocation(), pending.hEvent};
    if (WaitForMultipleObjects(2, waits, FALSE, 5000) == WAIT_OBJECT_0 + 1) {
      ok = GetOverlappedResult(channel.server(), &pending, &transferred, FALSE);
    } else {
      CancelIoEx(channel.server(), &pending);
      GetOverlappedResult(channel.server(), &pending, &transferred, TRUE);
    }
  }
  CloseHandle(pending.hEvent);
  return ok && transferred == size;
}

bool Send(const BlockChannel& channel, void* bytes, DWORD size) {
  BlockReply reply{ERROR_SUCCESS, size};
  return Transfer(channel, &reply, sizeof(reply), true) &&
      (size == 0 || Transfer(channel, bytes, size, true));
}

bool Serve(const BlockChannel& channel) {
  std::array<char, 8192> media{};
  // A real on-media MBR in this isolated byte buffer: partition1 begins at
  // sector1 and spans8 sectors. The sys/dev aliases must discover this table.
  media[510] = 0x55;
  media[511] = static_cast<char>(0xaa);
  media[446 + 4] = 0x0c;
  media[446 + 8] = 1;
  media[446 + 12] = 8;
  std::memcpy(media.data() + 513, "shared", 6);
  unsigned described = 0, read = 0, written = 0;
  for (;;) {
    BlockRequest request{};
    if (!Transfer(channel, &request, sizeof(request), false)) {
      return WaitForSingleObject(channel.revocation(), 0) == WAIT_OBJECT_0 &&
          described == 1 && read >= 3 && written == 1;
    }
    if (request.version != kBlockProtocolVersion || request.reserved) return false;
    if (request.operation == BlockOperation::kDescribe) {
      if (request.length || request.offset || ++described != 1) return false;
      BlockDescription description{media.size(), 512, kBlockWritable | kBlockRemovable, 0, 0};
      if (!Send(channel, &description, sizeof(description))) return false;
      continue;
    }
    if (!request.length || request.offset > media.size() ||
        request.length > media.size() - request.offset) return false;
    char* at = media.data() + request.offset;
    if (request.operation == BlockOperation::kRead) {
      ++read;
      if (!Send(channel, at, request.length)) return false;
    } else if (request.operation == BlockOperation::kWrite) {
      ++written;
      if (!Transfer(channel, at, request.length, false) || !Send(channel, nullptr, 0)) return false;
    } else {
      return false;
    }
  }
}

struct Module {
  HMODULE image = nullptr;
  int (*attach)(HANDLE, HANDLE, HANDLE) = nullptr;
  int (*query)() = nullptr;
  int (*open)() = nullptr;
  int (*read)(int, char*, unsigned, int64_t) = nullptr;
  int (*write)(int, const char*, unsigned, int64_t) = nullptr;
  int (*close)(int) = nullptr;
  int (*revoked)(int) = nullptr;

  ~Module() { Unload(); }
  void Unload() {
    if (image) FreeLibrary(image);
    image = nullptr;
  }
  bool Load(const char* path) {
    image = LoadLibraryA(path);
    if (!image) return false;
    attach = reinterpret_cast<decltype(attach)>(reinterpret_cast<void*>(GetProcAddress(image, "UsbOwnerAttach")));
    query = reinterpret_cast<decltype(query)>(reinterpret_cast<void*>(GetProcAddress(image, "UsbOwnerQuery")));
    open = reinterpret_cast<decltype(open)>(reinterpret_cast<void*>(GetProcAddress(image, "UsbOwnerOpen")));
    read = reinterpret_cast<decltype(read)>(reinterpret_cast<void*>(GetProcAddress(image, "UsbOwnerRead")));
    write = reinterpret_cast<decltype(write)>(reinterpret_cast<void*>(GetProcAddress(image, "UsbOwnerWrite")));
    close = reinterpret_cast<decltype(close)>(reinterpret_cast<void*>(GetProcAddress(image, "UsbOwnerClose")));
    revoked = reinterpret_cast<decltype(revoked)>(reinterpret_cast<void*>(GetProcAddress(image, "UsbOwnerRevokedRead")));
    return attach && query && open && read && write && close && revoked;
  }
};

int Check(const char* dll, bool attach_from_dll, BlockChannel& channel, const BlockGuestHandles& handles) {
  Module module;
  if (!module.Load(dll) || module.query() != 0) return 10;
  const int attached = attach_from_dll ? module.attach(handles.pipe, handles.gate, handles.revoked) :
      gesher::usb_block::AttachBroker(handles.pipe, handles.gate, handles.revoked);
  if (attached != 0 || module.query() != 1) return 11;
  struct stat info{};
  if (stat("/dev/block/sdb", &info) != 0 || !S_ISBLK(info.st_mode)) return 12;
  // This must fail before issuing another describe request, whichever module
  // made the first attachment. The server allows exactly one publication.
  if (module.attach(handles.pipe, handles.gate, handles.revoked) != -1 || errno != EBUSY ||
      gesher::usb_block::AttachBroker(handles.pipe, handles.gate, handles.revoked) != -1 ||
      errno != EBUSY) return 13;

  const int from_exe = open("/dev/block/sdb", O_RDWR);
  const int from_dll = module.open();
  if (from_exe < 0 || from_dll < 0) return 14;
  char value[6]{};
  if (module.read(from_exe, value, sizeof(value), 513) != sizeof(value) ||
      std::memcmp(value, "shared", sizeof(value)) != 0) return 15;
  if (pread(from_dll, value, sizeof(value), 513) != sizeof(value) ||
      std::memcmp(value, "shared", sizeof(value)) != 0) return 16;
  if (module.write(from_dll, "owner!", 6, 513) != 6) return 17;
  // Long-lived session and open-description deleters belong to the executable,
  // including when the DLL called AttachBroker and Open before being unloaded.
  module.Unload();
  if (pread(from_dll, value, sizeof(value), 513) != sizeof(value) ||
      std::memcmp(value, "owner!", sizeof(value)) != 0) return 18;
  if (!module.Load(dll) || module.query() != 1 ||
      module.read(from_exe, value, sizeof(value), 513) != sizeof(value) ||
      std::memcmp(value, "owner!", sizeof(value)) != 0) return 19;
  if (channel.Revoke() != ERROR_SUCCESS || module.query() != 0 ||
      stat("/dev/block/sdb", &info) == 0 || module.revoked(from_exe) != 1 ||
      pread(from_dll, value, 1, 0) != -1 || errno != ENODEV) return 20;
  if (module.close(from_exe) != 0 || close(from_dll) != 0) return 21;
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 3 || (std::strcmp(argv[2], "exe") && std::strcmp(argv[2], "dll"))) return 2;
  std::unique_ptr<BlockChannel> channel;
  if (BlockChannel::Create(&channel) != ERROR_SUCCESS) return 3;
  const HANDLE self = OpenProcess(PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, GetCurrentProcessId());
  if (!self) return 4;
  BlockGuestHandles handles;
  const DWORD handed = channel->Handoff(self, &handles);
  CloseHandle(self);
  if (handed != ERROR_SUCCESS) return 5;
  bool served = false;
  std::thread server([&] {
    served = Serve(*channel);
    if (!served) channel->Revoke();
  });
  const int result = Check(argv[1], std::strcmp(argv[2], "dll") == 0, *channel, handles);
  channel->Revoke();
  server.join();
  CloseHandle(handles.pipe);
  CloseHandle(handles.gate);
  CloseHandle(handles.revoked);
  if (result || !served) {
    std::fprintf(stderr, "USB executable/DLL session fixture failed: step=%d served=%d\n", result, served);
    return result ? result : 6;
  }
  std::printf("USB executable/DLL session owner fixture passed (%s attachment)\n", argv[2]);
  return 0;
}
