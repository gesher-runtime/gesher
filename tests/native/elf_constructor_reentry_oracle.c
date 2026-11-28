/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 * Use the reentry graph from build-load-transaction-graph.sh with Android libdl.
 * Link with -Wl,--export-dynamic so its callbacks resolve from the graph. */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static const char* directory;
static void* file;
static void* named;
static unsigned events[29];
static int file_during_open;
static int named_during_open;

static void* open_file(const char* name) {
  char path[4096];
  int length = snprintf(path, sizeof(path), "%s/%s", directory, name);
  if (length < 0 || (size_t)length >= sizeof(path)) return NULL;
  return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

static int call(void* handle, const char* name) {
  if (handle == NULL) return -1;
  int (*function)(void) = (int (*)(void))dlsym(handle, name);
  return function != NULL ? function() : -2;
}

void tx_event(int role) {
  if (role >= 25 && role <= 28) ++events[role];
}

void tx_reenter_siblings(void) {
  file = open_file("tx_reentry_file.so");
  file_during_open = call(file, "tx_sibling_init_count");
  named = dlopen("tx_reentry_memory.so", RTLD_NOW | RTLD_LOCAL);
  named_during_open = call(named, "tx_sibling_init_count");
}

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <complete-graph-directory>\n", argv[0]);
    return 2;
  }
  directory = argv[1];
  void* root = open_file("tx_reentry_graph.so");
  if (root == NULL) {
    const char* error = dlerror();
    fprintf(stderr, "N01 root open failed: %s\n", error != NULL ? error : "(no detail)");
    return 1;
  }
  const int root_result = call(root, "tx_reentry_graph");
  const int file_after_open = call(file, "tx_sibling_init_count");
  const int named_after_open = call(named, "tx_sibling_init_count");
  printf("N01 sibling counts within dlopen: path=%d soname=%d\n",
         file_during_open, named_during_open);
  printf("N01 after outer dlopen: root=%d path=%d soname=%d events=%u,%u,%u,%u\n",
         root_result, file_after_open, named_after_open, events[25], events[26],
         events[27], events[28]);
  if (file != NULL) dlclose(file);
  if (named != NULL) dlclose(named);
  dlclose(root);
  return root_result != 28 || file_during_open != 1 || named_during_open != 1 ||
      file_after_open != 1 || named_after_open != 1 || events[25] != 1 ||
      events[26] != 1 || events[27] != 1 || events[28] != 1;
}
