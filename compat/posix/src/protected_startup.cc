// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include <gesher/protected_startup.h>
#include <gesher/file_description.h>
#include <gesher/usb_block.h>

#include "../../usb_storage/protected_runtime_client.h"

#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <unistd.h>

#include <array>
#include <memory>

namespace gesher {
namespace {

struct CloseResource {
  void operator()(void* object) const {
    if (object && object != INVALID_HANDLE_VALUE) CloseHandle(object);
  }
};
using Handle = std::unique_ptr<void, CloseResource>;

}  // namespace

uint32_t InitializeProtectedProcessResources() try {
  usb_storage::ProtectedStartupResources resources;
  const uint32_t initialized = usb_storage::ProtectedInitializeResources(&resources);
  if (initialized != ERROR_SUCCESS) return initialized;
  const Handle block_pipe(reinterpret_cast<HANDLE>(resources.block_pipe));
  const Handle block_gate(reinterpret_cast<HANDLE>(resources.block_gate));
  const Handle block_revoked(reinterpret_cast<HANDLE>(resources.block_revoked));
  const Handle signal_section(reinterpret_cast<HANDLE>(resources.signal_section));
  const Handle signal_event(reinterpret_cast<HANDLE>(resources.signal_event));
  using SignalInstaller = DWORD (*)(HANDLE, HANDLE);
  auto install_signals = reinterpret_cast<SignalInstaller>(GetProcAddress(GetModuleHandleW(nullptr),
      "gesher_install_protected_signal_resources"));
  if (!signal_section || !signal_event || !install_signals) return ERROR_INVALID_DATA;
  const DWORD signals = install_signals(signal_section.get(), signal_event.get());
  if (signals != ERROR_SUCCESS) return signals;
  std::array<Handle, 3> handles;
  std::array<DescriptorSnapshotPtr, 3> snapshots;
  for (size_t i = 0; i < handles.size(); ++i) {
    handles[i].reset(reinterpret_cast<HANDLE>(resources.standard_handles[i]));
  }
  // Capture every endpoint before changing the CRT slots. No numeric fd/handle
  // alias is trusted after another endpoint has been adopted or closed.
  for (size_t i = 0; i < handles.size(); ++i) {
    if (!handles[i]) continue;
    if (!SetHandleInformation(handles[i].get(), HANDLE_FLAG_INHERIT, 0)) return GetLastError();
    snapshots[i] = CaptureFileHandle(handles[i].get());
    if (!snapshots[i] || snapshots[i]->kind != DescriptorKind::kFile) return ERROR_INVALID_HANDLE;
  }
  constexpr DWORD standard[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
  for (int fd = 0; fd < 3; ++fd) {
    if (!snapshots[fd]) {
      close(fd);
      if (!SetStdHandle(standard[fd], nullptr)) return GetLastError();
      continue;
    }
    const int adopted = AttachFileDescription(snapshots[fd], true);
    if (adopted < 0) return ERROR_INVALID_HANDLE;
    if (adopted != fd) {
      const int result = dup2(adopted, fd);
      close(adopted);
      if (result < 0) return ERROR_INVALID_HANDLE;
    }
    if (fcntl(fd, F_SETFD, 0) < 0 || _setmode(fd, _O_BINARY) < 0) return ERROR_INVALID_HANDLE;
    if (!SetStdHandle(standard[fd], reinterpret_cast<HANDLE>(_get_osfhandle(fd)))) return GetLastError();
  }
  const bool any_block = block_pipe || block_gate || block_revoked;
  if (any_block && (!block_pipe || !block_gate || !block_revoked)) return ERROR_INVALID_DATA;
  if (any_block && usb_block::AttachBroker(block_pipe.get(), block_gate.get(), block_revoked.get()) != 0) {
    return ERROR_ACCESS_DENIED;
  }
  return ERROR_SUCCESS;
} catch (const std::bad_alloc&) {
  return ERROR_NOT_ENOUGH_MEMORY;
}

}  // namespace gesher
