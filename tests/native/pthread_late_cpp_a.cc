#include "pthread_late_cpp_shared.h"
namespace {
struct ProbeA {
  LateCppCase *state=nullptr;
  ~ProbeA() { if (state) { LateCppRecord(state,"A"); ++state->a_done; } }
};
thread_local ProbeA object;
}
void LateCppTouchA(LateCppCase *state) { object.state=state; ++state->constructed; }
