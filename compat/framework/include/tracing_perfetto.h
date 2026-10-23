// Gesher: stands in for frameworks/base's libandroid_tracing_perfetto header.
// Perfetto is not ported; every tag is off and the trace calls do nothing
// (docs/framework-natives.md). Only what android_os_Trace.cpp uses is here.
#pragma once

#include <stdint.h>

namespace tracing_perfetto {

void registerWithPerfetto(bool test = false);

void traceBegin(uint64_t category, const char* name);
void traceEnd(uint64_t category);
void traceAsyncBegin(uint64_t category, const char* name, int32_t cookie);
void traceAsyncEnd(uint64_t category, const char* name, int32_t cookie);
void traceAsyncBeginForTrack(uint64_t category, const char* name, const char* trackName,
                             int32_t cookie);
void traceAsyncEndForTrack(uint64_t category, const char* trackName, int32_t cookie);
void traceInstant(uint64_t category, const char* name);
void traceInstantForTrack(uint64_t category, const char* trackName, const char* name);
void traceCounter(uint64_t category, const char* name, int64_t value);
bool isTagEnabled(uint64_t category);

}  // namespace tracing_perfetto
