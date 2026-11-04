// libexec2test.so: the exec2 track's driver. Run as the main program through
// gesher-exec, it starts libexec2child.so (its path in argv[1]) through the
// process APIs an app's native code uses -- fork()+exec*(), system(), popen(),
// posix_spawn() -- and sends it signals, checking each against what Android
// would do (docs/exec.md, ADR 0055). Each check prints "exec2: PASS <name>" or
// "exec2: FAIL <name> ..."; the last line is "exec2: SUMMARY pass=P fail=F".
//
// With "--exec-replace <child>" as the arguments it instead execv()s the child
// ("exit 17"), to show exec outside a fork replaces the image (the launcher
// process exits 17). With "--dart-start <child>" it starts the child as dart:io's
// Process.start does (dart_start below); the track runs that one with a time
// limit, as a regression there hangs. With "--fork-fds" it checks that a fork
// child's fd calls and failed exec leave this process's fds alone (fork_fds).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // pipe2
#endif
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
// Declared here too: exec3_jni.c includes this file after <unistd.h>, so the
// _GNU_SOURCE above comes too late for bionic's declaration.
int pipe2(int fds[2], int flags);

static int g_pass, g_fail;

// Opt-in boundaries for the real APK process, without replacing any operation
// or assertion. A fatal signal can otherwise erase which spawn/stdio step ran.
static void phase(const char* name) {
  if (getenv("GESHER_TRACE_EXEC_PHASES") == NULL) return;
  fprintf(stderr, "exec4-phase: %s\n", name);
  fflush(stderr);
}

static void ok(const char* name, int cond, const char* detail) {
  if (cond) {
    printf("exec2: PASS %s\n", name);
    ++g_pass;
  } else {
    printf("exec2: FAIL %s (%s)\n", name, detail ? detail : "");
    ++g_fail;
  }
  fflush(stdout);
}

// dart:io's ProcessStarter (runtime/bin/process_android.cc), how a Flutter app runs a
// program: O_CLOEXEC pipes; the fork child first waits for one byte the parent writes
// once it has registered the pid, then puts its stdout on a pipe and execs; the parent
// reads the exec-status pipe until the program's end of it closes at exec (its errno if
// exec failed), then the program's output. The fork child runs before its parent (a
// vfork-style child, docs/exec.md), so the child's read must not wait for the parent;
// the parent's byte must not reach the program's output either. The program keeps
// running (sigloop) until it is killed, so the exec-status pipe ends only if exec
// closed it.
static void dart_start(const char* child) {
  int exec_control[2], read_in[2];
  if (pipe2(exec_control, O_CLOEXEC) != 0 || pipe2(read_in, O_CLOEXEC) != 0) {
    ok("a fork child waits for its parent's byte before exec (dart:io Process.start)", 0, "pipe2");
    return;
  }
  pid_t pid = fork();
  if (pid == 0) {
    char go;
    if (read(read_in[0], &go, 1) != 1) _exit(126);
    dup2(read_in[1], 1);
    char* a[] = {(char*)child, (char*)"sigloop", NULL};
    execvp(child, a);
    int e = errno;
    ssize_t reported = write(exec_control[1], &e, sizeof(e));
    (void)reported;
    _exit(127);
  }
  ok("a fork child waits for its parent's byte before exec (dart:io Process.start)", pid > 0,
     "fork");
  if (pid <= 0) return;
  char go = '1';
  ssize_t sent = write(read_in[1], &go, 1);
  close(exec_control[1]);
  int child_errno = 0;
  ssize_t got = read(exec_control[0], &child_errno, sizeof(child_errno));
  close(exec_control[0]);
  close(read_in[1]);
  ok("exec closes the O_CLOEXEC fds: the exec-status pipe ends while the program runs",
     sent == 1 && got == 0, "exec status");
  // The program's whole output, once it is gone: its "child-ready" line, nothing else.
  char out[128] = {0};
  size_t used = 0;
  ssize_t n = read(read_in[0], out, sizeof(out) - 1);
  if (n > 0) used = (size_t)n;
  kill(pid, SIGKILL);
  int status = -1;
  waitpid(pid, &status, 0);
  while (used < sizeof(out) - 1 && (n = read(read_in[0], out + used, sizeof(out) - 1 - used)) > 0) {
    used += (size_t)n;
  }
  close(read_in[0]);
  ok("the parent's byte for the child is not in the program's output",
     strcmp(out, "child-ready\n") == 0, out);
}

