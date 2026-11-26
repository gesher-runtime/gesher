// The same public anonymous-pipe/epoll contract runs on Linux and Gesher.
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

namespace {
int checks, failures;

void Check(const char* name, bool passed) {
  ++checks;
  if (!passed) {
    ++failures;
    std::printf("FAIL %s errno=%d\n", name, errno);
  }
}

bool Add(int epoll, int writer, int tag) {
  // EPOLLERR is always reportable, even when normal write readiness is masked.
  epoll_event interest{};
  interest.data.u32 = tag;
  return epoll_ctl(epoll, EPOLL_CTL_ADD, writer, &interest) == 0;
}

bool Empty(int epoll) {
  epoll_event event{};
  return epoll_wait(epoll, &event, 1, 0) == 0;
}

bool WriterError(int epoll, unsigned tag) {
  epoll_event event{};
  return epoll_wait(epoll, &event, 1, 1000) == 1 &&
         event.events == EPOLLERR && event.data.u32 == tag;
}

void Run(bool replace_without_close) {
  int original[2] = {-1, -1};
  int replacement[2] = {-1, -1};
  int epoll = epoll_create1(EPOLL_CLOEXEC);
  Check("create epoll", epoll >= 0);
  const auto close_if_open = [](int fd) { if (fd >= 0) close(fd); };
  if (epoll < 0 || pipe2(original, O_NONBLOCK | O_CLOEXEC) < 0 ||
      pipe2(replacement, O_NONBLOCK | O_CLOEXEC) < 0) {
    Check("create two anonymous pipes", false);
    for (int fd : original) close_if_open(fd);
    for (int fd : replacement) close_if_open(fd);
    close_if_open(epoll);
    return;
  }
  const int reader_alias = dup(original[0]);
  const int writer_alias = dup(original[1]);
  const int surviving_writer = dup(original[1]);
  const bool aliases = reader_alias >= 0 && writer_alias >= 0 && surviving_writer >= 0;
  Check("initial aliases", aliases);
  if (aliases) {
    Check("register writer", Add(epoll, original[1], 17));
    Check("registered writer has no error before peer closes", Empty(epoll));
    Check("close one reader but keep its alias", close(original[0]) == 0);
    original[0] = -1;
    Check("one live reader keeps writer connected", Empty(epoll));

    if (!replace_without_close) Check("close watched writer number", close(original[1]) == 0);
    Check("reuse watched writer number for a second pipe", dup2(replacement[1], original[1]) == original[1]);
    Check("register second pipe at the same number", Add(epoll, original[1], 27));
    Check("unregistered alias cannot delete the original registration",
          epoll_ctl(epoll, EPOLL_CTL_DEL, writer_alias, nullptr) == -1 && errno == ENOENT);
    Check("retire first sampling alias", close(writer_alias) == 0);
    Check("both pipes remain connected", Empty(epoll));

    Check("close original pipe's final reader", close(reader_alias) == 0);
    Check("original registration observes only its own EPOLLERR", WriterError(epoll, 17));
    Check("error stays level-triggered while alias lives", WriterError(epoll, 17));
    Check("close final original writer alias", close(surviving_writer) == 0);
    Check("original registration retires at final writer close", Empty(epoll));

    Check("close second pipe's reader", close(replacement[0]) == 0);
    replacement[0] = -1;
    Check("reused number observes its own EPOLLERR", WriterError(epoll, 27));
    Check("remove reused number's registration",
          epoll_ctl(epoll, EPOLL_CTL_DEL, original[1], nullptr) == 0 && Empty(epoll));
  } else {
    close_if_open(reader_alias);
    close_if_open(writer_alias);
    close_if_open(surviving_writer);
  }
  for (int fd : original) close_if_open(fd);
  for (int fd : replacement) close_if_open(fd);
  close(epoll);
}
}  // namespace

int main() {
  Run(false);
  Run(true);
  std::printf("pipe epoll alias ownership: %d checks, %d failures\n", checks, failures);
  return failures != 0;
}
