// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "binder_transport.h"

#include "../../binder/protected_client_startup.h"
#include "../../binder/protected_data_pipe.h"

#include <algorithm>
#include <charconv>
#include <iterator>
#include <mutex>

namespace gesher {
namespace {

constexpr size_t kLocatorCapacity = std::size(binder::data_pipe::kLocatorPrefix) +
    binder::data_pipe::kLocatorHexDigits;
struct RoutingState {
  std::mutex mutex;
  bool sealed = false;
  bool protected_mode = false;
  wchar_t locator[kLocatorCapacity]{};
  binder::ProtectedDataEndpoint endpoint{};
};

// gesher_posix is linked into the executable and into Android DLLs. Native
// startup and the framework's Binder calls must share one sealed route.
RoutingState* Owner() {
  using Factory = RoutingState* (*)();
  static const auto factory = reinterpret_cast<Factory>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(nullptr), "gesher_binder_transport_owner_v1")));
  return factory ? factory() : nullptr;
}

bool ParseId(std::string_view text, DWORD* value) {
  if (text.empty()) return false;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), *value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size() && *value != 0;
}

}  // namespace

extern "C" void gesher_binder_transport_anchor_v1() {}
extern "C" __declspec(dllexport) RoutingState* gesher_binder_transport_owner_v1() {
  static RoutingState state;
  return &state;
}

bool ConfigureProtectedBinderStartup(std::string_view name, std::string_view authority_pid,
                                     std::string_view desktop_session) {
  constexpr size_t name_length = kLocatorCapacity - 1;
  DWORD authority = 0, desktop = 0;
  if (name.size() != name_length || !ParseId(authority_pid, &authority) ||
      !ParseId(desktop_session, &desktop)) return false;
  wchar_t wide[name_length];
  std::copy(name.begin(), name.end(), wide);
  return ConfigureProtectedBinder({std::wstring_view(wide, name_length), authority, 0, desktop}) ==
      ERROR_SUCCESS;
}

DWORD ConfigureProtectedBinder(const binder::ProtectedDataEndpoint& routing) {
  auto* state = Owner();
  if (!state) return ERROR_PROC_NOT_FOUND;
  std::lock_guard lock(state->mutex);
  if (state->sealed) return ERROR_ALREADY_INITIALIZED;
  state->sealed = state->protected_mode = true;
  if (!binder::data_pipe::ValidLocator(routing.locator) ||
      routing.authority_pid == 0 || routing.authority_session != 0) return ERROR_INVALID_PARAMETER;
  std::copy(routing.locator.begin(), routing.locator.end(), state->locator);
  state->endpoint = routing;
  state->endpoint.locator = std::wstring_view(state->locator, routing.locator.size());
  return ERROR_SUCCESS;
}

const binder::ProtectedDataEndpoint* ProtectedBinderEndpoint() {
  auto* state = Owner();
  if (!state) {
    // A missing executable owner is a broken runtime, never permission to
    // fall back to the ordinary user-controlled discovery route.
    static const binder::ProtectedDataEndpoint unavailable{};
    return &unavailable;
  }
  std::lock_guard lock(state->mutex);
  state->sealed = true;
  return state->protected_mode ? &state->endpoint : nullptr;
}

HANDLE OpenProtectedBinder(const binder::ProtectedDataEndpoint& routing) {
  HANDLE pipe = nullptr;
  for (int attempt = 0; attempt < 50; ++attempt) {
    const DWORD status = binder::OpenProtectedDataPipe(routing, &pipe);
    if (status == ERROR_SUCCESS) return pipe;
    if (status != ERROR_PIPE_BUSY) break;
    // The sealed name is NUL terminated. Wait only for a busy instance of
    // this same endpoint, and authenticate the handle after each open.
    if (!WaitNamedPipeW(routing.locator.data(), 100) && GetLastError() != ERROR_SEM_TIMEOUT) {
      break;
    }
  }
  return INVALID_HANDLE_VALUE;
}

}  // namespace gesher
