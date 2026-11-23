// Native Windows regression for a stream operation concurrent with a pthread
// TLS destructor. Run as a separate child so the existing process timeout
// reports a lock inversion as failure without stranding the rest of the suite.
#include <pthread.h>

namespace {
struct StdioTlsExitContext {
  FILE* stream;
  HANDLE entered;
  pthread_key_t key;
  bool installed = false;
  bool written = false;
};

void StdioTlsExitDestructor(void* value) {
  auto& context = *static_cast<StdioTlsExitContext*>(value);
  // The pinned winpthreads thread wrapper invokes this callback with its
  // thread registry locked. Main already owns the FILE lock; signalling here
  // makes the lock order deterministic without timing sleeps or private hooks.
  if (!SetEvent(context.entered)) return;
  context.written = fputs("tls", context.stream) >= 0;
}

void* StdioTlsExitWorker(void* value) {
  auto& context = *static_cast<StdioTlsExitContext*>(value);
  context.installed = pthread_setspecific(context.key, &context) == 0;
  if (!context.installed) SetEvent(context.entered);
  return nullptr;
}

int StdioTlsExitContract() {
  // Establish main's current record before the worker can exit. The repaired
  // setters already own that record; first attachment is a different operation.
  if (pthread_self() == 0) return 1;
  HANDLE reader = INVALID_HANDLE_VALUE, writer = INVALID_HANDLE_VALUE;
  if (!CreatePipe(&reader, &writer, nullptr, 4096)) return 2;
  int fd = _open_osfhandle(reinterpret_cast<intptr_t>(writer), _O_WRONLY | _O_BINARY);
  if (fd < 0) { CloseHandle(reader); CloseHandle(writer); return 3; }
  FILE* output = fdopen(fd, "wb");
  if (!output) { close(fd); CloseHandle(reader); return 4; }
  StdioTlsExitContext context{output, CreateEventW(nullptr, TRUE, FALSE, nullptr), {}};
  if (!context.entered) { fclose(output); CloseHandle(reader); return 5; }
  if (pthread_key_create(&context.key, StdioTlsExitDestructor) != 0) {
    CloseHandle(context.entered); fclose(output); CloseHandle(reader); return 6;
  }
  bool ready = setvbuf(output, nullptr, _IONBF, 0) == 0;
  pthread_t worker{};
  flockfile(output);
  int started = pthread_create(&worker, nullptr, StdioTlsExitWorker, &context);
  bool entered = started == 0 && WaitForSingleObject(context.entered, 5000) == WAIT_OBJECT_0;
  // This write must reach the real pipe and its cancellation admission while
  // main still owns FILE. A registry lookup in the current-thread cancellation
  // setters deadlocks against the TLS callback above.
  bool written = ready && entered && fputs("caller", output) >= 0;
  funlockfile(output);
  int joined = started == 0 ? pthread_join(worker, nullptr) : started;
  bool ok = ready && entered && written && joined == 0 && context.installed && context.written;
  ok = pthread_key_delete(context.key) == 0 && ok;
  CloseHandle(context.entered);
  ok = fclose(output) == 0 && ok;
  char bytes[9]{};
  DWORD available = 0, received = 0;
  ok = PeekNamedPipe(reader, nullptr, 0, nullptr, &available, nullptr) &&
       available == sizeof(bytes) && ReadFile(reader, bytes, sizeof(bytes), &received, nullptr) &&
       received == sizeof(bytes) && memcmp(bytes, "callertls", sizeof(bytes)) == 0 && ok;
  CloseHandle(reader);
  if (ok) puts("stdio TLS exit: all passed");
  return ok ? 0 : 7;
}
}  // namespace
