// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <string_view>

namespace gesher::binder {

// Supplied by protected startup before running guest code. These values are
// routing and identity constraints. The kernel endpoint and ACL authenticate
// the connection. Never construct this from Android environment or argv.
struct ProtectedDataEndpoint {
  std::wstring_view locator;
  DWORD authority_pid;
  DWORD authority_session;
  DWORD desktop_session;
};

// Open exactly this local pipe, once, synchronously and without inheritance.
// On success the caller owns the returned handle. On failure *result stays
// null, including ERROR_PIPE_BUSY. Protected callers must never fall back to
// ordinary Binder discovery or send a hello before this function succeeds.
DWORD OpenProtectedDataPipe(const ProtectedDataEndpoint& endpoint, HANDLE* result);

#ifdef GESHER_BINDER_DATA_CLIENT_FIXTURE
// Separately compiled real-account fixture. Production fixes the owner to
// LocalSystem and the authority session to session zero.
DWORD OpenProtectedDataPipeForFixture(const ProtectedDataEndpoint& endpoint,
                                      PSID fixture_owner, HANDLE* result);
#endif

}  // namespace gesher::binder
