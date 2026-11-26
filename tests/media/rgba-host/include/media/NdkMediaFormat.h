// Declaration for video_decoder.cc's unused codec-configuration writer.
// The standalone converter probe aborts if that metadata boundary is invoked.
#pragma once
#include <cstddef>
struct AMediaFormat;
extern "C" void AMediaFormat_setBuffer(AMediaFormat*, const char*, const void*, size_t);
