#pragma once
#include <stddef.h>
struct gesher_builtin_symbol { const char* name; void* address; };
inline void gesher_register_builtin_library(const char*, const gesher_builtin_symbol*, size_t) { }
