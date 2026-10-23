// Gesher: stands in for the C++ aconfig library of com.android.server.backup
// (backup_flags_cc_lib, generated from frameworks/base/services/backup/
// flags.aconfig). Only the flag libandroidfw's BackupHelpers reads, fixed at
// its bp4a value: build/release enables it from bp1a on (read-only).
#pragma once

#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
namespace com::android::server::backup {
inline bool enable_max_size_writes_to_pipes() {
  return true;
}
}  // namespace com::android::server::backup

extern "C" {
#endif

static inline bool com_android_server_backup_enable_max_size_writes_to_pipes(void) {
  return true;
}

#ifdef __cplusplus
}
#endif
