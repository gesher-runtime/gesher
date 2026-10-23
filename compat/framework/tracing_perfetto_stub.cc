// Gesher: perfetto, stubbed. android.os.Trace's natives hand every event to
// tracing_perfetto (frameworks/base), which Gesher does not build; here every
// tag is off and tracing is a no-op (docs/framework-natives.md).
#include <tracing_perfetto.h>

namespace tracing_perfetto {

void registerWithPerfetto(bool) {}

void traceBegin(uint64_t, const char*) {}
void traceEnd(uint64_t) {}
void traceAsyncBegin(uint64_t, const char*, int32_t) {}
void traceAsyncEnd(uint64_t, const char*, int32_t) {}
void traceAsyncBeginForTrack(uint64_t, const char*, const char*, int32_t) {}
void traceAsyncEndForTrack(uint64_t, const char*, int32_t) {}
void traceInstant(uint64_t, const char*) {}
void traceInstantForTrack(uint64_t, const char*, const char*) {}
void traceCounter(uint64_t, const char*, int64_t) {}
bool isTagEnabled(uint64_t) {
  return false;
}

}  // namespace tracing_perfetto
