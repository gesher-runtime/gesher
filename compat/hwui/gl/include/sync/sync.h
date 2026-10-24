// Gesher: libsync's sync_wait (system/core/libsync), for hwui's EglManager.
// Gesher has no sync files: EGL_ANDROID_native_fence_sync is not offered
// (ANGLE has no Android fences), so the fences hwui waits on are -1 and
// sync_wait is not reached; compat/hwui/gl/hwui_gl_standins.cc fails it.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int sync_wait(int fd, int timeout);

#ifdef __cplusplus
}
#endif
