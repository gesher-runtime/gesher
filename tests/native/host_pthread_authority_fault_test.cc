// CI-only fault-role client. This target is absent with the default option OFF.
#include <pthread.h>
#include <windows.h>
#include <time.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

#if GESHER_WINPTHREAD_CPU_TEST_API != 1
#error The isolated fault fixture requires the separately patched fault supplier
#endif
extern "C" int gesher_host_cpu_clock_test_exhaust_ids_v1();

static void Check(bool ok, const char* name) {
  if (!ok) { std::fprintf(stderr, "FAIL host-clock-fault %s\n", name); std::abort(); }
}
struct Target {
  std::atomic<bool> ready{false}, stop{false};
  HANDLE oracle = nullptr;
  pthread_t identity = 0;
};
static void* Worker(void* arg) {
  auto& t = *static_cast<Target*>(arg);
  t.identity = pthread_self();
  Check(DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t.oracle,
                         THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, 0), "real oracle duplicate");
  t.ready = true;
  while (!t.stop) Sleep(1);
  return nullptr;
}
static void Await(Target& t) {
  auto deadline = GetTickCount64() + 5000;
  while (!t.ready && GetTickCount64() < deadline) Sleep(1);
  Check(t.ready && WaitForSingleObject(t.oracle, 0) == WAIT_TIMEOUT, "real target held alive");
}
int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  pthread_key_t key = static_cast<pthread_key_t>(123), next = static_cast<pthread_key_t>(456);
  Check(_pthread_key_dest == nullptr, "initial key table null");
  Check(gesher_winpthread_cpu_test_control_v1(4, 1) == 0, "inject reserve failure");
  Check(pthread_key_create(&key, nullptr) == ENOMEM && key == 123 && _pthread_key_dest == nullptr,
        "reserve failure unchanged");
  Check(gesher_winpthread_cpu_test_control_v1(4, 2) == 0, "inject initial commit failure");
  Check(pthread_key_create(&key, nullptr) == ENOMEM && key == 123 && _pthread_key_dest == nullptr,
        "initial commit failure unchanged");
  Check(gesher_winpthread_cpu_test_control_v1(4, 0) == 0, "clear key fault");
  Check(pthread_key_create(&key, nullptr) == 0 && _pthread_key_dest != nullptr, "key initial growth");
  auto table = _pthread_key_dest;
  Check(pthread_setspecific(key, &key) == 0, "key value set");
  Check(gesher_winpthread_cpu_test_control_v1(4, 2) == 0, "inject later commit failure");
  Check(pthread_key_create(&next, nullptr) == ENOMEM && next == 456 && _pthread_key_dest == table &&
        pthread_getspecific(key) == &key, "growth failure preserves table and value");
  Check(gesher_winpthread_cpu_test_control_v1(4, 0) == 0, "clear growth fault");
  Check(pthread_key_create(&next, nullptr) == 0 && _pthread_key_dest == table, "growth recovery");
  Check(pthread_key_delete(key) == 0 && pthread_key_delete(next) == 0, "key cleanup");
  std::puts("PASS host-key-fault reserve initial-commit growth-failure unchanged and recovery");
  (void)pthread_self();
  DWORD baseline = 0, final_count = 0;
  Check(GetProcessHandleCount(GetCurrentProcess(), &baseline), "handle baseline");
  Target first, second;
  pthread_t first_id, second_id;
  Check(pthread_create(&first_id, nullptr, Worker, &first) == 0, "first create");
  Check(pthread_create(&second_id, nullptr, Worker, &second) == 0, "second create");
  Await(first); Await(second);
  Check(first_id == first.identity && second_id == second.identity, "real public identities");
  Check(gesher_winpthread_cpu_test_control_v1(1, 1) == 0, "enable duplication failure");
  for (int i = 0; i != 512; ++i) {
    void* owned = reinterpret_cast<void*>(static_cast<uintptr_t>(123));
    unsigned long long generation = 456;
    Check(gesher_winpthread_dup_cpu_handle_v1(first_id, &owned, &generation) == EAGAIN &&
          owned == reinterpret_cast<void*>(static_cast<uintptr_t>(123)) && generation == 456, "supplier failure outputs");
    clockid_t unchanged = 123;
    Check(pthread_getcpuclockid(first_id, &unchanged) == EAGAIN && unchanged == 123, "host failure output");
  }
  Check(WaitForSingleObject(first.oracle, 0) == WAIT_TIMEOUT, "failure used live target");
  Check(gesher_winpthread_cpu_test_control_v1(1, 0) == 0, "disable duplication failure");
  clockid_t saved;
  Check(pthread_getcpuclockid(first_id, &saved) == 0, "recovery genuine clock");
  timespec sample{};
  Check(clock_gettime(saved, &sample) == 0, "recovered clock query");
  std::puts("PASS host-clock-fault 512-live-target-duplicate-failures outputs recovery");
  Check(gesher_host_cpu_clock_test_exhaust_ids_v1() == 0, "exhaust host IDs");
  clockid_t cached = 123, unchanged = 456;
  Check(pthread_getcpuclockid(first_id, &cached) == 0 && cached == saved, "cached acquisition at exhaustion");
  Check(pthread_getcpuclockid(second_id, &unchanged) == EAGAIN && unchanged == 456, "new real target ID exhaustion");
  Check(gesher_winpthread_cpu_test_control_v1(2, 0) == 0, "exhaust supplier generations");
  pthread_t impossible = 0;
  Check(pthread_create(&impossible, nullptr, Worker, &second) == EAGAIN, "generation exhaustion registration rejected");
  Check(pthread_getcpuclockid(first_id, &cached) == 0 && cached == saved, "existing authority survives generation exhaustion");
  first.stop = true; second.stop = true;
  Check(pthread_join(first_id, nullptr) == 0 && pthread_join(second_id, nullptr) == 0, "real fault targets joined");
  Check(WaitForSingleObject(first.oracle, 0) == WAIT_OBJECT_0 && WaitForSingleObject(second.oracle, 0) == WAIT_OBJECT_0, "real kernel exits");
  Check(CloseHandle(first.oracle) && CloseHandle(second.oracle), "owned oracles closed");
  timespec retired{42, 43};
  Check(clock_gettime(saved, &retired) == -1 && errno == EINVAL && retired.tv_sec == 42 && retired.tv_nsec == 43, "retired output unchanged");
  Check(GetProcessHandleCount(GetCurrentProcess(), &final_count) && final_count <= baseline + 8, "fault handle reclamation");
  std::printf("PASS host-clock-fault ID-and-generation-exhaustion retained-authority real-exit handles=%lu/%lu\n", baseline, final_count);
}
