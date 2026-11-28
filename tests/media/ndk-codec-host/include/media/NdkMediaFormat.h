// Explicit metadata boundary for the complete production codec/NDK units.
#pragma once
#include <cstddef>
#include <cstdint>

#include "NdkMediaError.h"
struct AMediaFormat;
AMediaFormat* AMediaFormat_new();
media_status_t AMediaFormat_delete(AMediaFormat*);
media_status_t AMediaFormat_copy(AMediaFormat*, AMediaFormat*);
void AMediaFormat_setString(AMediaFormat*, const char*, const char*);
void AMediaFormat_setInt32(AMediaFormat*, const char*, int32_t);
void AMediaFormat_setInt64(AMediaFormat*, const char*, int64_t);
void AMediaFormat_setBuffer(AMediaFormat*, const char*, const void*, size_t);
