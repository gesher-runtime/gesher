/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "windows.h"
constexpr HANDLE WTS_CURRENT_SERVER_HANDLE = nullptr;
constexpr int WTSSessionInfoEx = 25;
constexpr LONG WTS_SESSIONSTATE_LOCK = 0, WTS_SESSIONSTATE_UNLOCK = 1;
// Only the fields read by production code are represented at this explicit API boundary.
// The separate Windows build uses Microsoft's actual structure layout and headers.
struct WTSINFOEX_LEVEL1_W { ULONG SessionId; int SessionState; LONG SessionFlags; };
struct WTSINFOEXW { DWORD Level; union { WTSINFOEX_LEVEL1_W WTSInfoExLevel1; } Data; };
BOOL WTSQuerySessionInformationW(HANDLE, DWORD, int, LPWSTR*, DWORD*);
void WTSFreeMemory(void*);
