/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
#ifndef FIXTURE_VALUE
#define FIXTURE_VALUE 100
#endif
#ifdef EVENTS_PROVIDER
void registry_ctor_event(void) {}
#else
extern void registry_ctor_event(void);
static int constructors;
__attribute__((constructor)) static void initialize(void) {
  ++constructors;
  registry_ctor_event();
}
int namespace_value(void) { return FIXTURE_VALUE + constructors; }
#endif
