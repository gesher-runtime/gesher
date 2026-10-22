// libutils probe: Looper over gesher-base's epoll and eventfd, RefBase and
// String8/String16, as libandroid_runtime's MessageQueue and the framework's
// natives use them. Prints "all passed" and exits 0 when every check holds.
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <thread>

#include <cutils/properties.h>
#include <utils/Looper.h>
#include <utils/RefBase.h>
#include <utils/String16.h>
#include <utils/String8.h>
#include <utils/SystemClock.h>
#include <utils/Timers.h>

using namespace android;

static int fails = 0;
#define CHECK(c)                                                              \
  do {                                                                        \
    if (!(c)) {                                                               \
      printf("FAIL line %d: %s (errno %d)\n", __LINE__, #c, errno);           \
      ++fails;                                                                \
    }                                                                         \
  } while (0)

namespace {

std::atomic<int> destroyed{0};

struct Counted : public RefBase {
  ~Counted() override { ++destroyed; }
};

struct Recorder : public MessageHandler {
  std::atomic<int> what{0};
  void handleMessage(const Message& message) override { what = message.what; }
};

struct FdCallback : public LooperCallback {
  std::atomic<int> calls{0};
  int handleEvent(int fd, int events, void*) override {
    uint64_t v;
    if (events & Looper::EVENT_INPUT) read(fd, &v, sizeof(v));
    ++calls;
    return 1;  // keep watching
  }
};

}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);

  // RefBase: strong and weak references, destruction on the last strong one.
  {
    sp<Counted> a = sp<Counted>::make();
    wp<Counted> weak = a;
    CHECK(a->getStrongCount() == 1);
    {
      sp<Counted> b = a;
      CHECK(a->getStrongCount() == 2);
    }
    CHECK(weak.promote() == a);
    a.clear();
    CHECK(destroyed == 1);
    CHECK(weak.promote() == nullptr);
  }

  // String8 and String16.
  {
    String8 s = String8::format("%s-%d", "gesher", 16);
    CHECK(s == "gesher-16");
    s.append("/x");
    CHECK(s.length() == 11);
    String16 wide(s);
    CHECK(wide.size() == 11);
    CHECK(String8(wide) == "gesher-16/x");
    String8 utf8("\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d");  // shalom, in Hebrew
    CHECK(String16(utf8).size() == 4);
  }

  // Timers: the monotonic clocks advance.
  {
    nsecs_t t0 = systemTime(SYSTEM_TIME_MONOTONIC);
    int64_t up0 = uptimeMillis();
    usleep(20 * 1000);
    CHECK(systemTime(SYSTEM_TIME_MONOTONIC) - t0 >= 15 * 1000000LL);
    CHECK(uptimeMillis() - up0 >= 15);
    CHECK(elapsedRealtime() >= uptimeMillis());
  }

  // libcutils' properties, over libbase's in-process store.
  {
    CHECK(property_set("gesher.probe", "42") == 0);
    char value[PROPERTY_VALUE_MAX];
    CHECK(property_get("gesher.probe", value, "") == 2);
    CHECK(property_get_int32("gesher.probe", 0) == 42);
  }

  // gesher-base additions libutils and libbinder rely on.
  {
    CHECK(pthread_atfork(nullptr, nullptr, nullptr) == 0);  // no fork; accepted
    int efd = eventfd(0, EFD_NONBLOCK);
    int dup_fd = fcntl(efd, F_DUPFD_CLOEXEC, 0);
    CHECK(dup_fd >= 0 && dup_fd != efd);
    uint64_t v = 5;
    CHECK(write(dup_fd, &v, sizeof(v)) == 8);  // the same counter
    CHECK(read(efd, &v, sizeof(v)) == 8 && v == 5);
    int high = fcntl(efd, F_DUPFD_CLOEXEC, 100);
    CHECK(high >= 100);
    CHECK(fcntl(efd, F_DUPFD, -1) == -1 && errno == EINVAL);
    close(high);
    close(dup_fd);
    close(efd);
  }

  sp<Looper> looper = Looper::prepare(0);
  CHECK(looper != nullptr);
  CHECK(Looper::getForThread() == looper);

  // pollOnce with nothing to do times out after about the timeout.
  {
    int64_t start = uptimeMillis();
    int r = looper->pollOnce(100);
    int64_t took = uptimeMillis() - start;
    CHECK(r == Looper::POLL_TIMEOUT);
    CHECK(took >= 90 && took < 1000);
    CHECK(looper->pollOnce(0) == Looper::POLL_TIMEOUT);
  }

  // wake() from another thread ends a long poll at once (the wake eventfd).
  {
    std::thread waker([&] {
      usleep(50 * 1000);
      looper->wake();
    });
    int64_t start = uptimeMillis();
    int r = looper->pollOnce(5000);
    int64_t took = uptimeMillis() - start;
    waker.join();
    CHECK(r == Looper::POLL_WAKE);
    CHECK(took < 1000);
    // A wake before the poll is not lost.
    looper->wake();
    CHECK(looper->pollOnce(1000) == Looper::POLL_WAKE);
  }

  // Delayed messages are handled after their time, in order.
  {
    sp<Recorder> handler = sp<Recorder>::make();
    nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
    looper->sendMessageAtTime(now + ms2ns(60), handler, Message(2));
    looper->sendMessageAtTime(now + ms2ns(30), handler, Message(1));
    int64_t start = uptimeMillis();
    while (handler->what != 1 && uptimeMillis() - start < 2000) looper->pollOnce(1000);
    CHECK(handler->what == 1);
    CHECK(uptimeMillis() - start >= 20);
    while (handler->what != 2 && uptimeMillis() - start < 2000) looper->pollOnce(1000);
    CHECK(handler->what == 2);
    looper->sendMessage(handler, Message(3));
    looper->removeMessages(handler);
    looper->pollOnce(0);
    CHECK(handler->what == 2);
  }

  // An fd with a callback: an eventfd written by another thread.
  {
    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    CHECK(efd >= 0);
    sp<FdCallback> callback = sp<FdCallback>::make();
    CHECK(looper->addFd(efd, Looper::POLL_CALLBACK, Looper::EVENT_INPUT, callback, nullptr) == 1);
    std::thread writer([&] {
      usleep(50 * 1000);
      uint64_t one = 1;
      write(efd, &one, sizeof(one));
    });
    int64_t start = uptimeMillis();
    while (callback->calls == 0 && uptimeMillis() - start < 2000) looper->pollOnce(1000);
    writer.join();
    CHECK(callback->calls == 1);
    CHECK(looper->removeFd(efd) == 1);
    close(efd);
  }

  // An fd with an ident: pollOnce returns it.
  {
    int efd = eventfd(1, EFD_NONBLOCK);
    CHECK(looper->addFd(efd, 7, Looper::EVENT_INPUT, nullptr, nullptr) == 1);
    int events = 0;
    CHECK(looper->pollOnce(1000, nullptr, &events, nullptr) == 7);
    CHECK((events & Looper::EVENT_INPUT) != 0);
    CHECK(looper->removeFd(efd) == 1);
    close(efd);
  }

  printf(fails != 0 ? "%d failures\n" : "all passed\n", fails);
  return fails != 0;
}
