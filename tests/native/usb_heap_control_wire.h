// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Disposable native diagnostic only; never used by applications or authority IPC.
#pragma once
#include <windows.h>
#define GESHER_HEAP_CONTROL_MAGIC 0x47484331u
typedef struct GesherHeapControlReport {
  DWORD magic, version, create_success, create_error;
  DWORD destroy_called, destroy_success, destroy_error, reserved;
} GesherHeapControlReport;
