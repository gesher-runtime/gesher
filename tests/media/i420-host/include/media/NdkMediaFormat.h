// Declaration only for the full converter translation unit. The standalone
// link discards unused codec/metadata sections; no provider success is faked.
#pragma once
#include <cstddef>
struct AMediaFormat;
extern "C" void AMediaFormat_setBuffer(AMediaFormat*, const char*, const void*, size_t);
