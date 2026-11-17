// Accurate Pipe Semantics, O_NONBLOCK, Backpressure, and Epoll Readiness (Roadmap Items N30 & N31).
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

static int assertions = 0;
static int failures = 0;

#define CHECK(cond)                                                                \
  do {                                                                             \
    ++assertions;                                                                  \
    if (!(cond)) {                                                                 \
      ++failures;                                                                  \
      std::printf("FAIL line %d: %s (errno=%d)\n", __LINE__, #cond, errno);        \
    }                                                                              \
  } while (0)

#define CHECK_EQ(a, b)                                                             \
  do {                                                                             \
    ++assertions;                                                                  \
    auto val_a = (a);                                                              \
    auto val_b = (b);                                                              \
    if (val_a != val_b) {                                                          \
      ++failures;                                                                  \
      std::printf("FAIL line %d: %s == %s (actual %lld vs %lld, errno=%d)\n",      \
                  __LINE__, #a, #b, static_cast<long long>(val_a),                 \
                  static_cast<long long>(val_b), errno);                           \
    }                                                                              \
  } while (0)

static void TestPipeCreationAndFlags() {
  int fds[2] = {-1, -1};

  // Invalid flags must fail with EINVAL
  CHECK_EQ(pipe2(fds, 0x1234), -1);
  CHECK_EQ(errno, EINVAL);

  // Default pipe
  CHECK_EQ(pipe(fds), 0);
  CHECK(fds[0] >= 0);
  CHECK(fds[1] >= 0);

  int flags0 = fcntl(fds[0], F_GETFL);
  int flags1 = fcntl(fds[1], F_GETFL);
  CHECK_EQ(flags0 & O_ACCMODE, O_RDONLY);
  CHECK_EQ(flags1 & O_ACCMODE, O_WRONLY);
  CHECK_EQ(flags0 & O_NONBLOCK, 0);
  CHECK_EQ(flags1 & O_NONBLOCK, 0);

  close(fds[0]);
  close(fds[1]);

  // pipe2 with O_NONBLOCK | O_CLOEXEC
  CHECK_EQ(pipe2(fds, O_NONBLOCK | O_CLOEXEC), 0);
  flags0 = fcntl(fds[0], F_GETFL);
  flags1 = fcntl(fds[1], F_GETFL);
  CHECK_EQ(flags0 & O_ACCMODE, O_RDONLY);
  CHECK_EQ(flags1 & O_ACCMODE, O_WRONLY);
  CHECK((flags0 & O_NONBLOCK) != 0);
  CHECK((flags1 & O_NONBLOCK) != 0);

  int fdflags0 = fcntl(fds[0], F_GETFD);
  int fdflags1 = fcntl(fds[1], F_GETFD);
  CHECK((fdflags0 & FD_CLOEXEC) != 0);
  CHECK((fdflags1 & FD_CLOEXEC) != 0);

  close(fds[0]);
  close(fds[1]);
}

static void TestDynamicFcntlNonblock() {
  int fds[2];
  CHECK_EQ(pipe(fds), 0);

  // Set O_NONBLOCK on reader
  int flags0 = fcntl(fds[0], F_GETFL);
  CHECK_EQ(fcntl(fds[0], F_SETFL, flags0 | O_NONBLOCK), 0);
  CHECK((fcntl(fds[0], F_GETFL) & O_NONBLOCK) != 0);
  // Writer remains blocking
  CHECK_EQ(fcntl(fds[1], F_GETFL) & O_NONBLOCK, 0);

  // Non-blocking read on empty pipe returns EAGAIN
  char buf[32];
  CHECK_EQ(read(fds[0], buf, sizeof(buf)), -1);
  CHECK(errno == EAGAIN || errno == EWOULDBLOCK);

  // Toggle O_NONBLOCK back off
  CHECK_EQ(fcntl(fds[0], F_SETFL, flags0), 0);
  CHECK_EQ(fcntl(fds[0], F_GETFL) & O_NONBLOCK, 0);

  // Set O_NONBLOCK on writer
  int flags1 = fcntl(fds[1], F_GETFL);
  CHECK_EQ(fcntl(fds[1], F_SETFL, flags1 | O_NONBLOCK), 0);
  CHECK((fcntl(fds[1], F_GETFL) & O_NONBLOCK) != 0);
  // Reader remains blocking
  CHECK_EQ(fcntl(fds[0], F_GETFL) & O_NONBLOCK, 0);

  close(fds[0]);
  close(fds[1]);
}

static void TestNonblockingEmptyReadAndEOF() {
  int fds[2];
  CHECK_EQ(pipe2(fds, O_NONBLOCK), 0);

  char buf[64];
  // Empty pipe read yields EAGAIN
  CHECK_EQ(read(fds[0], buf, sizeof(buf)), -1);
  CHECK(errno == EAGAIN || errno == EWOULDBLOCK);

  // Write some data
  CHECK_EQ(write(fds[1], "hello", 5), 5);

  // Read partial data
  CHECK_EQ(read(fds[0], buf, 3), 3);
  CHECK_EQ(memcmp(buf, "hel", 3), 0);

  // Read remaining data
  CHECK_EQ(read(fds[0], buf, sizeof(buf)), 2);
  CHECK_EQ(memcmp(buf, "lo", 2), 0);

  // Now empty again -> EAGAIN
  CHECK_EQ(read(fds[0], buf, sizeof(buf)), -1);
  CHECK(errno == EAGAIN || errno == EWOULDBLOCK);

  // Close writer -> read yields 0 (EOF)
  close(fds[1]);
  CHECK_EQ(read(fds[0], buf, sizeof(buf)), 0);
  CHECK_EQ(read(fds[0], buf, sizeof(buf)), 0);

  close(fds[0]);
}

static void TestClosedReaderYieldsEPIPEAndSIGPIPE() {
  int fds[2];
  CHECK_EQ(pipe(fds), 0);

  // Block SIGPIPE so we can observe it in pending set
  sigset_t set, oldset;
  sigemptyset(&set);
  sigaddset(&set, SIGPIPE);
  CHECK_EQ(sigprocmask(SIG_BLOCK, &set, &oldset), 0);

  // Clear any existing pending SIGPIPE
  struct timespec ts = {0, 0};
  siginfo_t info;
  while (sigtimedwait(&set, &info, &ts) == SIGPIPE) {}

  // Close reader
  close(fds[0]);

  // Write to broken pipe must fail with EPIPE and raise SIGPIPE
  char buf[16] = "test";
  CHECK_EQ(write(fds[1], buf, 4), -1);
  CHECK_EQ(errno, EPIPE);

  // Verify SIGPIPE is pending
  sigset_t pending;
  sigemptyset(&pending);
  CHECK_EQ(sigpending(&pending), 0);
  CHECK(sigismember(&pending, SIGPIPE));

  // Consume SIGPIPE
  CHECK_EQ(sigtimedwait(&set, &info, &ts), SIGPIPE);

  // Restore old signal mask
  CHECK_EQ(sigprocmask(SIG_SETMASK, &oldset, nullptr), 0);

  close(fds[1]);
}

static void TestBackpressureAndPartialWrite() {
  int fds[2];
  CHECK_EQ(pipe2(fds, O_NONBLOCK), 0);

  std::vector<char> chunk(4096, 'A');
  size_t total_written = 0;

  // Fill the pipe until full
  for (int i = 0; i < 30; ++i) {
    int n = write(fds[1], chunk.data(), static_cast<unsigned int>(chunk.size()));
    if (n > 0) {
      total_written += n;
    } else {
      CHECK_EQ(n, -1);
      CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
      break;
    }
  }

  // Pipe is full: total_written should be 64KB (65536)
  CHECK_EQ(total_written, 64 * 1024);

  // Further non-blocking write to full pipe returns EAGAIN
  CHECK_EQ(write(fds[1], chunk.data(), 1), -1);
  CHECK(errno == EAGAIN || errno == EWOULDBLOCK);

  // Read 1024 bytes to create partial space
  std::vector<char> read_buf(1024);
  CHECK_EQ(read(fds[0], read_buf.data(), 1024), 1024);

  // Write 4096 bytes under O_NONBLOCK: should do partial write of 1024 bytes (up to capacity)
  int partial = write(fds[1], chunk.data(), 4096);
  CHECK_EQ(partial, 1024);

  // Now full again
  CHECK_EQ(write(fds[1], chunk.data(), 1), -1);
  CHECK(errno == EAGAIN || errno == EWOULDBLOCK);

  close(fds[0]);
  close(fds[1]);
}

static void TestEpollReadiness() {
  int fds[2];
  CHECK_EQ(pipe2(fds, O_NONBLOCK), 0);

  int epfd = epoll_create1(0);
  CHECK(epfd >= 0);

  epoll_event ev0 = {};
  ev0.events = EPOLLIN | EPOLLRDHUP;
  ev0.data.fd = fds[0];
  CHECK_EQ(epoll_ctl(epfd, EPOLL_CTL_ADD, fds[0], &ev0), 0);

  epoll_event ev1 = {};
  ev1.events = EPOLLOUT;
  ev1.data.fd = fds[1];
  CHECK_EQ(epoll_ctl(epfd, EPOLL_CTL_ADD, fds[1], &ev1), 0);

  // Initially: writer is writable (space available), reader is NOT readable (empty)
  epoll_event events[4];
  int n = epoll_wait(epfd, events, 4, 0);
  CHECK_EQ(n, 1);
  CHECK_EQ(events[0].data.fd, fds[1]);
  CHECK((events[0].events & EPOLLOUT) != 0);
  CHECK_EQ(events[0].events & EPOLLIN, 0);

  // Write data to pipe
  CHECK_EQ(write(fds[1], "ping", 4), 4);

  // Now both reader and writer should be ready
  n = epoll_wait(epfd, events, 4, 0);
  CHECK_EQ(n, 2);
  bool found_read = false, found_write = false;
  for (int i = 0; i < n; ++i) {
    if (events[i].data.fd == fds[0]) {
      found_read = true;
      CHECK((events[i].events & EPOLLIN) != 0);
    }
    if (events[i].data.fd == fds[1]) {
      found_write = true;
      CHECK((events[i].events & EPOLLOUT) != 0);
    }
  }
  CHECK(found_read);
  CHECK(found_write);

  // Close writer -> reader peer closure must report EPOLLHUP (and EPOLLIN because data/EOF is present)
  close(fds[1]);
  n = epoll_wait(epfd, events, 4, 100);
  CHECK_EQ(n, 1);
  CHECK_EQ(events[0].data.fd, fds[0]);
  CHECK((events[0].events & EPOLLHUP) != 0);
  CHECK((events[0].events & EPOLLIN) != 0);

  // Read data and verify EOF
  char buf[16];
  CHECK_EQ(read(fds[0], buf, sizeof(buf)), 4);
  CHECK_EQ(read(fds[0], buf, sizeof(buf)), 0);

  // After EOF, still reports EPOLLHUP
  n = epoll_wait(epfd, events, 4, 0);
  CHECK_EQ(n, 1);
  CHECK_EQ(events[0].data.fd, fds[0]);
  CHECK((events[0].events & EPOLLHUP) != 0);

  close(fds[0]);
  close(epfd);

  // Test writer EPOLLERR on reader peer closure
  CHECK_EQ(pipe2(fds, O_NONBLOCK), 0);
  epfd = epoll_create1(0);
  CHECK(epfd >= 0);

  ev1.events = EPOLLOUT;
  ev1.data.fd = fds[1];
  CHECK_EQ(epoll_ctl(epfd, EPOLL_CTL_ADD, fds[1], &ev1), 0);

  // Close reader end
  close(fds[0]);

  // Writer should report EPOLLERR
  n = epoll_wait(epfd, events, 4, 100);
  CHECK_EQ(n, 1);
  CHECK_EQ(events[0].data.fd, fds[1]);
  CHECK((events[0].events & EPOLLERR) != 0);

  close(fds[1]);
  close(epfd);
}

static void TestPollAndSelectReadiness() {
  int fds[2];
  CHECK_EQ(pipe2(fds, O_NONBLOCK), 0);

  pollfd pfd[2] = {
      {fds[0], POLLIN, 0},
      {fds[1], POLLOUT, 0},
  };

  // Initially only writer is ready
  int ret = poll(pfd, 2, 0);
  CHECK_EQ(ret, 1);
  CHECK_EQ(pfd[0].revents, 0);
  CHECK((pfd[1].revents & POLLOUT) != 0);

  // Write data
  CHECK_EQ(write(fds[1], "abc", 3), 3);

  // Both ready
  ret = poll(pfd, 2, 0);
  CHECK_EQ(ret, 2);
  CHECK((pfd[0].revents & POLLIN) != 0);
  CHECK((pfd[1].revents & POLLOUT) != 0);

  // Close write end -> reader gets POLLHUP
  close(fds[1]);
  ret = poll(pfd, 1, 0);
  CHECK_EQ(ret, 1);
  CHECK((pfd[0].revents & POLLHUP) != 0);
  CHECK((pfd[0].revents & POLLIN) != 0);

  close(fds[0]);
}

static void TestDupSharingAndLifetime() {
  int fds[2];
  CHECK_EQ(pipe(fds), 0);

  int r_dup = dup(fds[0]);
  CHECK(r_dup >= 0);
  int w_dup = dup(fds[1]);
  CHECK(w_dup >= 0);

  // Set O_NONBLOCK on original fds[0] -> r_dup must reflect it (shared file description)
  CHECK_EQ(fcntl(fds[0], F_SETFL, O_NONBLOCK), 0);
  CHECK((fcntl(r_dup, F_GETFL) & O_NONBLOCK) != 0);

  // Close fds[0]; r_dup is still an open reader
  close(fds[0]);

  // Write should succeed because r_dup is still open
  CHECK_EQ(write(w_dup, "dup", 3), 3);

  char buf[16] = {0};
  CHECK_EQ(read(r_dup, buf, sizeof(buf)), 3);
  CHECK_EQ(memcmp(buf, "dup", 3), 0);

  // Close r_dup; now NO readers remain
  close(r_dup);

  // Write to w_dup should now fail with EPIPE
  sigset_t set, oldset;
  sigemptyset(&set);
  sigaddset(&set, SIGPIPE);
  CHECK_EQ(sigprocmask(SIG_BLOCK, &set, &oldset), 0);

  CHECK_EQ(write(w_dup, "fail", 4), -1);
  CHECK_EQ(errno, EPIPE);

  struct timespec ts = {0, 0};
  siginfo_t info;
  CHECK_EQ(sigtimedwait(&set, &info, &ts), SIGPIPE);
  CHECK_EQ(sigprocmask(SIG_SETMASK, &oldset, nullptr), 0);

  close(fds[1]);
  close(w_dup);
}

static void TestThreadingSynchronization() {
  int fds[2];
  // Blocking pipe
  CHECK_EQ(pipe(fds), 0);

  std::atomic<bool> thread_started{false};
  std::thread reader_thread([&] {
    thread_started = true;
    char buf[16] = {0};
    int n = read(fds[0], buf, 5);
    CHECK_EQ(n, 5);
    CHECK_EQ(memcmp(buf, "hello", 5), 0);
    // Next read blocks until writer writes "world"
    n = read(fds[0], buf, 5);
    CHECK_EQ(n, 5);
    CHECK_EQ(memcmp(buf, "world", 5), 0);
    // Final read blocks until writer closes -> 0 (EOF)
    n = read(fds[0], buf, sizeof(buf));
    CHECK_EQ(n, 0);
  });

  while (!thread_started) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CHECK_EQ(write(fds[1], "hello", 5), 5);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CHECK_EQ(write(fds[1], "world", 5), 5);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  close(fds[1]);
  reader_thread.join();
  close(fds[0]);
}

int main() {
  std::printf("Running pipe contract tests...\n");
  TestPipeCreationAndFlags();
  TestDynamicFcntlNonblock();
  TestNonblockingEmptyReadAndEOF();
  TestClosedReaderYieldsEPIPEAndSIGPIPE();
  TestBackpressureAndPartialWrite();
  TestEpollReadiness();
  TestPollAndSelectReadiness();
  TestDupSharingAndLifetime();
  TestThreadingSynchronization();

  std::printf("pipe_contract_test: %d assertions, %d failures\n", assertions, failures);
  return failures == 0 ? 0 : 1;
}
