// Cancellation must retire the operation without retaining FILE locks/pins.
#include <atomic>
#include <pthread.h>
#include <time.h>

struct StdioCanceledOperation {
  FILE* stream;
  int operation;
  std::atomic<bool> entered{false};
};
static void* StdioCanceledCall(void* raw) {
  auto& call = *static_cast<StdioCanceledOperation*>(raw);
  call.entered.store(true, std::memory_order_release);
  if (call.operation == 0) fgetc(call.stream);
  else if (call.operation == 1) fprintf(call.stream, "%d-%s", 1, "pending");
  else if (call.operation == 2) fflush(NULL);
  else fclose(call.stream);
  return NULL;
}
static void StdioPause(void) {
  timespec delay = {0, 1000000};
  nanosleep(&delay, NULL);
}
static void test_stdio_cancel_contracts(void) {
  for (int operation = 0; operation != 4; ++operation) {
    fprintf(stderr, "stdio cancellation phase: %d\n", operation);
    int ends[2] = {-1, -1};
    check("stdio cancel pipe opens", pipe(ends) == 0);
    if (ends[0] < 0) continue;
    if (operation != 0) {
      check("stdio cancel fill is nonblocking", fcntl(ends[1], F_SETFL, O_NONBLOCK) == 0);
      char block[4096] = {};
      while (write(ends[1], block, sizeof(block)) > 0) {}
      check("stdio cancel fixture is full", errno == EAGAIN);
      check("stdio cancel writer becomes blocking", fcntl(ends[1], F_SETFL, 0) == 0);
    }
    FILE* stream = fdopen(ends[operation == 0 ? 0 : 1], operation == 0 ? "r" : "w");
    check("stdio cancel stream opens", stream != NULL);
    if (!stream) { close(ends[0]); close(ends[1]); continue; }
    char buffer[4096];
    check("stdio cancel buffering configured", setvbuf(stream, operation >= 2 ? buffer : NULL,
          operation >= 2 ? _IOFBF : _IONBF, operation >= 2 ? sizeof(buffer) : 0) == 0);
    if (operation >= 2) check("stdio cancel flush has pending bytes", fputs("pending", stream) >= 0);
    StdioCanceledOperation call{stream, operation};
    pthread_t thread;
    int created = pthread_create(&thread, NULL, StdioCanceledCall, &call);
    check("stdio cancel caller starts", created == 0);
    if (created == 0) {
      while (!call.entered.load(std::memory_order_acquire)) StdioPause();
      for (int pause = 0; pause != 10; ++pause) StdioPause();
      check("stdio blocked operation accepts cancellation", pthread_cancel(thread) == 0);
      void* result = NULL;
      check("stdio canceled caller joins", pthread_join(thread, &result) == 0 && result == PTHREAD_CANCELED);
      int acquired = ftrylockfile(stream);
      check("stdio cancellation releases its stream lock", acquired == 0);
      if (acquired == 0) funlockfile(stream);
      if (operation == 0) {
        check("stdio canceled read leaves stream usable", write(ends[1], "r", 1) == 1 && fgetc(stream) == 'r');
      } else {
        check("stdio canceled write drain becomes nonblocking", fcntl(ends[0], F_SETFL, O_NONBLOCK) == 0);
        char block[4096];
        while (read(ends[0], block, sizeof(block)) > 0) {}
        check("stdio canceled output leaves stream usable", fputs("after", stream) >= 0 && fflush(stream) == 0);
      }
    }
    check("stdio canceled stream closes normally", fclose(stream) == 0);
    close(ends[operation == 0 ? 1 : 0]);
  }
}
