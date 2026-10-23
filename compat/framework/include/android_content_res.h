// Gesher: stands in for the C++ aconfig library of android.content.res
// (android.content.res.flags-aconfig-cc, generated from
// frameworks/base/core/java/android/content/res/flags.aconfig). Only the flags
// libandroidfw reads, fixed at their bp4a values: build/release sets none of
// them, so each is the declaration's default, disabled.
#pragma once

#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
namespace android::content::res {
inline bool resource_readwrite_flags() {
  return false;
}
}  // namespace android::content::res

extern "C" {
#endif

static inline bool android_content_res_resource_readwrite_flags(void) {
  return false;
}

#ifdef __cplusplus
}
#endif
