/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 * Use build-load-transaction-graph.sh and link this executable against libdl
 * with -Wl,--export-dynamic so the fixtures can record constructor calls. */
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>

static int first_initializations, second_initializations;

void tx_event(int role) {
  if (role == 29) ++first_initializations;
  if (role == 30) ++second_initializations;
}

static int value(void* handle, const char* symbol) {
  if (!handle) return -1;
  int (*call)(void) = (int (*)(void))dlsym(handle, symbol);
  return call ? call() : -1;
}

int main(int argc, char** argv) {
  if (argc != 2 || chdir(argv[1]) != 0) return 2;
  const char* a_path = "./identity-a/tx_identity.so";
  const char* b_path = "./identity-b/tx_identity.so";
  char alias[80];
  snprintf(alias, sizeof(alias), "./identity-a/tx_identity_alias-%ld.so", (long)getpid());
  if (link(a_path, alias) != 0) return 2;
  void* a = dlopen(a_path, RTLD_NOW);
  void* b = dlopen(b_path, RTLD_NOW);
  void* hardlink = dlopen(alias, RTLD_NOW);
  void* bare = dlopen("tx_identity.so", RTLD_NOW);
  void* repeated = dlopen(a_path, RTLD_NOW);
  void* missing = dlopen("./incomplete/tx_identity.so", RTLD_NOW);
  int a_value = value(a, "tx_identity"), b_value = value(b, "tx_identity");
  int dependency = value(b, "tx_first_only");
  int separate = a && b && a != b;
  printf("N03 identity: separate=%d exports=%d,%d dependency=%d hardlink=%d bare=%d repeated=%d missing=%d constructors=%d,%d\n",
         separate, a_value, b_value, dependency, a && hardlink == a, a && bare == a,
         a && repeated == a, missing == NULL, first_initializations, second_initializations);
  if (unlink(alias) != 0) return 2;
  return !(separate && a_value == 29 && b_value == 30 && dependency == 290 && hardlink == a && bare == a &&
           repeated == a && missing == NULL && first_initializations == 1 && second_initializations == 1);
}
