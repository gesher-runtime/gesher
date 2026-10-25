// Gesher media: logging, through liblog (tag "GesherMedia") in the runtime,
// or stderr in standalone builds (the probe built outside CMake).
#pragma once

#ifdef GESHER_MEDIA_STANDALONE
#include <cstdio>
#define MEDIA_LOG_(level, fmt, ...) fprintf(stderr, "GesherMedia " level ": " fmt "\n", ##__VA_ARGS__)
#define MEDIA_LOGI(fmt, ...) MEDIA_LOG_("I", fmt, ##__VA_ARGS__)
#define MEDIA_LOGW(fmt, ...) MEDIA_LOG_("W", fmt, ##__VA_ARGS__)
#define MEDIA_LOGE(fmt, ...) MEDIA_LOG_("E", fmt, ##__VA_ARGS__)
#else
#include <android/log.h>
#define MEDIA_LOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO, "GesherMedia", fmt, ##__VA_ARGS__)
#define MEDIA_LOGW(fmt, ...) __android_log_print(ANDROID_LOG_WARN, "GesherMedia", fmt, ##__VA_ARGS__)
#define MEDIA_LOGE(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, "GesherMedia", fmt, ##__VA_ARGS__)
#endif
