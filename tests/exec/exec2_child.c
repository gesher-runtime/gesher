// libexec2child.so: the program the exec2 driver (exec2_test.c) starts through
// fork+exec, system, popen and posix_spawn, and signals. Built by
// .github/workflows/exec.yml with the NDK; run on Windows through gesher-exec
// (docs/exec.md, ADR 0055).
//
//   libexec2child.so echo [args...]   print the arguments
//   libexec2child.so env NAME         print getenv(NAME)
//   libexec2child.so cwd              print the working directory
//   libexec2child.so cat              copy stdin to stdout
//   libexec2child.so exit N           exit(N)
//   libexec2child.so sigloop          install SIGTERM/SIGUSR1 handlers, print
//                                     "child-ready", then wait; SIGTERM exits
//                                     42 if a SIGUSR1 arrived first, else 43
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <unistd.h>

static volatile sig_atomic_t g_usr1;

static void on_usr1(int sig) {
  (void)sig;
  g_usr1 = 1;
}
static void on_term(int sig) {
  (void)sig;
  _exit(g_usr1 ? 42 : 43);
}

static void property_value(void* cookie, const char* name, const char* value, unsigned serial) {
  (void)cookie;
  (void)name;
  printf("property-value:%s:%u\n", value, serial);
}

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "echo";
  if (strcmp(mode, "echo") == 0) {
    for (int i = 2; i < argc; ++i) printf("%s%s", i > 2 ? " " : "", argv[i]);
    printf("\n");
    fflush(stdout);
    return 0;
  }
  if (strcmp(mode, "env") == 0 && argc > 2) {
    const char* v = getenv(argv[2]);
    printf("%s\n", v ? v : "(null)");
    fflush(stdout);
    return 0;
  }
  if (strcmp(mode, "cwd") == 0) {
    char cwd[4096];
    printf("%s\n", getcwd(cwd, sizeof(cwd)) ? cwd : "(error)");
    fflush(stdout);
    return 0;
  }
  if (strcmp(mode, "cat") == 0) {
    char buf[8192];
    ssize_t n;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
      ssize_t off = 0;
      while (off < n) {
        ssize_t w = write(1, buf + off, (size_t)(n - off));
        if (w <= 0) return 3;
        off += w;
      }
    }
    return 0;
  }
  if (strcmp(mode, "exit") == 0 && argc > 2) {
    return atoi(argv[2]);
  }
  if (strcmp(mode, "property-get") == 0 && argc > 2) {
    char value[PROP_VALUE_MAX];
    __system_property_get(argv[2], value);
    printf("%s\n", value);
    return 0;
  }
  if (strcmp(mode, "property-set") == 0 && argc > 3) {
    return __system_property_set(argv[2], argv[3]) == 0 ? 0 : 1;
  }
  if (strcmp(mode, "property-watch") == 0 && argc > 2) {
    // serial is a platform libc export, absent from the public NDK header and
    // stub library. Resolve it as an NDK library querying the platform would.
    unsigned (*serial)(const prop_info*) = dlsym(RTLD_DEFAULT, "__system_property_serial");
    if (serial == NULL) return 5;
    const prop_info* property = __system_property_find(argv[2]);
    if (property == NULL) return 2;
    unsigned previous = serial(property);
    printf("property-ready:%u\n", previous);
    fflush(stdout);
    if (getchar() != '\n') return 3;
    __system_property_read_callback(property, property_value, NULL);
    return serial(property) != previous ? 0 : 4;
  }
  if (strcmp(mode, "sigloop") == 0 || strcmp(mode, "sigloop-pid") == 0) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    if (strcmp(mode, "sigloop-pid") == 0) printf("child-ready:%d\n", getpid());
    else printf("child-ready\n");
    fflush(stdout);
    for (;;) pause();
  }
  fprintf(stderr, "libexec2child: unknown mode %s\n", mode);
  return 2;
}
