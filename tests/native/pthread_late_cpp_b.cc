#include "pthread_late_cpp_shared.h"
namespace {
struct ProbeB {
  LateCppCase *state=nullptr;
  ~ProbeB() { if (state) LateCppRecord(state,"B"); }
};
thread_local ProbeB object;
}
void LateCppTouchB(LateCppCase *state) { object.state=state; ++state->constructed; }
