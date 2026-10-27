// libexec2test.so: the exec2 track's driver. Run as the main program through
// gesher-exec, it starts libexec2child.so (its path in argv[1]) through the
// process APIs an app's native code uses -- fork()+exec*(), system(), popen(),
// posix_spawn() -- and sends it signals, checking each against what Android
// would do (docs/exec.md, ADR 0055). Each check prints "exec2: PASS <name>" or
// "exec2: FAIL <name> ..."; the last line is "exec2: SUMMARY pass=P fail=F".
//
// With "--exec-replace <child>" as the arguments it instead execv()s the child
// ("exit 17"), to show exec outside a fork replaces the image (the launcher
// process exits 17).
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

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

int main(int argc, char** argv) {
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
