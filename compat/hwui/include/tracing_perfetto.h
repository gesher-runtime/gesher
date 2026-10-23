// Gesher: tracing_perfetto as hwui uses it. The framework's stand-in
// (compat/framework/include) has every tag off; hwui's Bitmap natives also ask
// for a perfetto category, and get none.
#pragma once

#include_next <tracing_perfetto.h>

struct PerfettoTeCategory;

namespace tracing_perfetto {

inline PerfettoTeCategory* getPerfettoCategory(uint64_t) {
  return nullptr;
}

}  // namespace tracing_perfetto
