// gesher-base: POSIX additions to <stdlib.h>.
#pragma once

#include_next <stdlib.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The Windows CRT's RAND_MAX is 32767; Linux code assumes 2^31-1 (ART
// static_asserts at least 65535). rand()/srand() are redirected to a
// generator with the glibc range.
#undef RAND_MAX
#define RAND_MAX 0x7fffffff
int gesher_rand(void);
void gesher_srand(unsigned int seed);
#define rand() gesher_rand()
#define srand(seed) gesher_srand(seed)
#ifdef __cplusplus
}  // extern "C"
// So that std::rand() and std::srand() find the redirected functions too.
namespace std {
using ::gesher_rand;
using ::gesher_srand;
}  // namespace std
extern "C" {
#endif

char* realpath(const char* path, char* resolved_path);
char* mkdtemp(char* name);
int mkstemp(char* name);
int mkostemp(char* name, int flags);
int mkstemps(char* name, int suffix);
int mkostemps(char* name, int suffix, int flags);
int posix_memalign(void** memptr, size_t alignment, size_t size);
int setenv(const char* name, const char* value, int overwrite);
// BSD's random()/srandom(), over the same generator as rand() (0..2^31-1).
long random(void);
void srandom(unsigned int seed);
int unsetenv(const char* name);

#ifdef __cplusplus
}
#endif
