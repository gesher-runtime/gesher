// Gesher: the part of bionic's <sys/system_properties.h> libRS uses
// (frameworks/rs rsCppUtils.h: property_get over __system_property_get).
// The function is gesher-base's (compat/posix/src/base_properties.cc).
#pragma once

#define PROP_NAME_MAX 32
#define PROP_VALUE_MAX 92

#ifdef __cplusplus
extern "C" {
#endif

int __system_property_get(const char* name, char* value);

#ifdef __cplusplus
}
#endif
