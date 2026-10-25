// libexectest.so: an Android x86-64 executable (a PIE, named like a library
// as apps ship them in lib/x86_64) for tests/windows/tracks/42-exec.ps1 and
// gesher.ExecTest. Built by .github/workflows/exec.yml with the NDK; runs on
// Windows through gesher-exec.exe (docs/exec.md).
//
//   libexectest.so echo [args...]   argv, environment, cwd, auxv, TLS, threads
//   libexectest.so cat               stdin to stdout; byte count on stderr
//   libexectest.so big <n>           n bytes of a pattern to stdout (write(2))
//   libexectest.so exit <n>          exit(n) after atexit/destructor output
//   libexectest.so abort | segv      die by SIGABRT / SIGSEGV
//   libexectest.so sleep             print "sleeping", then sleep 60 s
//   libexectest.so stderr            one line to stdout, one to stderr
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <unistd.h>

extern char** environ;
extern const char* __progname;
const char* exec_helper_hello(void);

static int g_ctor_argc = -1;
static int g_report_dtor;  // only "exit" reports its destructor
static __thread int t_value = 7;  // emulated TLS (API 26)

__attribute__((constructor)) static void exec_ctor(int argc, char** argv, char** envp) {
  (void)argv;
  g_ctor_argc = envp != NULL ? argc : -2;
}

__attribute__((destructor)) static void exec_dtor(void) {
  static const char m[] = "destructor ran\n";
  if (g_report_dtor) write(1, m, sizeof(m) - 1);
}

static void on_exit_handler(void) { printf("atexit ran\n"); }

static void* thread_main(void* arg) {
  t_value = 99;
  *(int*)arg = t_value + 1;
  return NULL;
}

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "echo";
  if (strcmp(mode, "echo") == 0) {
    printf("argc=%d\n", argc);
    for (int i = 0; i < argc; ++i) printf("argv[%d]=<%s>\n", i, argv[i]);
    const char* v = getenv("EXEC_TEST_VAR");
    printf("getenv=<%s>\n", v ? v : "(null)");
    int found = 0;
    for (char** e = environ; *e != NULL; ++e) {
      if (strncmp(*e, "EXEC_TEST_VAR=", 14) == 0) found = 1;
    }
    printf("environ has EXEC_TEST_VAR=%d\n", found);
    char cwd[1024];
    printf("cwd=<%s>\n", getcwd(cwd, sizeof(cwd)) ? cwd : "(error)");
    printf("progname=<%s>\n", __progname);
    printf("pagesz=%lu phdr=%s random=%s\n", getauxval(AT_PAGESZ), getauxval(AT_PHDR) ? "set" : "unset",
           getauxval(AT_RANDOM) ? "set" : "unset");
    printf("ctor argc=%d\n", g_ctor_argc);
    printf("helper=<%s>\n", exec_helper_hello());
    int result = 0;
    pthread_t t;
    pthread_create(&t, NULL, thread_main, &result);
    pthread_join(t, NULL);
    printf("thread=%d tls=%d\n", result, t_value);
    printf("echo done\n");
    return 0;
  }
  if (strcmp(mode, "cat") == 0) {
    char buf[4096];
    long total = 0;
    ssize_t n;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
      ssize_t off = 0;
      while (off < n) {
        ssize_t w = write(1, buf + off, (size_t)(n - off));
        if (w <= 0) return 3;
        off += w;
      }
      total += n;
    }
    fprintf(stderr, "cat: %ld bytes\n", total);
    return n < 0 ? 4 : 0;
  }
  if (strcmp(mode, "big") == 0 && argc > 2) {
    long n = atol(argv[2]);
    long left = n;
    while (left > 0) {
      char tmp[8192];
      size_t chunk = left < (long)sizeof(tmp) ? (size_t)left : sizeof(tmp);
      // byte i of the output is 'a' + i % 26
      long pos = n - left;
      for (size_t i = 0; i < chunk; ++i) tmp[i] = (char)('a' + (pos + (long)i) % 26);
      ssize_t w = write(1, tmp, chunk);
      if (w <= 0) return 3;
      left -= w;
    }
    return 0;
  }
  if (strcmp(mode, "exit") == 0 && argc > 2) {
    g_report_dtor = 1;
    atexit(on_exit_handler);
    printf("exiting with %s\n", argv[2]);
    exit(atoi(argv[2]));
  }
  if (strcmp(mode, "abort") == 0) {
    printf("aborting\n");
    fflush(stdout);
    abort();
  }
  if (strcmp(mode, "segv") == 0) {
    printf("faulting\n");
    fflush(stdout);
    *(volatile int*)(uintptr_t)16 = 1;
    return 0;
  }
  if (strcmp(mode, "sleep") == 0) {
    printf("sleeping\n");
    fflush(stdout);
    sleep(60);
    printf("woke up\n");
    return 0;
  }
  if (strcmp(mode, "stderr") == 0) {
    printf("to stdout\n");
    fflush(stdout);
    fprintf(stderr, "to stderr\n");
    return 0;
  }
  fprintf(stderr, "libexectest: unknown mode %s\n", mode);
  return 2;
}
