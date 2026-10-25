// Gesher (devices track): what libcamera_metadata needs that the Windows C
// runtime lacks (force-included into camera_metadata.c by devices.cmake).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// dprintf(3): camera_metadata.c's dump functions print to a file descriptor.
int gesher_camera_metadata_dprintf(int fd, const char* format, ...)
    __attribute__((format(printf, 2, 3)));
#define dprintf gesher_camera_metadata_dprintf

#ifdef __cplusplus
}
#endif
