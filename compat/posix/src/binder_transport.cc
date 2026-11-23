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

std::mutex mutex;
bool sealed = false;
bool protected_mode = false;
wchar_t locator[std::size(binder::data_pipe::kLocatorPrefix) +
                binder::data_pipe::kLocatorHexDigits]{};
binder::ProtectedDataEndpoint endpoint{};

bool ParseId(std::string_view text, DWORD* value) {
  if (text.empty()) return false;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), *value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size() && *value != 0;
}

}  // namespace

bool ConfigureProtectedBinderStartup(std::string_view name, std::string_view authority_pid,
                                     std::string_view desktop_session) {
  constexpr size_t name_length = std::size(locator) - 1;
  DWORD authority = 0, desktop = 0;
  if (name.size() != name_length || !ParseId(authority_pid, &authority) ||
      !ParseId(desktop_session, &desktop)) return false;
  wchar_t wide[name_length];
  std::copy(name.begin(), name.end(), wide);
  return ConfigureProtectedBinder({std::wstring_view(wide, name_length), authority, 0, desktop}) ==
      ERROR_SUCCESS;
}

DWORD ConfigureProtectedBinder(const binder::ProtectedDataEndpoint& routing) {
  std::lock_guard lock(mutex);
  if (sealed) return ERROR_ALREADY_INITIALIZED;
  sealed = protected_mode = true;
  if (!binder::data_pipe::ValidLocator(routing.locator) ||
      routing.authority_pid == 0 || routing.authority_session != 0) return ERROR_INVALID_PARAMETER;
  std::copy(routing.locator.begin(), routing.locator.end(), locator);
  endpoint = routing;
  endpoint.locator = std::wstring_view(locator, routing.locator.size());
  return ERROR_SUCCESS;
}

const binder::ProtectedDataEndpoint* ProtectedBinderEndpoint() {
  std::lock_guard lock(mutex);
  sealed = true;
  return protected_mode ? &endpoint : nullptr;
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
