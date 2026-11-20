#pragma once

#include <cstdint>

struct AMediaFormat;
void AMediaFormat_setString(AMediaFormat*, const char*, const char*);
void AMediaFormat_setInt32(AMediaFormat*, const char*, int32_t);