// dart:io's Process.start of a program that is not there ("which" on a device without
// it): the child's exec fails and it exits before its parent has written its byte. On
// Linux it could not have: it was still waiting for that byte, which the parent writes
// once it has registered the pid. Dart's exit-code thread, in wait() for another
// program, reaped such a child before its registration, kept counting it and aborted
// ("Wait for process exit failed: 10"). So it is not waited for until the byte.
static void dart_start_missing(void) {
  int exec_control[2], read_in[2];
  if (pipe2(exec_control, O_CLOEXEC) != 0 || pipe2(read_in, O_CLOEXEC) != 0) {
    ok("a fork child whose exec fails is not waited for before its parent's byte", 0, "pipe2");
    return;
  }
  pid_t pid = fork();
  if (pid == 0) {
    char go;
    if (read(read_in[0], &go, 1) != 1) _exit(126);
    char* a[] = {(char*)"/system/xbin/gesher-no-such-program", NULL};
    execvp(a[0], a);
    int e = errno;
    ssize_t reported = write(exec_control[1], &e, sizeof(e));
    (void)reported;
    _exit(127);
  }
  int status = -1;
  pid_t early = waitpid(-1, &status, WNOHANG);
  char go = '1';
  ssize_t sent = write(read_in[1], &go, 1);
  close(exec_control[1]);
  int child_errno = 0;
  ssize_t got = read(exec_control[0], &child_errno, sizeof(child_errno));
  close(exec_control[0]);
  close(read_in[0]);
  close(read_in[1]);
  ok("a fork child whose exec fails is not waited for before its parent's byte",
     pid > 0 && early == 0, "waitpid(-1, WNOHANG) before the byte");
  pid_t reaped = waitpid(-1, &status, 0);
  ok("then it is, with exit 127 and exec's errno",
     sent == 1 && got == (ssize_t)sizeof(child_errno) && child_errno == ENOENT &&
         reaped == pid && WIFEXITED(status) && WEXITSTATUS(status) == 127,
     "waitpid after the byte");
}

// A fork child that drops every fd above 2 before an exec that fails, as Java's
// childproc (closeDescriptors) and root checkers ("exec which/su/busybox") do. The
// vfork-style child runs in this process (docs/exec.md), so its close loop,
// closefrom/close_range, dup2 onto an fd and fcntl(F_SETFD) must act on the
// child's fd table only, never on this process's fds (ART's among them), and its
// failed exec must return to the child, which then exits 127 as on Linux.
typedef int (*close_range_fn)(unsigned first, unsigned last, int flags);
typedef void (*closefrom_fn)(int lowest);

static void fork_fds(void) {
  const char* name_exit = "a fork child's failed exec returns to the child (ENOENT), which exits 127";
  const char* name_fds =
      "a fork child's close loop, closefrom, close_range and dup2 leave the parent's fds open";
  const char* name_flags = "a fork child's fcntl(F_SETFD) leaves the parent's FD_CLOEXEC alone";
  close_range_fn close_range_p = (close_range_fn)dlsym(RTLD_DEFAULT, "close_range");
  closefrom_fn closefrom_p = (closefrom_fn)dlsym(RTLD_DEFAULT, "closefrom");
  int p[2];
  if (pipe(p) != 0) {
    ok(name_fds, 0, "pipe");
    return;
  }
  int extra = dup(p[1]);
  int flags_before = fcntl(p[0], F_GETFD);
  pid_t pid = fork();
  if (pid == 0) {
    fcntl(p[0], F_SETFD, (flags_before & FD_CLOEXEC) != 0 ? 0 : FD_CLOEXEC);
    dup2(0, extra);
    for (int fd = 3; fd < 1024; ++fd) close(fd);
    if (closefrom_p != NULL) closefrom_p(3);
    if (close_range_p != NULL) close_range_p(3, ~0u, 0);
    char* a[] = {(char*)"/system/xbin/gesher-no-such-program", (char*)"su", NULL};
    execv(a[0], a);
    _exit(errno == ENOENT ? 127 : 126);
  }
  int status = -1;
  pid_t waited = pid > 0 ? waitpid(pid, &status, 0) : -1;
  char detail[128];
  snprintf(detail, sizeof(detail), "fork %d, waitpid %d, status 0x%x", (int)pid, (int)waited, status);
  ok(name_exit, pid > 0 && waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 127, detail);

  char c = 0;
  ssize_t wrote = write(extra, "x", 1);
  ssize_t got = wrote == 1 ? read(p[0], &c, 1) : -1;
  ssize_t wrote_again = write(p[1], "y", 1);
  snprintf(detail, sizeof(detail), "close_range %s, closefrom %s, write %d, read %d '%c', write %d",
           close_range_p != NULL ? "found" : "missing", closefrom_p != NULL ? "found" : "missing",
           (int)wrote, (int)got, c != 0 ? c : '?', (int)wrote_again);
  ok(name_fds,
     close_range_p != NULL && closefrom_p != NULL && wrote == 1 && got == 1 && c == 'x' &&
         wrote_again == 1,
     detail);
  int flags_after = fcntl(p[0], F_GETFD);
  snprintf(detail, sizeof(detail), "F_GETFD %d before, %d after", flags_before, flags_after);
  ok(name_flags, flags_before >= 0 && flags_after == flags_before, detail);
  close(p[0]);
  close(p[1]);
  close(extra);
}

