// Included ahead of every Skia source (cmake/aosp/skia.cmake).
//
// gesher-base's <stdlib.h> turns rand() and srand() into function-like macros
// (for the glibc RAND_MAX); Skia has locals named rand (`LCGRandom rand(seed)`),
// which the macro would swallow. Skia does not call the C library's rand.
#pragma once
#include <stdlib.h>
#undef rand
#undef srand
