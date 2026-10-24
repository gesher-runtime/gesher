// Gesher: perfetto's track-event macros, as no-ops. Gesher does not build
// perfetto (compat/framework/tracing_perfetto_stub.cc has every category off),
// so hwui's events are never emitted; their arguments are not evaluated.
#pragma once

#include <stdint.h>

struct PerfettoTeCategory {
  bool* enabled;
};

#define PERFETTO_TE(...) ((void)0)

static inline uint64_t PerfettoTeGlobalFlow(uint64_t id) {
  return id;
}
