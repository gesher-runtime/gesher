// Actual virtual resources through the public FILE close/reopen paths.
// Also runs on Linux, where these descriptors need no CRT reservation slot.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

static void test_stdio_virtual_contracts(void) {
  for (int reopen = 0; reopen != 3; ++reopen) {
    int pair[2];
    check("stdio socketpair opens real endpoints", socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    int alias = dup(pair[0]);
    FILE* stream = fdopen(pair[0], "r+");
    check("fdopen accepts a real socket", stream != NULL && alias >= 0);
    if (!stream || alias < 0) { if (stream) fclose(stream); else close(pair[0]); close(pair[1]); if (alias >= 0) close(alias); continue; }
    check("socket stream flush reaches peer", fputs("s", stream) >= 0 && fflush(stream) == 0);
    char byte = 0;
    check("socket stream peer receives byte", read(pair[1], &byte, 1) == 1 && byte == 's');
    int slot = fileno(stream);
    if (reopen == 0) check("socket stream closes", fclose(stream) == 0);
    else if (reopen == 1) {
      stream = freopen("/dev/null", "r+", stream);
      check("socket stream reopens on actual device", stream != NULL);
      if (stream) { check("reopened slot no longer routes to old socket", fputs("discarded", stream) >= 0 && fflush(stream) == 0); fclose(stream); }
    } else check("failed socket reopen retires original", freopen("", "r", stream) == NULL);
    check("duplicate survives stream retirement", write(alias, "d", 1) == 1 && read(pair[1], &byte, 1) == 1 && byte == 'd');
    check("last socket alias closes", close(alias) == 0);
    check("peer sees EOF after FILE and last alias retire", read(pair[1], &byte, 1) == 0);
    close(pair[1]);
    int fresh = open("/dev/null", O_RDWR);
    check("reused CRT slot has device semantics", fresh >= 0 && dup2(fresh, slot) == slot && read(slot, &byte, 1) == 0);
    if (fresh != slot) close(fresh);
    close(slot);
  }

  int event = eventfd(0, EFD_NONBLOCK);
  int alias = dup(event);
  int epoll = epoll_create1(0);
  epoll_event interest = {};
  interest.events = EPOLLIN;
  interest.data.fd = event;
  check("event stream fixture registers readiness", event >= 0 && alias >= 0 && epoll >= 0 &&
        epoll_ctl(epoll, EPOLL_CTL_ADD, event, &interest) == 0);
  FILE* stream = fdopen(event, "r+");
  check("fdopen accepts actual event resource", stream != NULL);
  if (stream) {
    check("event stream uses unbuffered writes", setvbuf(stream, NULL, _IONBF, 0) == 0);
    uint64_t count = 3;
    check("event stream writes actual counter", fwrite(&count, sizeof(count), 1, stream) == 1);
    count = 0;
    check("event alias observes stream counter", read(alias, &count, sizeof(count)) == sizeof(count) && count == 3);
    check("event stream closes", fclose(stream) == 0);
    count = 5;
    check("event alias survives FILE close", write(alias, &count, sizeof(count)) == sizeof(count));
    count = 0;
    check("event alias remains readable", read(alias, &count, sizeof(count)) == sizeof(count) && count == 5);
  } else close(event);
  close(alias);
  close(epoll);
  int reused = open("/dev/null", O_RDWR);
  uint64_t count = 0;
  check("retired event slot cannot route to stale object", reused >= 0 && dup2(reused, event) == event && read(event, &count, sizeof(count)) == 0);
  if (reused != event) close(reused);
  close(event);

  event = eventfd(1, EFD_NONBLOCK);
  epoll = epoll_create1(0);
  stream = fdopen(epoll, "r");
  check("fdopen accepts actual epoll resource", stream != NULL && event >= 0);
  if (stream) check("epoll stream closes its resource", fclose(stream) == 0);
  else close(epoll);
  reused = open("/dev/null", O_RDWR);
  check("closed epoll numeric slot is reused", reused >= 0 && dup2(reused, epoll) == epoll);
  interest.data.fd = event;
  errno = 0;
  check("reused epoll slot has no retired registry", epoll_ctl(epoll, EPOLL_CTL_ADD, event, &interest) == -1 && errno == EINVAL);
  if (reused != epoll) close(reused);
  close(epoll); close(event);
}
