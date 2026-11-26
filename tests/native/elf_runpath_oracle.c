/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 * Build with the Android NDK and -Wl,--export-dynamic. The fixture graph comes
 * from build-load-transaction-graph.sh. Run again with its runpath/environment
 * directory on LD_LIBRARY_PATH to check that it takes priority. */
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int constructors[36];

void tx_event(int role) {
  if (role > 0 && role < (int)(sizeof(constructors) / sizeof(constructors[0]))) {
    ++constructors[role];
  }
}

static bool missing_dependency(const char* path, const char* dependency) {
  if (dlopen(path, RTLD_NOW) != NULL) return false;
  const char* error = dlerror();
  return error && strstr(error, dependency);
}

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3 || chdir(argv[1]) != 0) return 2;
  const bool environment = argc == 3 && strcmp(argv[2], "--environment") == 0;
  if (argc == 3 && !environment) return 2;
  if (!environment) {
    const bool rpath_ignored = missing_dependency("./runpath/root/tx_runpath_legacy.so",
                                                 "tx_runpath_mid.so");
    const bool not_inherited = missing_dependency("./noinherit/root/tx_runpath_root.so",
                                                  "tx_runpath_leaf.so");
    printf("Android DT_RPATH ignored=%d DT_RUNPATH not inherited=%d\n", rpath_ignored, not_inherited);
    if (!rpath_ignored || !not_inherited || constructors[31] || constructors[32] || constructors[33]) {
      return 1;
    }
  }
  void* root = dlopen("./runpath/root/tx_runpath_root.so", RTLD_NOW);
  if (!root) {
    puts(dlerror());
    return 1;
  }
  int (*value)(void) = (int (*)(void))dlsym(root, "tx_runpath_root");
  const int answer = value ? value() : -1;
  printf("Android DT_RUNPATH environment=%d value=%d constructors=%d,%d,%d,%d,%d\n",
         environment, answer, constructors[31], constructors[32], constructors[33],
         constructors[34], constructors[35]);
  return !(answer == (environment ? 36 : 33) &&
           constructors[31] == !environment && constructors[32] == !environment &&
           constructors[33] == 1 && constructors[34] == 0 && constructors[35] == environment);
}
