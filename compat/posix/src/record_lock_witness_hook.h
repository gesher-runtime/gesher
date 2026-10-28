#pragma once
// Compile-time-only observer in the dedicated CI executable. No production
// activation path, environment variable or protocol command enables this.
#ifdef GESHER_RECORD_WAIT_WITNESS_TEST
namespace gesher::record_locks {
extern thread_local void (*record_wait_phase)(bool entering);
}
#endif
