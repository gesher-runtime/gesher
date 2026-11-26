/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#ifndef GESHER_ELF_CALL
#define GESHER_ELF_CALL
#define GESHER_ELF_SYMBOL(handle, name) dlsym(handle, name)
static int set_target(uint32_t version) {
  void (*set_version)(uint32_t) = (void (*)(uint32_t))
      dlsym(RTLD_DEFAULT, "android_set_application_target_sdk_version");
  if (set_version == NULL) {
    set_version = (void (*)(uint32_t))
        dlsym(RTLD_DEFAULT, "__loader_android_set_application_target_sdk_version");
  }
  if (set_version == NULL) {
    fprintf(stderr, "set target unavailable: %s\n", dlerror());
    return 0;
  }
  set_version(version);
  return 1;
}
#endif

static int checks;
static int failures;
static void check(int passed, const char* label) {
  ++checks;
  if (!passed) ++failures;
  printf("%s %s\n", passed ? "PASS" : "FAIL", label);
}

static int run(uint32_t target, int legacy_alias) {
  static const char* original = "./n03_without_soname.so";
  char alias[96];
  snprintf(alias, sizeof(alias), "n03_removed_alias_%ld_%u.so", (long)getpid(), target);
  char path[100];
  snprintf(path, sizeof(path), "./%s", alias);
  printf("target=%u\n", target);
  if (!set_target(target)) return 0;
  if (rename(original, path) != 0) {
    perror("rename to the temporary basename");
    return 0;
  }
  void* library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  check(library != NULL, "the explicit file loads without a SONAME");
  if (rename(path, original) != 0) {
    perror("restore the source filename");
    return 0;
  }
  if (library == NULL) return 0;
  int (GESHER_ELF_CALL *count)(void) =
      (int (GESHER_ELF_CALL *)(void))GESHER_ELF_SYMBOL(library, "n03_constructor_calls");
  check(count != NULL && count() == 1, "constructor runs once");
  void* file = dlopen(original, RTLD_NOW | RTLD_NOLOAD);
  check(file == library, "NOLOAD finds the same file after it was renamed");
  if (file != NULL) check(dlclose(file) == 0, "release the file-identity reference");
  void* bare = dlopen(alias, RTLD_NOW | RTLD_NOLOAD);
  check(legacy_alias ? bare == library : bare == NULL, "bare removed alias follows the target SDK rule");
  if (bare != NULL) check(dlclose(bare) == 0, "release the basename reference");
  check(count != NULL && count() == 1, "alias lookup did not run the constructor again");
  check(dlclose(library) == 0, "release the last reference");
  void* retired = dlopen(original, RTLD_NOW | RTLD_NOLOAD);
  check(retired == NULL, "the last close retires the file identity");
  if (retired != NULL) dlclose(retired);
  return 1;
}

int main(int argc, char** argv) {
  if (argc != 2 || chdir(argv[1]) != 0) return 2;
  if (!run(22, 1) || !run(23, 0) || !run(0, 0)) return 2;
  printf("ELF SONAME compatibility: %d checks, %d failures\n", checks, failures);
  return failures != 0;
}
