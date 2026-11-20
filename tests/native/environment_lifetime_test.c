// getenv's result outlives later changes to the environment, as with bionic,
// from an x86-64 Android library through the bionic shim. bionic's putenv
// keeps the caller's string and its setenv/unsetenv never free a value they
// replace or remove, so a pointer getenv returned stays readable and keeps
// its bytes; mozglue relies on that (it caches getenv("MOZ_ANDROID_LIBDIR")
// in a static while GeckoLoader putenv()s the same assignment again). The
// checks also cover what getenv reports after each change.
// Built by native-libs.yml as environment_lifetime_test.so; elf_load_test
// calls gesher_selftest().
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(const char* what, int ok) {
  printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) ++failures;
}

static int equals(const char* value, const char* expected) {
  return value != NULL && strcmp(value, expected) == 0;
}

// Reuses freed blocks of the sizes the replaced strings had (the CRT frees a
// replaced value and its next allocations of that size take the bytes).
static void churn(size_t size) {
  enum { kBlocks = 4096 };
  static char* blocks[kBlocks];
  for (int i = 0; i < kBlocks; ++i) {
    blocks[i] = malloc(size);
    if (blocks[i] != NULL) memset(blocks[i], 'X', size);
  }
  for (int i = 0; i < kBlocks; ++i) free(blocks[i]);
}

int gesher_selftest(void) {
  printf("environment_lifetime: getenv results after setenv/putenv/unsetenv\n");
  static const char kName[] = "GESHER_ENV_LIFETIME_TEST";
  static const char kFirst[] = "/data/app/~~first/org.example-1/lib/x86_64";
  static const char kThird[] = "third";

  check("setenv of a new variable", setenv(kName, kFirst, 1) == 0);
  const char* first = getenv(kName);
  check("getenv returns the value set", equals(first, kFirst));
  const char* again = getenv(kName);
  check("getenv again returns the same value", equals(again, kFirst));

  // The same assignment again, as GeckoLoader does before each load.
  char* assignment = malloc(sizeof(kName) + sizeof(kFirst));
  if (assignment == NULL) return ++failures;
  snprintf(assignment, sizeof(kName) + sizeof(kFirst), "%s=%s", kName, kFirst);
  check("putenv of the same assignment", putenv(assignment) == 0);
  churn(sizeof(kName) + sizeof(kFirst));
  check("the first result keeps its bytes after putenv", equals(first, kFirst));
  check("getenv after putenv", equals(getenv(kName), kFirst));

  char second[sizeof(kName) + 16];
  snprintf(second, sizeof(second), "%s=second", kName);
  check("putenv of another value", putenv(strdup(second)) == 0);
  check("getenv after putenv of another value", equals(getenv(kName), "second"));
  const char* second_value = getenv(kName);

  check("setenv overwrite=0 keeps the value", setenv(kName, kThird, 0) == 0);
  check("getenv after setenv overwrite=0", equals(getenv(kName), "second"));
  check("setenv overwrite=1", setenv(kName, kThird, 1) == 0);
  check("getenv after setenv overwrite=1", equals(getenv(kName), kThird));
  churn(sizeof(kName) + 8);
  check("the putenv result keeps its bytes after setenv", equals(second_value, "second"));

  check("unsetenv", unsetenv(kName) == 0);
  check("getenv after unsetenv is null", getenv(kName) == NULL);
  churn(sizeof(kName) + sizeof(kThird));
  check("the first result keeps its bytes after unsetenv", equals(first, kFirst));
  check("getenv of a never-set variable is null", getenv("GESHER_ENV_NEVER_SET_TEST") == NULL);

  check("setenv after unsetenv", setenv(kName, kFirst, 1) == 0);
  check("getenv after setting the first value again", equals(getenv(kName), kFirst));
  unsetenv(kName);

  if (failures == 0) printf("environment_lifetime: all passed\n");
  return failures;
}
