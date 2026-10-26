// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Keep the pinned host liblog implementation, replacing only its property-free
// loggability fallback. No AOSP patch or host-tools behavior change is needed.
#define __android_log_is_loggable gesher_original_log_is_loggable
#define __android_log_is_loggable_len gesher_original_log_is_loggable_len
#include "properties.cpp"
#undef __android_log_is_loggable
#undef __android_log_is_loggable_len
#include <gesher/logging.h>

extern "C" int __android_log_is_loggable_len(int priority, const char* tag, size_t length, int fallback) {
  if (!tag || length == 0) { tag = GetDefaultTag().c_str(); length = GetDefaultTag().size(); }
  return gesher::logging::IsLoggable(priority, tag, length, fallback, __android_log_get_minimum_priority());
}
extern "C" int __android_log_is_loggable(int priority, const char* tag, int fallback) {
  return __android_log_is_loggable_len(priority, tag, tag ? strlen(tag) : 0, fallback);
}
extern "C" const char* gesher_android_default_log_tag() { return GetDefaultTag().c_str(); }
