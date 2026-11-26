// A library whose constructor forks (arm64_fork_adversarial.c, mode ctor): the
// fork runs under the ELF loader's lock, held by the forking thread itself.
// Only when ARM64_FORK_CTOR is set: mode cbfork loads it as a plain library.
#include <stdlib.h>
#include <unistd.h>
int g_ctor_pid = -1;
__attribute__((constructor)) static void init(void) {
  if (getenv("ARM64_FORK_CTOR") != NULL) g_ctor_pid = fork();
}
