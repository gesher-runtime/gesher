// gesher-base: <sys/wait.h>. Gesher does not create child processes the
// POSIX way (no fork), so waitpid fails with ECHILD.
#pragma once

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WNOHANG    1
#define WUNTRACED  2

#define WEXITSTATUS(s) (((s) & 0xff00) >> 8)
#define WTERMSIG(s)    ((s) & 0x7f)
#define WSTOPSIG(s)    WEXITSTATUS(s)
#define WIFEXITED(s)   (WTERMSIG(s) == 0)
#define WIFSIGNALED(s) (((signed char)(((s) & 0x7f) + 1) >> 1) > 0)
#define WIFSTOPPED(s)  (((s) & 0xff) == 0x7f)

pid_t waitpid(pid_t pid, int* status, int options);
pid_t wait(int* status);

#ifdef __cplusplus
}
#endif
