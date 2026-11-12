// Public bionic key reuse contracts; shared with native ELF fixtures.
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

typedef int (*key_contract_create_fn)(int32_t*, void (*)(void*));
typedef int (*key_contract_delete_fn)(int32_t);
typedef void* (*key_contract_get_fn)(int32_t);
typedef int (*key_contract_set_fn)(int32_t, const void*);
struct key_contract_api {
  key_contract_create_fn create;
  key_contract_delete_fn destroy;
  key_contract_get_fn get;
  key_contract_set_fn set;
};
static struct key_contract_api get_key_contract_api(void) {
  struct key_contract_api api;
  api.create = (key_contract_create_fn)dlsym(RTLD_DEFAULT, "pthread_key_create");
  api.destroy = (key_contract_delete_fn)dlsym(RTLD_DEFAULT, "pthread_key_delete");
  api.get = (key_contract_get_fn)dlsym(RTLD_DEFAULT, "pthread_getspecific");
  api.set = (key_contract_set_fn)dlsym(RTLD_DEFAULT, "pthread_setspecific");
  return api;
}
struct key_contract_payload { int kind, calls, wrong_calls; };
static void key_contract_old_destructor(void* value) {
  ++((struct key_contract_payload*)value)->calls;
}
static void key_contract_new_destructor(void* value) {
  struct key_contract_payload* payload = (struct key_contract_payload*)value;
  if (payload->kind == 2) ++payload->calls;
  else ++payload->wrong_calls;
}
struct key_contract_work {
  struct key_contract_api api;
  int32_t old_key, new_key;
  pthread_mutex_t* mutex;
  pthread_cond_t* condition;
  int* ready;
  int* released;
  int assign_new, saw_null, result;
  struct key_contract_payload old_value, new_value;
};
static void* key_contract_worker(void* context) {
  struct key_contract_work* work = (struct key_contract_work*)context;
  work->result = work->api.set(work->old_key, &work->old_value);
  pthread_mutex_lock(work->mutex);
  ++*work->ready;
  pthread_cond_broadcast(work->condition);
  while (!*work->released) pthread_cond_wait(work->condition, work->mutex);
  pthread_mutex_unlock(work->mutex);
  if (work->assign_new) {
    work->saw_null = work->api.get(work->new_key) == NULL;
    work->result |= work->api.set(work->new_key, &work->new_value);
  }
  return NULL;
}

static void test_pthread_key_contracts(void) {
  struct key_contract_api api = get_key_contract_api();
  int available = api.create && api.destroy && api.get && api.set;
  check("pthread keys: public exports", available);
  if (!available) return;
  int32_t old_key = -1, new_key = -1;
  struct key_contract_payload value = {1, 0, 0};
  check("pthread keys: create old key", api.create(&old_key, key_contract_old_destructor) == 0);
  check("pthread keys: new key begins empty", api.get(old_key) == NULL);
  check("pthread keys: set/get preserves value", api.set(old_key, &value) == 0 && api.get(old_key) == &value);
  check("pthread keys: delete does not run destructor", api.destroy(old_key) == 0 && value.calls == 0);
  check("pthread keys: deleted key reads empty", api.get(old_key) == NULL);
  check("pthread keys: repeated deletion is EINVAL", api.destroy(old_key) == EINVAL);
  check("pthread keys: setting deleted key is EINVAL", api.set(old_key, &value) == EINVAL);
  check("pthread keys: replacement reuses shim slot", api.create(&new_key, key_contract_new_destructor) == 0 && new_key == old_key);
  check("pthread keys: replacement starts empty", api.get(new_key) == NULL);
  api.set(new_key, NULL);
  api.destroy(new_key);
  check("pthread keys: out-of-range operations rejected",
        api.destroy(-1) == EINVAL && api.set(-1, &value) == EINVAL && api.get(-1) == NULL &&
        api.destroy(INT32_MAX) == EINVAL && api.set(INT32_MAX, &value) == EINVAL && api.get(INT32_MAX) == NULL);

  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
  int ready = 0, released = 0;
  struct key_contract_work work[2];
  memset(work, 0, sizeof(work));
  check("pthread keys: create cross-thread old key", api.create(&old_key, key_contract_old_destructor) == 0);
  pthread_t workers[2];
  int started = 0;
  for (int i = 0; i < 2; ++i) {
    work[i].api = api;
    work[i].old_key = old_key;
    work[i].mutex = &mutex;
    work[i].condition = &condition;
    work[i].ready = &ready;
    work[i].released = &released;
    work[i].assign_new = i == 0;
    work[i].old_value.kind = 1;
    work[i].new_value.kind = 2;
    if (pthread_create(&workers[i], NULL, key_contract_worker, &work[i]) != 0) break;
    ++started;
  }
  check("pthread keys: start retaining threads", started == 2);
  pthread_mutex_lock(&mutex);
  while (ready < started) pthread_cond_wait(&condition, &mutex);
  check("pthread keys: delete across live threads", api.destroy(old_key) == 0);
  check("pthread keys: recreate across live threads", api.create(&new_key, key_contract_new_destructor) == 0 && new_key == old_key);
  for (int i = 0; i < started; ++i) work[i].new_key = new_key;
  released = 1;
  pthread_cond_broadcast(&condition);
  pthread_mutex_unlock(&mutex);
  for (int i = 0; i < started; ++i) pthread_join(workers[i], NULL);
  if (started == 2) {
    check("pthread keys: another thread sees empty replacement", work[0].saw_null);
    check("pthread keys: new assignment gets its destructor", work[0].result == 0 && work[0].new_value.calls == 1);
    check("pthread keys: obsolete values never reach replacement destructor",
          work[0].old_value.calls == 0 && work[0].old_value.wrong_calls == 0 &&
          work[1].result == 0 && work[1].old_value.calls == 0 && work[1].old_value.wrong_calls == 0);
  }
  api.destroy(new_key);
  pthread_cond_destroy(&condition);
  pthread_mutex_destroy(&mutex);
}
