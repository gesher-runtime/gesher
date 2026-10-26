// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// ART/libbase replaces the logger during startup. Keep recording around every
// selected logger instead of relying on a constructor's one-time registration.
#define __android_log_set_logger gesher_original_android_log_set_logger
#include "logger_write.cpp"
#undef __android_log_set_logger
#include <gesher/logging.h>
#include <gesher/system_properties.h>

static std::atomic<__android_logger_function> forward_logger{__android_log_stderr_logger};
static void GesherLogger(const __android_log_message* message) {
  gesher::logging::Record(message->buffer_id, message->priority, message->tag, message->message);
  if (auto logger = forward_logger.load(std::memory_order_relaxed); logger && logger != GesherLogger) logger(message);
}
extern "C" void __android_log_set_logger(__android_logger_function logger) {
  forward_logger.store(logger, std::memory_order_relaxed);
  gesher_original_android_log_set_logger(GesherLogger);
}
extern "C" void gesher_android_log_anchor() {}
__attribute__((constructor)) static void InstallGesherLogger() {
  // Establish the boot's area before tests/apps change ANDROID_DATA for scratch.
  gesher::properties::Initialize();
  gesher_original_android_log_set_logger(GesherLogger);
}
