/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Original, ordinary compiler-built ELF objects for namespace, file identity,
 * search order and caller-scope tests. No Android or system binary is modified.
 * The two link-only roles provide definitions to the static linker; the test
 * runtime supplies their explicitly owned callback/libdl providers instead.
 */
#ifndef NS_ROLE
#error NS_ROLE is required
#endif
#ifndef NS_VALUE
#define NS_VALUE 0
#endif
#ifndef NS_EXPORT
#define NS_EXPORT ns_value
#endif
#ifndef NS_IMPORT
#define NS_IMPORT ns_leaf_value
#endif
#ifndef NS_EVENT_ID
#define NS_EVENT_ID 0
#endif
#ifndef NS_VISIBILITY
#define NS_VISIBILITY 0
#endif

#if NS_ROLE != 0 && NS_ROLE != 6
/* Observable storage keeps a compiler from folding constructor-only state
 * into the getter and removing the constructor from an otherwise pure DSO. */
static volatile int private_constructor_count;
#if NS_EVENT_ID
extern void ns_event(int id);
#endif
__attribute__((constructor)) static void initialized(void) {
  ++private_constructor_count;
#if NS_EVENT_ID
  ns_event(NS_EVENT_ID);
#endif
}
int ns_constructor_count(void) { return private_constructor_count; }
#endif

#if NS_ROLE == 0
/* Link-time definition only. The host records actual constructor events. */
void ns_event(int id) { (void)id; }
#elif NS_ROLE == 1
int NS_EXPORT(void) { return NS_VALUE; }
int ns_storage = NS_VALUE;
int ns_provider_value(void) { return NS_VALUE; }
#elif NS_ROLE == 2
extern int NS_IMPORT(void);
int NS_EXPORT(void) { return NS_IMPORT(); }
#elif NS_ROLE == 3 || NS_ROLE == 5
#if NS_VISIBILITY == 1
#define NS_COLLISION_VISIBILITY __attribute__((visibility("protected")))
#elif NS_VISIBILITY == 2
#define NS_COLLISION_VISIBILITY __attribute__((visibility("hidden")))
#else
#define NS_COLLISION_VISIBILITY
#endif
NS_COLLISION_VISIBILITY __attribute__((noinline))
int ns_collision(void) { return NS_VALUE; }
int ns_provider_value(void) { return NS_VALUE; }
/* A real initialized function pointer makes data relocation binding visible.
 * Volatile prevents the compiler from replacing the observation with a direct
 * call. Its storage is private, so another object's pointer cannot interpose. */
static int (*volatile private_collision_slot)(void) = ns_collision;
int ns_bind_self(void) { return private_collision_slot(); }
void* ns_collision_address(void) { return (void*)&ns_collision; }

#if NS_ROLE == 5
extern void* dlsym(void* handle, const char* name);
extern void* dlopen(const char* path, int flags);

/* Keep the actual public-libdl call site inside this DSO. The builder also
 * disables sibling calls; the opaque result use prevents a later tail-call
 * optimization from turning a host return PC into the apparent guest caller. */
__attribute__((noinline)) void* ns_default(const char* name) {
  void* result = dlsym((void*)0, name);
  __asm__ volatile("" : "+r"(result));
  return result;
}
__attribute__((noinline)) void* ns_next(const char* name) {
  void* result = dlsym((void*)(__INTPTR_TYPE__)-1, name);
  __asm__ volatile("" : "+r"(result));
  return result;
}
__attribute__((noinline)) void* ns_handle(void* handle, const char* name) {
  void* result = dlsym(handle, name);
  __asm__ volatile("" : "+r"(result));
  return result;
}
__attribute__((noinline)) void* ns_open(const char* path, int flags) {
  void* result = dlopen(path, flags);
  __asm__ volatile("" : "+r"(result));
  return result;
}
#endif
#elif NS_ROLE == 4
/* This object has a real DT_NEEDED edge but no collision definition. It makes
 * breadth-first lookup distinguish a direct sibling from a deeper dependency. */
int ns_left(void) { return NS_VALUE; }
#elif NS_ROLE == 6
/* Link-time declarations only; never use this DSO as a runtime libdl. */
void* dlsym(void* handle, const char* name) {
  (void)handle;
  (void)name;
  return (void*)0;
}
void* dlopen(const char* path, int flags) {
  (void)path;
  (void)flags;
  return (void*)0;
}
#else
#error Unknown NS_ROLE
#endif
