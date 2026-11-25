// Gesher owns stream orientation. MinGW's inline fwide merely returns its
// argument and would bypass the shared host/guest stream state at -O2.
#pragma once
#pragma push_macro("__CRT__NO_INLINE")
#undef __CRT__NO_INLINE
#define __CRT__NO_INLINE 1
#include_next <wchar.h>
#pragma pop_macro("__CRT__NO_INLINE")
