// Gesher: what the extractors use of the NDK binder header. They ask who
// calls them to decide on float output for more than 16-bit PCM, which they
// give the media server (AID_MEDIA); Gesher's player takes float, so it is
// the media server.
#pragma once

#include <sys/types.h>

static inline uid_t AIBinder_getCallingUid() { return 1013; /* AID_MEDIA */ }
