// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <windows.h>

namespace gesher::usb_storage {
inline DWORD CreateProtectedOwnerJob(HANDLE* output) {
  if (!output || *output) return ERROR_INVALID_PARAMETER;
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (!job) return GetLastError();
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                               sizeof(limits)) ||
      !SetHandleInformation(job, HANDLE_FLAG_INHERIT, 0)) {
    const DWORD error = GetLastError();
    CloseHandle(job);
    return error;
  }
  *output = job;
  return ERROR_SUCCESS;
}

inline DWORD StopProtectedOwnerJob(HANDLE job, DWORD milliseconds = 5000) {
  if (!job) return ERROR_SUCCESS;
  if (!TerminateJobObject(job, ERROR_OPERATION_ABORTED)) return GetLastError();
  const ULONGLONG deadline = GetTickCount64() + milliseconds;
  do {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION usage{};
    if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
                                   &usage, sizeof(usage), nullptr))
      return GetLastError();
    if (!usage.ActiveProcesses) return ERROR_SUCCESS;
    Sleep(2);
  } while (GetTickCount64() < deadline);
  return ERROR_TIMEOUT;
}
}  // namespace gesher::usb_storage
