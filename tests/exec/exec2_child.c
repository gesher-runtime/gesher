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
#include <android/log.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <time.h>
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

typedef unsigned (*property_serial_fn)(const prop_info*);
typedef bool (*property_wait_fn)(const prop_info*, unsigned, unsigned*, const struct timespec*);
struct property_capture { const char* name; const char* value; unsigned serial; };
static void property_capture(void* cookie, const char* name, const char* value, unsigned serial) {
  struct property_capture* captured = cookie;
  *captured = (struct property_capture){name, value, serial};
}
struct property_enumeration { const char* name; unsigned count; int failed; };
static void property_enumerate(const prop_info* property, void* cookie) {
  struct property_enumeration* state = cookie;
  if (property == NULL || __system_property_find(state->name) == NULL) state->failed = 1;
  // Re-entering the property service must not hold either enumeration lock.
  if (state->count++ == 0 && __system_property_set(state->name, "enumerated") != 0) state->failed = 1;
}
static int property_abi(const char* prefix) {
  property_serial_fn serial = dlsym(RTLD_DEFAULT, "__system_property_serial");
  property_wait_fn wait = dlsym(RTLD_DEFAULT, "__system_property_wait");
  int (*read_property)(const prop_info*, char*, char*) = dlsym(RTLD_DEFAULT, "__system_property_read");
  int (*foreach)(void (*)(const prop_info*, void*), void*) = dlsym(RTLD_DEFAULT, "__system_property_foreach");
  unsigned checks = 0;
#define PROPERTY_CHECK(expression) do { ++checks; if (!(expression)) { \
  fprintf(stderr, "property ABI check %u failed: %s\n", checks, #expression); return 6; } } while (0)
  PROPERTY_CHECK(serial && wait && read_property && foreach);
  char name[256], ro[256], long_ro[256];
  snprintf(name, sizeof(name), "debug.%s.abi", prefix);
  snprintf(ro, sizeof(ro), "ro.%s.stable", prefix);
  snprintf(long_ro, sizeof(long_ro), "ro.%s.abi.long", prefix);
  PROPERTY_CHECK(__system_property_set(name, "abc") == 0);
  const prop_info* property = __system_property_find(name);
  PROPERTY_CHECK(property && serial(property) == (3U << 24));
  PROPERTY_CHECK(__system_property_set(name, "abc") == 0);
  unsigned previous = serial(property);
  PROPERTY_CHECK(previous == ((3U << 24) | 2U));
  struct timespec zero = {0, 0}, invalid = {0, 1000000000};
  unsigned changed = 0xdeadbeef;
  PROPERTY_CHECK(!wait(property, previous, &changed, &zero) && changed == 0xdeadbeef);
  PROPERTY_CHECK(wait(property, previous - 2, &changed, &zero) && changed == previous);
  changed = 0xdeadbeef;
  PROPERTY_CHECK(!wait(property, previous, &changed, &invalid) && changed == 0xdeadbeef);
  PROPERTY_CHECK(__system_property_set(ro, "constant") == 0);
  const prop_info* immutable = __system_property_find(ro);
  PROPERTY_CHECK(immutable != NULL);
  struct property_capture first, second;
  __system_property_read_callback(immutable, property_capture, &first);
  for (int i = 0; i < 128; ++i) {
    char value[92];
    memset(value, 'a' + i % 26, sizeof(value) - 1);
    value[sizeof(value) - 1] = 0;
    PROPERTY_CHECK(__system_property_set(name, value) == 0);
  }
  PROPERTY_CHECK(strcmp(first.value, "constant") == 0 && strcmp(first.name, ro) == 0);
  __system_property_read_callback(immutable, property_capture, &second);
  PROPERTY_CHECK(first.value == second.value && first.serial == (8U << 24));
  char long_value[181];
  memset(long_value, 'x', sizeof(long_value) - 1);
  long_value[sizeof(long_value) - 1] = 0;
  PROPERTY_CHECK(__system_property_set(long_ro, long_value) == 0);
  const prop_info* long_property = __system_property_find(long_ro);
  PROPERTY_CHECK(long_property != NULL);
  __system_property_read_callback(long_property, property_capture, &first);
  const char* legacy_error = "Must use __system_property_read_callback() to read";
  PROPERTY_CHECK(strcmp(first.value, long_value) == 0 &&
                 first.serial == ((unsigned)strlen(legacy_error) << 24 | (1U << 16)));
  char value[PROP_VALUE_MAX], old_name[32];
  PROPERTY_CHECK(__system_property_get(long_ro, value) == (int)strlen(legacy_error) &&
                 strcmp(value, legacy_error) == 0);
  PROPERTY_CHECK(read_property(long_property, old_name, value) == (int)strlen(legacy_error) &&
                 strcmp(value, legacy_error) == 0 && strncmp(old_name, long_ro, 31) == 0);
  struct property_enumeration enumerated = {name, 0, 0};
  PROPERTY_CHECK(foreach(property_enumerate, &enumerated) == 0 && enumerated.count > 0 && !enumerated.failed);
  for (const char** invalid_utf8 = (const char*[]){"\xc0\x80", "\xe0\x80\xa0", "\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", NULL};
       *invalid_utf8; ++invalid_utf8) PROPERTY_CHECK(__system_property_set(name, *invalid_utf8) != 0);
  PROPERTY_CHECK(__system_property_set(name, "\xf0\x9f\x98\x80") == 0);
  PROPERTY_CHECK(__system_property_get(name, value) == 4 && strcmp(value, "\xf0\x9f\x98\x80") == 0);
  printf("property-abi:PASS serial, zero timeout, stable ro pointers, long values and reentrant foreach\n");
  return 0;
#undef PROPERTY_CHECK
}

static int property_wait(const char* mode, const char* name) {
  property_serial_fn serial = dlsym(RTLD_DEFAULT, "__system_property_serial");
  property_wait_fn wait = dlsym(RTLD_DEFAULT, "__system_property_wait");
  unsigned (*area_serial)(void) = dlsym(RTLD_DEFAULT, "__system_property_area_serial");
  unsigned (*wait_any)(unsigned) = dlsym(RTLD_DEFAULT, "__system_property_wait_any");
  if (!serial || !wait || !area_serial || !wait_any) return 5;
  bool global = strcmp(mode, "property-wait-global") == 0 || strcmp(mode, "property-wait-any") == 0;
  bool timeout = strcmp(mode, "property-wait-timeout") == 0;
  const prop_info* property = global ? NULL : __system_property_find(name);
  if (!global && !property) return 2;
  unsigned previous = global ? area_serial() : serial(property);
  printf("property-ready:%u\n", previous);
  fflush(stdout);
  struct timespec relative = {timeout ? 0 : 5, timeout ? 400000000 : 0};
  unsigned changed = 0xdeadbeef;
  bool woke;
  if (strcmp(mode, "property-wait-any") == 0) {
    changed = wait_any(previous);
    woke = changed != previous;
  } else {
    woke = wait(property, previous, &changed, &relative);
  }
  if (timeout) {
    if (woke || changed != 0xdeadbeef) return 7;
    puts("property-timeout:PASS");
    return 0;
  }
  if (!woke || changed == previous) return 4;
  printf("property-woke:%u\n", changed);
  return 0;
}

static int log_abi(const char* tag) {
  int (*loggable)(int, const char*, int) = dlsym(RTLD_DEFAULT, "__android_log_is_loggable");
  int (*loggable_len)(int, const char*, size_t, int) = dlsym(RTLD_DEFAULT, "__android_log_is_loggable_len");
  int (*set_minimum)(int) = dlsym(RTLD_DEFAULT, "__android_log_set_minimum_priority");
  int (*get_minimum)(void) = dlsym(RTLD_DEFAULT, "__android_log_get_minimum_priority");
  if (!loggable || !loggable_len || !set_minimum || !get_minimum) return 5;
  char key[256], full_tag[256], full_key[512];
  snprintf(key, sizeof(key), "log.tag.%s", tag);
  snprintf(full_tag, sizeof(full_tag), "%s.extra", tag);
  snprintf(full_key, sizeof(full_key), "log.tag.%s", full_tag);
  int previous = set_minimum(ANDROID_LOG_DEFAULT);
  int failed = 0;
  if (__system_property_set(key, "E") || __system_property_set(full_key, "V")) failed = 1;
  if (loggable(3, tag, 2) || !loggable(6, tag, 2)) failed = 1;
  if (loggable_len(3, full_tag, strlen(tag), 2) || !loggable_len(3, full_tag, strlen(full_tag), 2)) failed = 1;
  set_minimum(5);
  if (get_minimum() != 5 || loggable(3, tag, 2) || !loggable(5, tag, 2)) failed = 1;
  if (__system_property_set(key, "")) failed = 1;
  set_minimum(ANDROID_LOG_DEFAULT);
  if (!loggable(3, tag, 2) || loggable(3, tag, 6)) failed = 1;
  set_minimum(previous);
  if (failed) return 6;
  puts("log-abi:PASS length-limited tags, property priorities, defaults and minimum priority");
  return 0;
}

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "echo";
  if (strcmp(mode, "log-abi") == 0 && argc > 2) return log_abi(argv[2]);
  if (strcmp(mode, "log-write") == 0 && argc > 4) {
    int (*write_log)(int, const char*, const char*) = dlsym(RTLD_DEFAULT, "__android_log_write");
    if (!write_log) return 5;
    write_log(atoi(argv[3]), argv[2], argv[4]);
    return 0;
  }
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
  if (strcmp(mode, "property-abi") == 0 && argc > 2) return property_abi(argv[2]);
  if (strncmp(mode, "property-wait", 13) == 0 && argc > 2) return property_wait(mode, argv[2]);
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
