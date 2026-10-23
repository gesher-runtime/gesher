// gesher-base: <linux/sched.h>, the scheduling policy names the kernel uses
// (libbinder's Parcel encodes SCHED_NORMAL into flat_binder_objects).
#pragma once

#include <sched.h>

#ifndef SCHED_NORMAL
#define SCHED_NORMAL 0
#endif
#ifndef SCHED_BATCH
#define SCHED_BATCH 3
#endif
#ifndef SCHED_IDLE
#define SCHED_IDLE 5
#endif
