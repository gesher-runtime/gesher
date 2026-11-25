// Environment strings outlive later changes to the environment, as with
// bionic, from an x86-64 Android library through the bionic shim. bionic's
// putenv keeps the caller's string and its setenv/unsetenv never free a value
// they replace or remove, so what getenv returned, and what environ pointed
// at, stays readable and keeps its bytes. mozglue relies on it: its own getenv
// reads environ, and it caches getenv("MOZ_ANDROID_LIBDIR") in a static while
// GeckoLoader putenv()s the same assignment again. Both ways are checked, as
// is what getenv and environ report after each change.
// Built by native-libs.yml as environment_lifetime_test.so; elf_load_test
// calls gesher_selftest().
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

static void check(const char* what, int ok) {
  printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) ++failures;
}

static int equals(const char* value, const char* expected) {
  return value != NULL && strcmp(value, expected) == 0;
}

// The value of `name` as environ has it (mozglue's getenv walks environ).
static const char* environ_value(const char* name) {
  const size_t length = strlen(name);
  for (char** entry = environ; entry != NULL && *entry != NULL; ++entry) {
    if (strncmp(*entry, name, length) == 0 && (*entry)[length] == '=') return *entry + length + 1;
  }
  return NULL;
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
  printf("environment_lifetime: getenv and environ after setenv/putenv/unsetenv\n");
  static const char kName[] = "GESHER_ENV_LIFETIME_TEST";
  static const char kFirst[] = "/data/app/~~first/org.example-1/lib/x86_64";
  static const char kThird[] = "third";

  check("setenv of a new variable", setenv(kName, kFirst, 1) == 0);
  const char* first = getenv(kName);
  check("getenv returns the value set", equals(first, kFirst));
  const char* first_entry = environ_value(kName);
  check("environ has the value set", equals(first_entry, kFirst));
  check("getenv again returns the same value", equals(getenv(kName), kFirst));

  // The same assignment again, as GeckoLoader does before each load.
  char* assignment = malloc(sizeof(kName) + sizeof(kFirst));
  if (assignment == NULL) return ++failures;
  snprintf(assignment, sizeof(kName) + sizeof(kFirst), "%s=%s", kName, kFirst);
  check("putenv of the same assignment", putenv(assignment) == 0);
  churn(sizeof(kName) + sizeof(kFirst));
  check("the first result keeps its bytes after putenv", equals(first, kFirst));
  check("the first environ entry keeps its bytes after putenv", equals(first_entry, kFirst));
  check("getenv after putenv", equals(getenv(kName), kFirst));
  char* assigned_value = strchr(assignment, '=') + 1;
  check("putenv retains the exact caller value pointer", getenv(kName) == assigned_value);
  check("environ retains the exact caller value pointer", environ_value(kName) == assigned_value);
  assigned_value[0] = '!';
  check("getenv observes caller mutation", getenv(kName) && getenv(kName)[0] == '!');
  check("environ observes caller mutation", environ_value(kName) && environ_value(kName)[0] == '!');
  assigned_value[0] = '/';

  char second[sizeof(kName) + 16];
  snprintf(second, sizeof(second), "%s=second", kName);
  check("putenv of another value", putenv(strdup(second)) == 0);
  check("getenv after putenv of another value", equals(getenv(kName), "second"));
  check("environ after putenv of another value", equals(environ_value(kName), "second"));
  churn(sizeof(kName) + sizeof(kFirst));
  check("the first environ entry keeps its bytes after a new value",
        equals(first_entry, kFirst));
  const char* second_value = getenv(kName);

  check("setenv overwrite=0 keeps the value", setenv(kName, kThird, 0) == 0);
  check("getenv after setenv overwrite=0", equals(getenv(kName), "second"));
  check("setenv overwrite=1", setenv(kName, kThird, 1) == 0);
  check("getenv after setenv overwrite=1", equals(getenv(kName), kThird));
  churn(sizeof(kName) + 8);
  check("the putenv result keeps its bytes after setenv", equals(second_value, "second"));

  check("unsetenv", unsetenv(kName) == 0);
  check("getenv after unsetenv is null", getenv(kName) == NULL);
  check("environ after unsetenv has no entry", environ_value(kName) == NULL);
  churn(sizeof(kName) + sizeof(kThird));
  check("the first result keeps its bytes after unsetenv", equals(first, kFirst));
  check("unsetenv of an absent variable", unsetenv(kName) == 0);
  check("getenv of a never-set variable is null", getenv("GESHER_ENV_NEVER_SET_TEST") == NULL);

  check("setenv after unsetenv", setenv(kName, kFirst, 1) == 0);
  check("getenv after setting the first value again", equals(getenv(kName), kFirst));
  check("setenv of a name with '=' fails", setenv("GESHER=ENV", "x", 1) == -1);
  unsetenv(kName);

  char mutable[] = "GESHER_ENV_MUTABLE_A=first";
  check("putenv accepts automatic caller storage", putenv(mutable) == 0);
  char* mutable_value = strchr(mutable, '=') + 1;
  memcpy(mutable_value, "after", 6);
  check("putenv value mutation reaches getenv and environ", equals(getenv("GESHER_ENV_MUTABLE_A"), "after") && equals(environ_value("GESHER_ENV_MUTABLE_A"), "after"));
  mutable[sizeof("GESHER_ENV_MUTABLE_") - 1] = 'B';
  check("putenv name mutation changes the actual table entry", getenv("GESHER_ENV_MUTABLE_A") == NULL && equals(getenv("GESHER_ENV_MUTABLE_B"), "after") && equals(environ_value("GESHER_ENV_MUTABLE_B"), "after"));
  check("equal setenv detaches from caller storage", setenv("GESHER_ENV_MUTABLE_B", mutable_value, 1) == 0);
  memcpy(mutable_value, "other", 6);
  check("setenv copy stays independent after caller mutation", equals(getenv("GESHER_ENV_MUTABLE_B"), "after"));
  check("caller may remove the copied value", unsetenv("GESHER_ENV_MUTABLE_B") == 0);
  char empty[] = "GESHER_ENV_MUTABLE_B=";
  check("putenv preserves a present empty value", putenv(empty) == 0 && equals(getenv("GESHER_ENV_MUTABLE_B"), "") && equals(environ_value("GESHER_ENV_MUTABLE_B"), ""));
  char remove[] = "GESHER_ENV_MUTABLE_B";
  check("putenv without equals removes the name", putenv(remove) == 0 && getenv(remove) == NULL && environ_value(remove) == NULL);
  free(assignment);  // It is no longer in the environment; UCRT never owns it.

  if (failures == 0) printf("environment_lifetime: all passed\n");
  return failures;
}
