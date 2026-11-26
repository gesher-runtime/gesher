/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0 */

void gesher_n06_event_record(int event);

#ifdef GESHER_N06_DEPENDENCY
#define INIT_EVENT 101
#define FINI_EVENT 102

static int value;

int gesher_n06_dependency_advance(void) { return ++value; }
#else
#define INIT_EVENT 201
#define FINI_EVENT 202

int gesher_n06_dependency_advance(void);
int gesher_n06_root_advance(void) { return gesher_n06_dependency_advance(); }
#endif

__attribute__((constructor)) static void initialize(void) { gesher_n06_event_record(INIT_EVENT); }
__attribute__((destructor)) static void finalize(void) { gesher_n06_event_record(FINI_EVENT); }
