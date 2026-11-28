/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0 */

#include <dlfcn.h>
#include <stdio.h>

static int checks;
static int failures;

static void* open_fixture(const char* directory, const char* file, int flags) {
  char path[4096];
  int length = snprintf(path, sizeof(path), "%s/%s", directory, file);
  if (length < 0 || (size_t)length >= sizeof(path)) return NULL;
  void* handle = dlopen(path, RTLD_NOW | flags);
  if (!handle) fprintf(stderr, "%s: %s\n", path, dlerror());
  return handle;
}

static void expect_value(void* handle, const char* symbol, int expected, const char* label) {
  int (*function)(void) = handle ? (int (*)(void))dlsym(handle, symbol) : NULL;
  int value = function ? function() : -1;
  printf("%s: value=%d expected=%d\n", label, value, expected);
  ++checks;
  failures += value != expected;
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const char* directory = argv[1];
  void* ordinary = open_fixture(directory, "scope_ordinary.so", RTLD_GLOBAL);
  void* control = open_fixture(directory, "scope_control.so", RTLD_LOCAL);
  expect_value(ordinary, "gesher_scope_resident", 11, "explicit ordinary handle");
  expect_value(control, "gesher_scope_result", 33, "RTLD_GLOBAL alone leaves the relocation local");

  void* global = open_fixture(directory, "scope_global.so", RTLD_LOCAL);
  void* resident = open_fixture(directory, "scope_resident.so", RTLD_LOCAL);
  expect_value(global, "gesher_scope_resident", 22, "explicit DF_1_GLOBAL handle");
  expect_value(resident, "gesher_scope_result", 22, "resident global preempts the relocation");
  expect_value(resident, "gesher_scope_resident", 34, "explicit consumer handle finds its own export");
  expect_value(control, "gesher_scope_result", 33, "earlier relocation retains its original binding");

  void* pending = open_fixture(directory, "scope_pending.so", RTLD_LOCAL);
  expect_value(pending, "gesher_scope_result", 52, "pending global group prefers direct dependency");
  expect_value(pending, "gesher_scope_pending", 31, "pending consumer still owns its export");

  void* later = open_fixture(directory, "scope_later.so", RTLD_LOCAL);
  expect_value(later, "gesher_scope_result", 52, "published group preserves dependency order");
  printf("ELF relocation global scope: %d checks, %d failures\n", checks, failures);
  return failures != 0;
}
