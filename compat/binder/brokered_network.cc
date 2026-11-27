// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "brokered_network.h"

#include <vector>
#include "../posix/include/gesher/windows_process_identity.h"

namespace gesher::network_broker {
bool HasInternetCapabilities(HANDLE process) {
  std::vector<BYTE> bytes;
  if (!process_identity::ReadCapabilities(process, &bytes)) return false;
  const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(bytes.data());
  if (groups->GroupCount > 256 ||
      bytes.size() < offsetof(TOKEN_GROUPS, Groups) + groups->GroupCount * sizeof(SID_AND_ATTRIBUTES)) return false;
  BYTE internet[SECURITY_MAX_SID_SIZE], local[SECURITY_MAX_SID_SIZE];
  DWORD size = sizeof(internet);
  if (!CreateWellKnownSid(WinCapabilityInternetClientSid, nullptr, internet, &size)) return false;
  size = sizeof(local);
  if (!CreateWellKnownSid(WinCapabilityPrivateNetworkClientServerSid, nullptr, local, &size)) return false;
  bool a = false, b = false;
  for (DWORD i = 0; i < groups->GroupCount; ++i) {
    const auto& group = groups->Groups[i];
    if (!(group.Attributes & SE_GROUP_ENABLED) || (group.Attributes & SE_GROUP_USE_FOR_DENY_ONLY)) continue;
    a |= EqualSid(group.Sid, internet) != FALSE;
    b |= EqualSid(group.Sid, local) != FALSE;
  }
  return a && b;
}

}  // namespace gesher::network_broker
