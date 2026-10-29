// CI-only independent native ownership ABI. Never included by the product.
#pragma once
#include <winsock2.h>
struct OwnerRecord {
  DWORD abi = 1, size = sizeof(OwnerRecord);
  SOCKET socket = INVALID_SOCKET;
  sockaddr_in address{};
  HANDLE submitted = nullptr, allow_result = nullptr, completed = nullptr, native_event = nullptr;
  volatile LONG native_closed = 0, tls_retired = 0;
  int submit_error = -1, completion_error = -1;
  DWORD owner_id = 0, provider_family = 0, provider_type = 0, provider_protocol = 0;
  DWORD context_retired = 0;
  unsigned long long internal = 0;
};
using StartOwner = BOOL(WINAPI*)(OwnerRecord*, HANDLE*, HANDLE*);