int main(int argc, char** argv) {
  if (argc >= 2 && strcmp(argv[1], "--fork-fds") == 0) {
    fork_fds();
    printf("exec2: SUMMARY pass=%d fail=%d\n", g_pass, g_fail);
    fflush(stdout);
    return g_fail == 0 ? 0 : 1;
  }
  if (argc >= 3 && strcmp(argv[1], "--dart-start") == 0) {
    dart_start(argv[2]);
    dart_start_missing();
    printf("exec2: SUMMARY pass=%d fail=%d\n", g_pass, g_fail);
    fflush(stdout);
    return g_fail == 0 ? 0 : 1;
  }
  // exec outside a fork: replace this image with the child.
  if (argc >= 3 && strcmp(argv[1], "--exec-replace") == 0) {
    char* a[] = {argv[2], (char*)"exit", (char*)"17", NULL};
    execv(argv[2], a);
    perror("execv");
    return 99;
  }

  const char* child = argc > 1 ? argv[1] : "libexec2child.so";

  // --- system() --------------------------------------------------------------
  int rc = system("exit 5");
  ok("system runs the shell and returns its status",
     rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 5, "system exit 5");
  const char* no_command = NULL;  // a variable, so -Wnonnull does not fire
  ok("system(NULL) reports a shell is available", system(no_command) != 0, "system(NULL)");

  // --- popen() ---------------------------------------------------------------
  {
    FILE* f = popen("echo from-popen", "r");
    char line[128] = {0};
    int read_ok = f != NULL && fgets(line, sizeof(line), f) != NULL;
    if (f != NULL) {
      char* nl = strchr(line, '\n');
      if (nl) *nl = 0;
    }
    int status = f != NULL ? pclose(f) : -1;
    ok("popen reads a command's output",
       read_ok && strcmp(line, "from-popen") == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
       line);
  }

  // --- posix_spawn() with a file action (stdout to a pipe) -------------------
  {
    phase("spawn actions begin");
    int p[2];
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    phase("spawn actions initialized");
    posix_spawn_file_actions_adddup2(&fa, 0, 0);  // exercise a no-op dup2 too
    phase("spawn no-op dup2 recorded");
    char line[128] = {0};
    int spawned = -1, status = -1, got = 0;
    if (pipe(p) == 0) {
      phase("spawn pipe created");
      posix_spawn_file_actions_adddup2(&fa, p[1], 1);
      posix_spawn_file_actions_addclose(&fa, p[0]);
      posix_spawn_file_actions_addclose(&fa, p[1]);
      char* a[] = {(char*)child, (char*)"echo", (char*)"spawned", NULL};
      phase("spawn enter");
      spawned = posix_spawn(&got, child, &fa, NULL, a, environ);
      phase("spawn returned");
      close(p[1]);
      if (spawned == 0) {
        phase("spawn fdopen enter");
        FILE* f = fdopen(p[0], "r");
        phase("spawn fdopen returned");
        if (f) {
          phase("spawn fgets enter");
          if (fgets(line, sizeof(line), f)) {
            char* nl = strchr(line, '\n');
            if (nl) *nl = 0;
          }
          phase("spawn fgets returned");
          fclose(f);
          phase("spawn fclose returned");
        }
        waitpid(got, &status, 0);
        phase("spawn wait returned");
      } else {
        close(p[0]);
      }
    }
    posix_spawn_file_actions_destroy(&fa);
    phase("spawn actions destroyed");
    ok("posix_spawn with a dup2 file action, output captured",
       spawned == 0 && strcmp(line, "spawned") == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
       line);
  }

  // --- fork() + dup2 + execv(), stdout to a pipe -----------------------------
  {
    int p[2];
    char line[128] = {0};
    int status = -1;
    pid_t pid = -1;
    if (pipe(p) == 0) {
      pid = fork();
      if (pid == 0) {
        dup2(p[1], 1);
        close(p[0]);
        close(p[1]);
        char* a[] = {(char*)child, (char*)"echo", (char*)"forked", NULL};
        execv(child, a);
        _exit(127);
      }
      close(p[1]);
      FILE* f = fdopen(p[0], "r");
      if (f) {
        if (fgets(line, sizeof(line), f)) {
          char* nl = strchr(line, '\n');
          if (nl) *nl = 0;
        }
        fclose(f);
      }
      waitpid(pid, &status, 0);
    }
    ok("fork+dup2+execv runs the child with a redirected pipe",
       pid > 0 && strcmp(line, "forked") == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0, line);
  }

  // --- waitpid status: exit code and signal death ----------------------------
  {
    char* a[] = {(char*)child, (char*)"exit", (char*)"9", NULL};
    pid_t pid = -1;
    int status = -1;
    posix_spawn(&pid, child, NULL, NULL, a, environ);
    waitpid(pid, &status, 0);
    ok("waitpid decodes an exit code", WIFEXITED(status) && WEXITSTATUS(status) == 9, "exit 9");
  }

  // --- signals to another process: SIGUSR1 then SIGTERM, to its handlers -----
  {
    int p[2];
    char line[128] = {0};
    int status = -1;
    pid_t pid = -1;
    if (pipe(p) == 0) {
      posix_spawn_file_actions_t fa;
      posix_spawn_file_actions_init(&fa);
      posix_spawn_file_actions_adddup2(&fa, p[1], 1);
      posix_spawn_file_actions_addclose(&fa, p[0]);
      posix_spawn_file_actions_addclose(&fa, p[1]);
      char* a[] = {(char*)child, (char*)"sigloop", NULL};
      int sp = posix_spawn(&pid, child, &fa, NULL, a, environ);
      posix_spawn_file_actions_destroy(&fa);
      close(p[1]);
      if (sp == 0) {
        FILE* f = fdopen(p[0], "r");
        if (f && fgets(line, sizeof(line), f)) {
          // The child is ready (it printed "child-ready"): signal it.
          kill(pid, SIGUSR1);
          usleep(100000);
          kill(pid, SIGTERM);
        }
        if (f) fclose(f);
        waitpid(pid, &status, 0);
      }
    }
    ok("kill() to another process runs its SIGUSR1 and SIGTERM handlers",
       pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 42, "expected exit 42");
  }

  // --- signal 0: existence; SIGKILL: forced death ----------------------------
  {
    int p[2];
    pid_t pid = -1;
    int status = -1;
    int sp = -1;
    if (pipe(p) == 0) {
      posix_spawn_file_actions_t fa;
      posix_spawn_file_actions_init(&fa);
      posix_spawn_file_actions_adddup2(&fa, p[1], 1);  // its "child-ready" into the pipe
      posix_spawn_file_actions_addclose(&fa, p[0]);
      posix_spawn_file_actions_addclose(&fa, p[1]);
      char* a[] = {(char*)child, (char*)"sigloop", NULL};
      sp = posix_spawn(&pid, child, &fa, NULL, a, environ);
      posix_spawn_file_actions_destroy(&fa);
      close(p[1]);
      char line[64] = {0};
      FILE* f = fdopen(p[0], "r");
      if (f && fgets(line, sizeof(line), f)) { /* child ready */
      }
      if (f) fclose(f);
    }
    if (sp == 0) {
      int exists = kill(pid, 0);
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      ok("signal 0 finds a running process", exists == 0, "kill(pid,0)");
      ok("SIGKILL ends a process (WIFSIGNALED)",
         WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "expected signalled by 9");
    } else {
      ok("signal 0 finds a running process", 0, "spawn failed");
      ok("SIGKILL ends a process (WIFSIGNALED)", 0, "spawn failed");
    }
  }

  printf("exec2: SUMMARY pass=%d fail=%d\n", g_pass, g_fail);
  fflush(stdout);
  return g_fail == 0 ? 0 : 1;
}
