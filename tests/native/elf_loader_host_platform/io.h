#pragma once
#include <cstdint>
// Force the production FileImage fallback that reads the actual ELF bytes.
inline intptr_t _get_osfhandle(int) { return -1; }
