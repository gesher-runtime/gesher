// Host declarations only; no NDK or Windows runtime is provided here.
#pragma once
#include <cstdint>
#include <cstddef>
using media_status_t = int32_t;
constexpr media_status_t AMEDIA_OK=0, AMEDIA_ERROR_MALFORMED=-10001,
 AMEDIA_ERROR_UNSUPPORTED=-10002, AMEDIA_ERROR_INVALID_OBJECT=-10003,
 AMEDIA_ERROR_INVALID_PARAMETER=-10004, AMEDIA_ERROR_INVALID_OPERATION=-10005,
 AMEDIA_ERROR_END_OF_STREAM=-10006, AMEDIA_ERROR_IO=-10007, AMEDIA_ERROR_UNKNOWN=-10000, AMEDIA_ERROR_WOULD_BLOCK=-10008;
struct AMediaFormat;
AMediaFormat* AMediaFormat_new();
media_status_t AMediaFormat_delete(AMediaFormat*);
void AMediaFormat_clear(AMediaFormat*);
media_status_t AMediaFormat_copy(AMediaFormat*, const AMediaFormat*);
void AMediaFormat_setInt32(AMediaFormat*,const char*,int32_t);
void AMediaFormat_setInt64(AMediaFormat*,const char*,int64_t);
void AMediaFormat_setString(AMediaFormat*,const char*,const char*);
void AMediaFormat_setBuffer(AMediaFormat*,const char*,const void*,size_t);
bool AMediaFormat_getInt32(AMediaFormat*,const char*,int32_t*);
bool AMediaFormat_getInt64(AMediaFormat*,const char*,int64_t*);
bool AMediaFormat_getRect(AMediaFormat*,const char*,int32_t*,int32_t*,int32_t*,int32_t*);
bool AMediaFormat_getFloat(AMediaFormat*,const char*,float*);
bool AMediaFormat_getString(AMediaFormat*,const char*,const char**);
bool AMediaFormat_getBuffer(AMediaFormat*,const char*,void**,size_t*);
const char* AMediaFormat_toString(AMediaFormat*);
