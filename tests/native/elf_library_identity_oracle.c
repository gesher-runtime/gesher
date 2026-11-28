/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 * Use build-load-transaction-graph.sh and link this executable against libdl
 * with -Wl,--export-dynamic so the fixtures can record constructor calls. */
#include <android/dlext.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
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
  const int no_load_flags = RTLD_NOW | RTLD_NOLOAD;
  const int invalid_flags = RTLD_NOW | 0x40000000;
  void* invalid = dlopen(a_path, invalid_flags);
  const char* error = dlerror();
  int invalid_rejected = !invalid && error && strstr(error, "invalid flags to dlopen");
  void* cold = dlopen(a_path, no_load_flags);
  error = dlerror();
  int cold_rejected = !cold && error && strstr(error, "RTLD_NOLOAD");
  void* extended_cold = android_dlopen_ext(b_path, no_load_flags, NULL);
  error = dlerror();
  int extended_rejected = !extended_cold && error && strstr(error, "RTLD_NOLOAD");
  int cold_constructors = first_initializations + second_initializations;
  void* a = dlopen(a_path, RTLD_NOW);
  void* distinct = dlopen(b_path, no_load_flags);
  error = dlerror();
  int distinct_rejected = !distinct && error && strstr(error, "RTLD_NOLOAD");
  void* resident_alias = dlopen(alias, no_load_flags);
  void* resident_name = android_dlopen_ext("tx_identity.so", no_load_flags, NULL);
  int resident_alias_matches = a && resident_alias == a;
  int resident_name_matches = a && resident_name == a;
  printf("N02 NOLOAD: invalid=%d cold=%d extended=%d distinct=%d alias=%d SONAME=%d cold constructors=%d\n",
         invalid_rejected, cold_rejected, extended_rejected, distinct_rejected,
         resident_alias_matches, resident_name_matches, cold_constructors);
  if (resident_alias) dlclose(resident_alias);
  if (resident_name) dlclose(resident_name);
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
  return !(invalid_rejected && cold_rejected && extended_rejected && distinct_rejected &&
           resident_alias_matches && resident_name_matches && cold_constructors == 0 &&
           separate && a_value == 29 && b_value == 30 && dependency == 290 && hardlink == a && bare == a &&
           repeated == a && missing == NULL && first_initializations == 1 && second_initializations == 1);
}
