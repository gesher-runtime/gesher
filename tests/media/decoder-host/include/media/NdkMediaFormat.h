// The unused codec-configuration writer in video_decoder.cc only needs this
// declaration. The probe uses a real null Format, without an Android runtime.
#pragma once
#include <cstddef>
struct AMediaFormat;
extern "C" void AMediaFormat_setBuffer(AMediaFormat*, const char*, const void*, size_t);
