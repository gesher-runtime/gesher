/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0 */

#include <dlfcn.h>
#include <stdio.h>

#ifndef GESHER_N06_CALL
#define GESHER_N06_CALL
#define GESHER_N06_SYMBOL(handle, name) dlsym(handle, name)
#endif

typedef int (GESHER_N06_CALL *ReadCount)(void);
typedef int (GESHER_N06_CALL *ReadEvent)(int);

static int checks;
static int failures;

static void check(int passes, const char* label) {
  ++checks;
  if (!passes) ++failures;
  printf("%s %s\n", passes ? "PASS" : "FAIL", label);
}

static void* open_library(const char* directory, const char* name, int flags) {
  char path[4096];
  int length = snprintf(path, sizeof(path), "%s/%s", directory, name);
  if (length < 0 || (size_t)length >= sizeof(path)) return NULL;
  return dlopen(path, RTLD_NOW | flags);
}

static void check_unloaded(const char* directory, const char* name, const char* label) {
  void* handle = open_library(directory, name, RTLD_NOLOAD);
  check(handle == NULL, label);
  if (handle != NULL) dlclose(handle);
}

static void check_events(ReadCount count, ReadEvent event, const int* expected,
                         int expected_count, const char* label) {
  int actual_count = count();
  int matches = actual_count == expected_count;
  printf("  %s:", label);
  for (int i = 0; i < actual_count; ++i) {
    int actual = event(i);
    printf(" %d", actual);
    if (i < expected_count && actual != expected[i]) matches = 0;
  }
  puts("");
  check(matches, label);
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const char* directory = argv[1];
  const char* root_name = "libn06_root.so";
  const char* dep_name = "libn06_dep.so";
  void* trace = open_library(directory, "libn06_trace.so", RTLD_LOCAL);
  if (trace == NULL) {
    fprintf(stderr, "trace library: %s\n", dlerror());
    return 2;
  }
  ReadCount count = (ReadCount)GESHER_N06_SYMBOL(trace, "gesher_n06_event_count");
  ReadEvent event = (ReadEvent)GESHER_N06_SYMBOL(trace, "gesher_n06_event_at");
  if (count == NULL || event == NULL) return 2;
  check(count() == 0, "trace owner starts independently");

  void* first = open_library(directory, root_name, RTLD_LOCAL);
  void* second = open_library(directory, root_name, RTLD_LOCAL);
  if (first == NULL || second == NULL) return 2;
  ReadCount advance = (ReadCount)GESHER_N06_SYMBOL(second, "gesher_n06_root_advance");
  if (advance == NULL) return 2;
  const int started[] = {101, 201};
  check_events(count, event, started, 2, "two opens initialize dependency then root once");
  check(advance() == 1, "initial dependency state");
  check(dlclose(first) == 0, "close first root reference");
  check_events(count, event, started, 2, "second root reference keeps both libraries live");
  check(advance() == 2, "second root reference still accesses dependency state");
  check(dlclose(second) == 0, "close last root reference");

  const int retired[] = {101, 201, 202, 102};
  check_events(count, event, retired, 4, "last close finalizes root before dependency");
  check_unloaded(directory, root_name, "RTLD_NOLOAD finds no root after last close");
  check_unloaded(directory, dep_name, "RTLD_NOLOAD finds no dependency after last close");

  void* dependency = open_library(directory, dep_name, RTLD_LOCAL);
  void* root = open_library(directory, root_name, RTLD_LOCAL);
  if (dependency == NULL || root == NULL) return 2;
  advance = (ReadCount)GESHER_N06_SYMBOL(root, "gesher_n06_root_advance");
  if (advance == NULL) return 2;
  const int reloaded[] = {101, 201, 202, 102, 101, 201};
  check_events(count, event, reloaded, 6, "reopen initializes both libraries again");
  check(advance() == 1, "reopen resets dependency data");
  check(dlclose(root) == 0, "close root while dependency has its own reference");
  const int root_retired[] = {101, 201, 202, 102, 101, 201, 202};
  check_events(count, event, root_retired, 7, "root finalizes while independent dependency remains");
  check_unloaded(directory, root_name, "root disappears while dependency remains");
  advance = (ReadCount)GESHER_N06_SYMBOL(dependency, "gesher_n06_dependency_advance");
  check(advance != NULL && advance() == 2, "independent dependency remains callable");
  check(dlclose(dependency) == 0, "close independent dependency reference");
  const int both_retired[] = {101, 201, 202, 102, 101, 201, 202, 102};
  check_events(count, event, both_retired, 8, "dependency finalizes after its own last close");
  check_unloaded(directory, dep_name, "dependency disappears after its own last close");
  check(dlclose(trace) == 0, "close independent trace reference");
  printf("ELF ordinary close lifecycle: %d checks, %d failures\n", checks, failures);
  return failures != 0;
}
