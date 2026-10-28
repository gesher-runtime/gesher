// CI only. Portable real fcntl reference and actual Windows internal control.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <thread>
#ifdef _WIN32
#include "../../compat/posix/src/record_lock_wait.h"
#include "../../compat/posix/src/internal.h"
namespace locks = gesher::record_locks;
#ifdef GESHER_RECORD_WAIT_WITNESS_TEST
#include "kernel_witness.h"
#endif
#ifdef GESHER_RECORD_WAIT_TEST
#include "cleanup_controls.h"
#endif
#else
#include <signal.h>
#include <pthread.h>
volatile sig_atomic_t caught = 0;
void Handler(int) { caught = 1; }
#endif
void Print(const char* label, int value) {
  std::printf("%s %d\n", label, value); std::fflush(stdout);
}
int main(int argc, char** argv) {
  if (argc != 5) return 80;
#if defined(_WIN32) && defined(GESHER_RECORD_WAIT_WITNESS_TEST)
  if (std::strcmp(argv[1], "kernel-cleanup") == 0) return kernel_witness::Cleanup(argv[3]);
#endif
  int fd = open(argv[2], O_RDWR);
  if (fd < 0) return 81;
  int command = std::atoi(argv[3]);
  struct flock lock{}; lock.l_type = F_WRLCK; lock.l_whence = SEEK_SET; lock.l_len = 1;
  if (std::strcmp(argv[1], "holder") == 0) {
    if (fcntl(fd, command == 38 ? F_OFD_SETLK : F_SETLK, &lock)) return 82;
    Print("held", getpid());
    char text[32];
    while (std::fgets(text, sizeof(text), stdin)) {
      if (std::strcmp(text, "unlock\n") == 0) {
        lock.l_type = F_UNLCK;
        if (fcntl(fd, command == 38 ? F_OFD_SETLK : F_SETLK, &lock)) return 83;
      } else if (std::strcmp(text, "close\n") == 0) {
        if (close(fd)) return 84;
        fd = -1;
      } else if (std::strcmp(text, "dupclose\n") == 0) {
        int alias = dup(fd); if (alias < 0 || close(alias)) return 85;
      } else if (std::strcmp(text, "exit\n") == 0) break;
      else return 86;
      Print("released", 0);
    }
    if (fd >= 0 && close(fd)) return 87;
    return 0;
  }
  if (std::strcmp(argv[1], "public-gaps") == 0) {
    errno = 0; int result = fcntl(fd, command, &lock);
    int error = errno;
    Print("public-result", result); Print("public-einval", error == EINVAL);
    return close(fd) ? 88 : 0;
  }
  if (std::strcmp(argv[1], "probe") == 0) {
    errno = 0;
    int result = fcntl(fd, F_SETLK, &lock);
    int error = errno;
    Print("conflict", result == -1 && (error == EAGAIN || error == EACCES));
    return close(fd) ? 98 : 0;
  }
#ifdef _WIN32
  locks::PreparedOperation prepared{};
  if (locks::PrepareOperation(gesher::HandleFromFd(fd), F_SETLK, &lock, &prepared)) return 89;
  HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!cancel) return 90;
#ifdef GESHER_RECORD_WAIT_WITNESS_TEST
  if (std::strcmp(argv[1], "kernel-witness") == 0) {
    int result = kernel_witness::Run(prepared, cancel);
    if (!CloseHandle(cancel)) return 96;
    return close(fd) ? 97 : result;
  }
#endif
#ifdef GESHER_RECORD_WAIT_TEST
  if (std::strcmp(argv[1], "cleanup-controls") == 0) {
    int error = cleanup_controls::Run(prepared, cancel, argv[4]);
    cleanup_controls::Close(cancel);
    return close(fd) ? 97 : error;
  }
#endif
#else
  struct sigaction action{}; action.sa_handler = Handler;
  if (std::strcmp(argv[4], "restart") == 0) action.sa_flags = SA_RESTART;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGUSR1, &action, nullptr)) return 91;
#endif
  Print("ready", getpid());
  std::thread waiting([&] {
#ifdef _WIN32
    locks::Registry registry;
    int error = registry.Open();
    if (!error) error = command == 38 ? ENOSYS : locks::WaitProcessLock(registry, prepared, cancel,
      std::strcmp(argv[4], "timeout") == 0 ? 1000 : 5000);
#else
    errno = 0;
    int result = fcntl(fd, command, &lock);
    int error = result == 0 ? 0 : errno;
#endif
    const char* outcome = error == 0 ? "success" : error == EINTR ? "interrupted" :
      error == ECANCELED ? "canceled" : error == ETIMEDOUT ? "timeout" : "error";
    std::printf("result %s %d\n", outcome, error); std::fflush(stdout);
  });
  char text[32];
  while (std::fgets(text, sizeof(text), stdin)) {
    if (std::strcmp(text, "interrupt\n") == 0) {
#ifdef _WIN32
      if (!SetEvent(cancel)) return 92;
#else
      if (pthread_kill(waiting.native_handle(), SIGUSR1)) return 93;
#endif
    } else if (std::strcmp(text, "observe\n") == 0) {
#ifdef _WIN32
      locks::Registry observer; locks::RegistryState* state = nullptr;
      if (observer.Open() || observer.Begin(&state, 1000)) return 94;
      int count = 0;
      for (size_t i = 0; i < state->wait_count; ++i)
        if (state->waits[i].thread.process.pid == GetCurrentProcessId() &&
            state->waits[i].requested.file == prepared.file) ++count;
      observer.Unlock(); Print("registered", count);
#else
      Print("caught", caught);
#endif
    } else if (std::strcmp(text, "join\n") == 0) break;
    else return 95;
  }
  waiting.join();
#ifdef _WIN32
  if (!CloseHandle(cancel)) return 96;
#endif
  return close(fd) ? 97 : 0;
}
