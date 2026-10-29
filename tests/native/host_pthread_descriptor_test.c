/* Discovery descriptor ABI and image ownership; CI only. */
#include <pthread.h>
#include <windows.h>
#include <stdio.h>
typedef struct owner_v1 {
  unsigned size, version;
  pthread_t (__cdecl *self)(void);
  int (__cdecl *equal)(pthread_t, pthread_t);
  int (__cdecl *create)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
  int (__cdecl *join)(pthread_t, void **);
  int (__cdecl *tryjoin)(pthread_t, void **);
  int (__cdecl *detach)(pthread_t);
  int (__cdecl *cancel)(pthread_t);
  int (__cdecl *key_create)(pthread_key_t *, void (*)(void *));
  int (__cdecl *key_delete)(pthread_key_t);
  void *(__cdecl *getspecific)(pthread_key_t);
  int (__cdecl *setspecific)(pthread_key_t, const void *);
  struct _pthread_cleanup **(__cdecl *getclean)(void);
} owner_v1;
extern const owner_v1 * __cdecl gesher_pthread_owner_descriptor_v1(void);
typedef const owner_v1 * (__cdecl *descriptor_fn)(void);
static const owner_v1 *active;
static pthread_key_t destructor_key;
static volatile LONG destroyed;
static void destructor(void *value) { if (value == &destructor_key) InterlockedIncrement(&destroyed); }
static void *worker(void *unused) {
  (void)unused;
  return active->setspecific(destructor_key, &destructor_key) ? (void *)1 : NULL;
}
static int belongs(const void *address, HMODULE module) {
  HMODULE actual = NULL;
  return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)address, &actual) && actual == module;
}
static int check(const owner_v1 *p, HMODULE module, void (***dest)(void *)) {
  pthread_key_t keys[2048];
  void (**table)(void *);
  unsigned i;
  if (!p || p->size != sizeof(*p) || p->version != 1 ||
      !p->self || !p->equal || !p->create || !p->join || !p->tryjoin ||
      !p->detach || !p->cancel || !p->key_create || !p->key_delete ||
      !p->getspecific || !p->setspecific || !p->getclean) return 1;
  if (!belongs(p, module) || !belongs((const void *)p->self, module) ||
      !belongs((const void *)p->equal, module) || !belongs((const void *)p->create, module) ||
      !belongs((const void *)p->join, module) || !belongs((const void *)p->tryjoin, module) ||
      !belongs((const void *)p->detach, module) || !belongs((const void *)p->cancel, module) ||
      !belongs((const void *)p->key_create, module) || !belongs((const void *)p->key_delete, module) ||
      !belongs((const void *)p->getspecific, module) || !belongs((const void *)p->setspecific, module) ||
      !belongs((const void *)p->getclean, module)) return 2;
  pthread_t id = p->self();
  if (!id || !p->equal(id, id) || !p->getclean()) return 3;
  if (!dest || *dest || p->key_create(&keys[0], NULL)) return 4;
  table = *dest;
  if (!table || p->setspecific(keys[0], &keys[0]) || p->getspecific(keys[0]) != &keys[0]) return 5;
  for (i = 1; i < 2048; ++i)
    if (p->key_create(&keys[i], NULL) || *dest != table) return 6;
  for (i = 0; i < 2048; ++i)
    if (p->key_delete(keys[i])) return 7;
  pthread_t thread;
  void *result = (void *)1;
  active = p;
  destroyed = 0;
  if (p->key_create(&destructor_key, destructor) || destructor_key != keys[0] ||
      p->create(&thread, NULL, worker, NULL) || p->join(thread, &result) || result ||
      destroyed != 1 || p->key_delete(destructor_key)) return 8;
  return 0;
}
int main(void) {
  if (!belongs((const void *)gesher_pthread_owner_descriptor_v1, GetModuleHandleW(NULL)) ||
      check(gesher_pthread_owner_descriptor_v1(), GetModuleHandleW(NULL), &_pthread_key_dest)) return 1;
  HMODULE dll = LoadLibraryW(L"libwinpthread-1.dll");
  if (!dll) return 2;
  descriptor_fn get = (descriptor_fn)GetProcAddress(dll, "gesher_pthread_owner_descriptor_v1");
  void (***dest)(void *) = (void (***)(void *))GetProcAddress(dll, "_pthread_key_dest");
  if (!get || !belongs((const void *)get, dll) || !belongs(dest, dll) || check(get(), dll, dest)) return 3;
  if (!FreeLibrary(dll) || GetModuleHandleW(L"libwinpthread-1.dll")) return 4;
  puts("PASS discovery descriptor and stable key growth in both images; DLL unload");
  return 0;
}
