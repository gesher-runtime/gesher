// One portable socket contract, executed by Linux libc and the actual bionic
// x86_64/arm64 bridges. Reference rows contain actual results, not a script model.
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <initializer_list>

static void require(bool yes, const char* what) {
  if (!yes) { fprintf(stderr, "FAIL UDP: %s errno=%d\n", what, errno); exit(1); }
}
struct Pair { int rx, tx; sockaddr_storage addr; socklen_t len; };
static Pair pair(int family, int type = SOCK_DGRAM) {
  Pair p = {};
  p.rx = socket(family, type, 0); p.tx = socket(family, type, 0);
  require(p.rx >= 0 && p.tx >= 0, "socket");
  p.addr.ss_family = family;
  if (family == AF_INET) {
    auto* a = reinterpret_cast<sockaddr_in*>(&p.addr);
    a->sin_addr.s_addr = htonl(INADDR_LOOPBACK); p.len = sizeof(*a);
  } else {
    auto* a = reinterpret_cast<sockaddr_in6*>(&p.addr);
    a->sin6_addr = in6addr_loopback; p.len = sizeof(*a);
  }
  require(bind(p.rx, reinterpret_cast<sockaddr*>(&p.addr), p.len) == 0, "bind");
  require(bind(p.tx, reinterpret_cast<sockaddr*>(&p.addr), p.len) == 0, "sender bind");
  require(getsockname(p.rx, reinterpret_cast<sockaddr*>(&p.addr), &p.len) == 0, "name");
  timeval timeout = {3, 0};
  require(setsockopt(p.rx, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0, "timeout");
  return p;
}
static void send_packet(Pair& p, const void* data, size_t size) {
  require(sendto(p.tx, data, size, 0, reinterpret_cast<sockaddr*>(&p.addr), p.len) ==
          static_cast<ssize_t>(size), "send packet");
}
static void empty(Pair& p) {
  char c; errno = 0;
  require(recv(p.rx, &c, 1, MSG_DONTWAIT) == -1 && (errno == EAGAIN || errno == EWOULDBLOCK),
          "packet consumed exactly once");
}
static void matrix(int family) {
  Pair p = pair(family);
  const char payload[] = "abcdefghi";
  for (int api = 0; api < 3; ++api) for (int trunc = 0; trunc < 2; ++trunc)
    for (int peek = 0; peek < 2; ++peek) for (int size : {0, 9})
      for (int capacity : {0, 4, 9, 13}) {
        send_packet(p, payload, size);
        unsigned char data[20]; memset(data, 0xa5, sizeof(data));
        sockaddr_storage peer = {}; socklen_t peer_len = sizeof(peer);
        iovec iov[3] = {{data + 1, static_cast<size_t>(capacity / 2)},
                        {data + 1 + capacity / 2, 0},
                        {data + 1 + capacity / 2, static_cast<size_t>(capacity - capacity / 2)}};
        msghdr msg = {}; msg.msg_name = &peer; msg.msg_namelen = peer_len;
        msg.msg_iov = iov; msg.msg_iovlen = 3; msg.msg_flags = -1;
        int flags = (trunc ? MSG_TRUNC : 0) | (peek ? MSG_PEEK : 0);
        ssize_t n = api == 0 ? recv(p.rx, data + 1, capacity, flags) :
                    api == 1 ? recvfrom(p.rx, data + 1, capacity, flags,
                                       reinterpret_cast<sockaddr*>(&peer), &peer_len) :
                               recvmsg(p.rx, &msg, flags);
        int copied = size < capacity ? size : capacity;
        require(n == (trunc ? size : copied), "return copied/full length");
        require(memcmp(data + 1, payload, copied) == 0 && data[0] == 0xa5 &&
                data[1 + copied] == 0xa5, "prefix and buffer guards");
        int out = api == 2 ? msg.msg_flags : 0;
        require(out == ((api == 2 && size > capacity) ? MSG_TRUNC : 0), "output flags");
        if (api != 0) {
          require(peer.ss_family == family && (api == 1 ? peer_len : msg.msg_namelen) == p.len,
                  "source address layout");
          sockaddr_storage source = {}; socklen_t source_len = sizeof(source);
          require(getsockname(p.tx, reinterpret_cast<sockaddr*>(&source), &source_len) == 0,
                  "sender name");
          require(memcmp(&peer, &source, p.len) == 0, "sender address and port");
        }
        if (peek) {
          char whole[16] = {};
          require(recv(p.rx, whole, sizeof(whole), 0) == size && memcmp(whole, payload, size) == 0,
                  "peek retains complete packet");
        }
        empty(p);
        printf("UDP family=%d api=%d trunc=%d peek=%d payload=%d cap=%d n=%zd flags=%d\n",
               family, api, trunc, peek, size, capacity, n, out);
      }
  // No-iovec recvmsg still consumes a message and reports truncation.
  send_packet(p, payload, 9); msghdr no_iov = {};
  require(recvmsg(p.rx, &no_iov, MSG_TRUNC) == 9 && no_iov.msg_flags == MSG_TRUNC,
          "empty iovec MSG_TRUNC"); empty(p);
  // Actual Linux ignores OOB on UDP, copies the payload and consumes the packet.
  send_packet(p, payload, 9); char data[16]; errno = 0;
  require(recv(p.rx, data, sizeof(data), MSG_OOB) == 9 && memcmp(data, payload, 9) == 0,
          "UDP OOB ignored and payload received");
  empty(p);
  // An empty error queue leaves the separate ordinary datagram intact.
  send_packet(p, payload, 9); errno = 0;
  require(recv(p.rx, data, sizeof(data), MSG_ERRQUEUE | MSG_DONTWAIT) == -1 && errno == EAGAIN,
          "empty error queue");
  require(recv(p.rx, data, sizeof(data), MSG_WAITALL) == 9 && memcmp(data, payload, 9) == 0,
          "error flags preserve packet and UDP WAITALL ignored");
  empty(p);
  printf("UDP family=%d no-iov/errors/waitall PASS\n", family);
  close(p.tx); close(p.rx);
}
struct Thread { int fd; int seen[64]; };
static void* receive_many(void* arg) {
  auto* t = static_cast<Thread*>(arg);
  for (int i = 0; i < 32; ++i) {
    unsigned char data[4] = {}; iovec io = {data, sizeof(data)};
    msghdr msg = {}; msg.msg_iov = &io; msg.msg_iovlen = 1;
    ssize_t n = recvmsg(t->fd, &msg, MSG_TRUNC);
    require(data[0] < 64 && n == (data[0] % 2 ? 512 : 128) && msg.msg_flags == MSG_TRUNC,
            "concurrent aliased packet length belongs to prefix");
    require(data[1] == 0x42 && data[2] == 0x43 && data[3] == 0x44, "concurrent prefix");
    ++t->seen[data[0]];
  }
  return nullptr;
}
static void concurrent(int family) {
  Pair p = pair(family); int capacity = 262144;
  require(setsockopt(p.rx, SOL_SOCKET, SO_RCVBUF, &capacity, sizeof(capacity)) == 0, "queue");
  int alias = dup(p.rx); require(alias >= 0, "duplicate endpoint");
  for (int id = 0; id < 64; ++id) {
    unsigned char data[512] = {}; data[0] = id; data[1] = 0x42; data[2] = 0x43; data[3] = 0x44;
    send_packet(p, data, id % 2 ? 512 : 128);
  }
  Thread a = {}, b = {}; a.fd = p.rx; b.fd = alias; pthread_t x, y;
  require(pthread_create(&x, nullptr, receive_many, &a) == 0, "thread a");
  require(pthread_create(&y, nullptr, receive_many, &b) == 0, "thread b");
  require(pthread_join(x, nullptr) == 0 && pthread_join(y, nullptr) == 0, "join");
  for (int i = 0; i < 64; ++i) require(a.seen[i] + b.seen[i] == 1, "unique packet ownership");
  empty(p); close(alias); close(p.tx); close(p.rx);
  printf("UDP family=%d alias-concurrency=64 PASS\n", family);
}
static void boundaries(int family) {
  Pair p = pair(family);
  char payload[65527]; memset(payload, 'Z', sizeof(payload));
  int size = family == AF_INET ? 65507 : 65527;
  send_packet(p, payload, size); char data[4] = {};
  require(recv(p.rx, data, sizeof(data), MSG_TRUNC) == size && memcmp(data, "ZZZZ", 4) == 0,
          "ordinary maximum IP datagram"); empty(p);
  iovec iov[1025] = {}; iov[1023] = {data, sizeof(data)};
  msghdr msg = {}; msg.msg_iov = iov; msg.msg_iovlen = 1024;
  send_packet(p, "abcdefghi", 9);
  require(recvmsg(p.rx, &msg, MSG_TRUNC) == 9 && msg.msg_flags == MSG_TRUNC &&
          memcmp(data, "abcd", 4) == 0, "1024 iovecs"); empty(p);
  msg.msg_iovlen = 1025; send_packet(p, "abcdefghi", 9); errno = 0;
  require(recvmsg(p.rx, &msg, 0) == -1 && errno == EMSGSIZE, "too many iovecs");
  require(recv(p.rx, data, sizeof(data), MSG_TRUNC) == 9 && memcmp(data, "abcd", 4) == 0,
          "iov error leaves datagram queued"); empty(p);
  close(p.rx); close(p.tx);
  printf("UDP family=%d max=%d iov=1024/1025 PASS\n", family, size);
}
static void stream() {
  Pair p = pair(AF_INET, SOCK_STREAM);
  require(listen(p.rx, 1) == 0 && connect(p.tx, reinterpret_cast<sockaddr*>(&p.addr), p.len) == 0,
          "stream connect");
  int rx = accept(p.rx, nullptr, nullptr); require(rx >= 0, "accept");
  require(send(p.tx, "abcdefghi", 9, 0) == 9, "stream send");
  char data[16] = {}; iovec io = {data, 4}; msghdr msg = {}; msg.msg_iov = &io; msg.msg_iovlen = 1;
  require(recvmsg(rx, &msg, MSG_WAITALL) == 4 && msg.msg_flags == 0 && memcmp(data, "abcd", 4) == 0,
          "stream short read never datagram truncation");
  require(recv(rx, data, 5, MSG_WAITALL) == 5 && memcmp(data, "efghi", 5) == 0,
          "stream tail retained");
  close(rx); close(p.rx); close(p.tx); puts("UDP stream-control PASS");
}
static void observe_oob() {
  for (int family : {AF_INET, AF_INET6}) {
    Pair p = pair(family); send_packet(p, "abcdefghi", 9);
    unsigned char data[16] = {}, next[16] = {}; errno = 0;
    ssize_t n = recv(p.rx, data, sizeof(data), MSG_OOB); int error = errno;
    errno = 0; ssize_t queued = recv(p.rx, next, sizeof(next), MSG_PEEK | MSG_DONTWAIT);
    int queue_error = errno;
    printf("UDP_OBSERVE_OOB family=%d n=%zd errno=%d prefix=%02x%02x%02x%02x queue=%zd qerrno=%d qprefix=%02x%02x\n",
           family, n, error, data[0], data[1], data[2], data[3], queued, queue_error, next[0], next[1]);
    close(p.tx); close(p.rx);
  }
}
int main(int argc, char** argv) {
  // Diagnostic-only: report real OOB results without changing default assertions.
  if (argc > 1 && strcmp(argv[1], "--observe-oob") == 0) { observe_oob(); return 0; }
  // Negative fixture: suppress the requested semantics in the actual calls;
  // oversized MSG_TRUNC assertions must fail on Linux and on Gesher.
  if (argc > 1 && strcmp(argv[1], "--negative") == 0) {
    Pair p = pair(AF_INET); send_packet(p, "abcdefghi", 9); char data[4];
    require(recv(p.rx, data, sizeof(data), 0) == 9, "negative missing MSG_TRUNC rejected");
    return 0;
  }
  matrix(AF_INET); matrix(AF_INET6); concurrent(AF_INET); concurrent(AF_INET6);
  boundaries(AF_INET); boundaries(AF_INET6); stream();
  puts("UDP all passed; rows=192; alias=128; stream=1");
  return 0;
}
