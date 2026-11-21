// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#ifndef GESHER_NATIVE_CLEANUP_PROBE_H
#define GESHER_NATIVE_CLEANUP_PROBE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct CleanupLink CleanupLink;
typedef uint64_t (*CleanupCallback)(CleanupLink*);
typedef void (*CleanupNotify)(void*, int event, int id, int ok);
struct CleanupLink {
  CleanupNotify notify;
  void* state;
  CleanupCallback callback;
  int level;
};
enum {
  CLEANUP_ENTER = 1, CLEANUP_LEAVE, CLEANUP_AFTER,
  CLEANUP_FOREIGN_OPEN, CLEANUP_FOREIGN_CLOSE
};
uint64_t CleanupCBridge(CleanupLink*);
uint64_t CleanupForeignFinally(CleanupLink*, CleanupCallback);
#ifdef __cplusplus
}
#endif
#endif
