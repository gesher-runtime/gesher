/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <cstdint>
using DWORD = uint32_t;
using ULONG = uint32_t;
using LONG = int32_t;
using BOOL = int;
using HWND = void*;
using HANDLE = void*;
using LPWSTR = wchar_t*;
struct FILETIME { DWORD dwLowDateTime, dwHighDateTime; };
constexpr BOOL FALSE = 0;
constexpr DWORD PROCESS_QUERY_LIMITED_INFORMATION = 0x1000, SYNCHRONIZE = 0x100000;
constexpr DWORD WAIT_TIMEOUT = 258, WAIT_OBJECT_0 = 0, WAIT_FAILED = 0xffffffff;
HWND GetForegroundWindow();
DWORD GetWindowThreadProcessId(HWND, DWORD*);
HANDLE OpenProcess(DWORD, BOOL, DWORD);
BOOL GetProcessTimes(HANDLE, FILETIME*, FILETIME*, FILETIME*, FILETIME*);
DWORD WaitForSingleObject(HANDLE, DWORD);
BOOL CloseHandle(HANDLE);
DWORD GetCurrentProcessId();
BOOL ProcessIdToSessionId(DWORD, DWORD*);
