// gesher-base probe: the socket layer and the fd overrides (close, read,
// write, dup, dup2, poll), built against gesher-base's Linux headers like
// AOSP code. Prints "all passed" and exits 0 when every check holds.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <mntent.h>
#include <poll.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/xattr.h>
#include <termios.h>
#include <unistd.h>
#include <windows.h>

#include <atomic>
#include <thread>

static int fails = 0;
#define CHECK(c)                                                              \
  do {                                                                        \
    if (!(c)) {                                                               \
      printf("FAIL line %d: %s (errno %d)\n", __LINE__, #c, errno);           \
      ++fails;                                                                \
    }                                                                         \
  } while (0)

static sockaddr_in Loopback(unsigned short port) {
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(0x7F000001);
  return a;
}

int main(int, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  // Bad fds fail instead of ending the process.
  CHECK(close(-1) == -1 && errno == EBADF);
  CHECK(close(12345) == -1 && errno == EBADF);

  // socketpair, read/write/close on socket fds.
  int sv[2];
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  CHECK(gesher_is_socket(sv[0]) == 1 && gesher_is_socket(0) == 0);
  CHECK(write(sv[0], "ping", 4) == 4);
  char buf[64] = {};
  CHECK(read(sv[1], buf, sizeof(buf)) == 4 && memcmp(buf, "ping", 4) == 0);

  // poll: times out, then sees data and FIONREAD counts it.
  pollfd p = {sv[1], POLLIN, 0};
  DWORD t0 = GetTickCount();
  CHECK(poll(&p, 1, 50) == 0 && p.revents == 0);
  CHECK(GetTickCount() - t0 >= 40);
  CHECK(send(sv[0], "hello", 5, 0) == 5);
  CHECK(poll(&p, 1, 1000) == 1 && (p.revents & POLLIN) != 0);
  int avail = -1;
  CHECK(ioctl(sv[1], FIONREAD, &avail) == 0 && avail == 5);
  iovec iov[2] = {{buf, 2}, {buf + 2, 3}};
  CHECK(readv(sv[1], iov, 2) == 5 && memcmp(buf, "hello", 5) == 0);

  // Non-blocking via fcntl, and MSG_DONTWAIT on a blocking socket.
  CHECK(recv(sv[1], buf, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
  int fl = fcntl(sv[1], F_GETFL);
  CHECK(fcntl(sv[1], F_SETFL, fl | O_NONBLOCK) == 0);
  CHECK((fcntl(sv[1], F_GETFL) & O_NONBLOCK) != 0);
  CHECK(read(sv[1], buf, 1) == -1 && errno == EAGAIN);
  CHECK(fcntl(sv[1], F_SETFL, fl) == 0);

  // dup: the duplicate reaches the same peer.
  int d = dup(sv[0]);
  CHECK(d >= 0 && gesher_is_socket(d) == 1);
  CHECK(write(d, "dup", 3) == 3);
  CHECK(recv(sv[1], buf, sizeof(buf), 0) == 3 && memcmp(buf, "dup", 3) == 0);
  CHECK(close(d) == 0);

  // SO_REUSEADDR is recorded; SO_TYPE, SO_DOMAIN are Linux values.
  int one = 1, v = 0;
  socklen_t vl = sizeof(v);
  CHECK(setsockopt(sv[0], SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);
  CHECK(getsockopt(sv[0], SOL_SOCKET, SO_REUSEADDR, &v, &vl) == 0 && v == 1);
  vl = sizeof(v);
  CHECK(getsockopt(sv[0], SOL_SOCKET, SO_TYPE, &v, &vl) == 0 && v == SOCK_STREAM);
  vl = sizeof(v);
  CHECK(getsockopt(sv[0], SOL_SOCKET, SO_DOMAIN, &v, &vl) == 0 && v == AF_INET);
  vl = sizeof(v);
  CHECK(setsockopt(sv[0], IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0);
  CHECK(getsockopt(sv[0], IPPROTO_TCP, TCP_NODELAY, &v, &vl) == 0 && v != 0);
  CHECK(getsockopt(sv[0], SOL_SOCKET, SO_PEERCRED, &v, &vl) == -1 && errno == ENOPROTOOPT);

  // SO_RCVTIMEO expires with EAGAIN, as on Linux.
  timeval tv = {0, 100000};
  CHECK(setsockopt(sv[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
  timeval tv2 = {};
  socklen_t tl = sizeof(tv2);
  CHECK(getsockopt(sv[1], SOL_SOCKET, SO_RCVTIMEO, &tv2, &tl) == 0 && tv2.tv_usec == 100000);
  t0 = GetTickCount();
  CHECK(recv(sv[1], buf, 1, 0) == -1 && errno == EAGAIN);
  CHECK(GetTickCount() - t0 >= 80);
  CHECK(close(sv[0]) == 0 && close(sv[1]) == 0);
  CHECK(close(sv[0]) == -1 && errno == EBADF);

  // TCP: non-blocking connect, accept, names.
  int ls = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_in a = Loopback(0);
  CHECK(bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
  CHECK(listen(ls, 4) == 0);
  socklen_t al = sizeof(a);
  CHECK(getsockname(ls, reinterpret_cast<sockaddr*>(&a), &al) == 0 && a.sin_family == AF_INET &&
        a.sin_port != 0);
  int c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  int rc = connect(c, reinterpret_cast<sockaddr*>(&a), sizeof(a));
  CHECK(rc == 0 || errno == EINPROGRESS);
  pollfd cp = {c, POLLOUT, 0};
  CHECK(poll(&cp, 1, 2000) == 1 && (cp.revents & POLLOUT) != 0);
  int err = -1;
  vl = sizeof(err);
  CHECK(getsockopt(c, SOL_SOCKET, SO_ERROR, &err, &vl) == 0 && err == 0);
  sockaddr_in peer = {};
  socklen_t pl = sizeof(peer);
  int s = accept(ls, reinterpret_cast<sockaddr*>(&peer), &pl);
  CHECK(s >= 0 && pl == sizeof(sockaddr_in) && peer.sin_family == AF_INET);
  sockaddr_in cname = {};
  socklen_t cl = sizeof(cname);
  CHECK(getsockname(c, reinterpret_cast<sockaddr*>(&cname), &cl) == 0 &&
        cname.sin_port == peer.sin_port);
  // An accepted socket is blocking, whatever the listener.
  CHECK((fcntl(s, F_GETFL) & O_NONBLOCK) == 0);
  // Shutdown: the peer sees end of stream, and poll reports it readable.
  CHECK(shutdown(c, SHUT_WR) == 0);
  pollfd sp = {s, POLLIN, 0};
  CHECK(poll(&sp, 1, 2000) == 1 && (sp.revents & POLLIN) != 0);
  CHECK(read(s, buf, sizeof(buf)) == 0);
  close(c);
  close(s);

  // A refused connection.
  sockaddr_in closed = a;
  close(ls);
  c = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(connect(c, reinterpret_cast<sockaddr*>(&closed), sizeof(closed)) == -1 &&
        errno == ECONNREFUSED);
  close(c);

  // libcore's preClose0: dup2 onto a socket wakes a thread blocked on it.
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  std::atomic<int> result{1}, error{0};
  std::thread reader([&] {
    char b;
    int n = static_cast<int>(recv(sv[1], &b, 1, 0));
    error = errno;
    result = n;
  });
  Sleep(100);
  int null_fd = open("NUL", O_RDWR);
  CHECK(null_fd >= 0);
  CHECK(dup2(null_fd, sv[1]) == sv[1]);
  CHECK(gesher_is_socket(sv[1]) == 0);
  reader.join();
  CHECK(result == -1 || result == 0);
  close(null_fd);
  close(sv[1]);
  close(sv[0]);

  // Datagram pair, sendto/recvfrom with addresses.
  CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);
  CHECK(send(sv[0], "dgram", 5, 0) == 5);
  sockaddr_storage from = {};
  socklen_t fl2 = sizeof(from);
  CHECK(recvfrom(sv[1], buf, 2, 0, reinterpret_cast<sockaddr*>(&from), &fl2) == 2 &&
        from.ss_family == AF_INET);  // truncated datagram: the part that fit
  close(sv[0]);
  close(sv[1]);

  // Names.
  addrinfo hints = {}, *res = nullptr;
  hints.ai_socktype = SOCK_STREAM;
  CHECK(getaddrinfo("localhost", "80", &hints, &res) == 0 && res != nullptr);
  for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
    CHECK(ai->ai_family == AF_INET || ai->ai_family == AF_INET6);
    CHECK(ai->ai_socktype == SOCK_STREAM && ai->ai_addr->sa_family == ai->ai_family);
    if (ai->ai_family == AF_INET6) CHECK(ai->ai_addrlen == sizeof(sockaddr_in6));
  }
  freeaddrinfo(res);
  hints.ai_flags = AI_NUMERICHOST;
  CHECK(getaddrinfo("not an address", nullptr, &hints, &res) == EAI_NONAME);
  CHECK(strcmp(gai_strerror(EAI_NONAME), "Name or service not known") == 0);
  sockaddr_in lb = Loopback(443);
  char host[NI_MAXHOST], serv[NI_MAXSERV];
  CHECK(getnameinfo(reinterpret_cast<sockaddr*>(&lb), sizeof(lb), host, sizeof(host), serv,
                    sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV) == 0 &&
        strcmp(host, "127.0.0.1") == 0 && strcmp(serv, "443") == 0);

  in6_addr a6;
  char text[INET6_ADDRSTRLEN];
  CHECK(inet_pton(AF_INET6, "::1", &a6) == 1 && inet_ntop(AF_INET6, &a6, text, sizeof(text)) &&
        strcmp(text, "::1") == 0);
  CHECK(inet_pton(AF_INET, "1.2.3", &a6) == 0);
  CHECK(inet_pton(12345, "1.2.3.4", &a6) == -1 && errno == EAFNOSUPPORT);
  in_addr a4;
  CHECK(inet_aton("127.1", &a4) == 1 && a4.s_addr == htonl(0x7F000001));
  CHECK(inet_aton("0x7f.0.0.1", &a4) == 1 && a4.s_addr == htonl(0x7F000001));
  CHECK(inet_aton("256.0.0.1", &a4) == 0 && inet_aton(" 1.2.3.4", &a4) == 0);
  CHECK(inet_aton("4294967296", &a4) == 0 && inet_aton("4294967295", &a4) == 1);
  CHECK(inet_addr("bad") == INADDR_NONE);
  a4.s_addr = htonl(0x0A000102);
  CHECK(strcmp(inet_ntoa(a4), "10.0.1.2") == 0);

  // Interfaces: loopback with an AF_PACKET entry; names round-trip.
  ifaddrs* ifs = nullptr;
  CHECK(getifaddrs(&ifs) == 0 && ifs != nullptr);
  bool loopback_packet = false, loopback_v4 = false;
  for (ifaddrs* i = ifs; i != nullptr; i = i->ifa_next) {
    CHECK(strlen(i->ifa_name) < IFNAMSIZ);
    if ((i->ifa_flags & IFF_LOOPBACK) == 0 || i->ifa_addr == nullptr) continue;
    if (i->ifa_addr->sa_family == AF_PACKET) loopback_packet = true;
    if (i->ifa_addr->sa_family == AF_INET) {
      auto* in = reinterpret_cast<sockaddr_in*>(i->ifa_addr);
      auto* mask = reinterpret_cast<sockaddr_in*>(i->ifa_netmask);
      if (in->sin_addr.s_addr == htonl(0x7F000001)) {
        loopback_v4 = true;
        CHECK(mask != nullptr && mask->sin_addr.s_addr == htonl(0xFF000000));
        unsigned idx = if_nametoindex(i->ifa_name);
        char name[IFNAMSIZ];
        CHECK(idx != 0 && if_indextoname(idx, name) && strcmp(name, i->ifa_name) == 0);
      }
    }
  }
  CHECK(loopback_packet && loopback_v4);
  freeifaddrs(ifs);
  CHECK(if_nametoindex("no-such-if") == 0);
  struct if_nameindex* ni = if_nameindex();
  CHECK(ni != nullptr && ni[0].if_name != nullptr);
  if_freenameindex(ni);

  // FIONREAD and poll on a pipe, FIONREAD on a file.
  int pp[2];
  CHECK(pipe(pp) == 0);
  pollfd pipe_poll = {pp[0], POLLIN, 0};
  CHECK(poll(&pipe_poll, 1, 30) == 0);
  CHECK(write(pp[1], "abc", 3) == 3);
  CHECK(poll(&pipe_poll, 1, 1000) == 1 && (pipe_poll.revents & POLLIN) != 0);
  CHECK(ioctl(pp[0], FIONREAD, &avail) == 0 && avail == 3);
  close(pp[0]);
  close(pp[1]);
  int f = open(argv[0], O_RDONLY);
  char head[10];
  CHECK(f >= 0 && read(f, head, 10) == 10);
  CHECK(ioctl(f, FIONREAD, &avail) == 0 && avail > 0);
  CHECK(ioctl(f, 0x5401 /* TCGETS */, &avail) == -1 && errno == ENOTTY);
  close(f);

  // isatty: neither a socket nor NUL is a terminal.
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  CHECK(isatty(sv[0]) == 0 && errno == ENOTTY);
  null_fd = open("NUL", O_RDWR);
  CHECK(isatty(null_fd) == 0);
  termios tio;
  CHECK(tcgetattr(null_fd, &tio) == -1 && errno == ENOTTY);

  // sendfile from a file at an offset, into a socket.
  f = open(argv[0], O_RDONLY);
  off_t off = 2;
  CHECK(sendfile(sv[0], f, &off, 8) == 8 && off == 10);
  CHECK(recv(sv[1], buf, 8, MSG_WAITALL) == 8 && memcmp(buf, head + 2, 8) == 0);
  close(f);
  close(null_fd);
  close(sv[0]);
  close(sv[1]);

  // Users, prctl, capabilities, xattr, mount table.
  passwd* pw = getpwuid(getuid());
  CHECK(pw != nullptr && strcmp(pw->pw_name, "u0_a0") == 0 && pw->pw_uid == getuid());
  CHECK(getpwnam("nobody-here") == nullptr);
  passwd pwd;
  passwd* pr;
  char small[8];
  CHECK(getpwuid_r(getuid(), &pwd, small, sizeof(small), &pr) == ERANGE && pr == nullptr);
  group* gr = getgrgid(getgid());
  CHECK(gr != nullptr && gr->gr_mem != nullptr && gr->gr_mem[0] == nullptr);
  CHECK(prctl(PR_SET_NAME, "gesher-probe-thread-name") == 0);
  char tname[16] = {};
  CHECK(prctl(PR_GET_NAME, tname) == 0 && strcmp(tname, "gesher-probe-th") == 0);
  __user_cap_header_struct ch = {_LINUX_CAPABILITY_VERSION_3, 0};
  __user_cap_data_struct cd[2];
  memset(cd, 0xFF, sizeof(cd));
  CHECK(capget(&ch, cd) == 0 && cd[0].effective == 0 && cd[1].permitted == 0);
  CHECK(getxattr("x", "user.a", buf, sizeof(buf)) == -1 && errno == ENOTSUP);
  FILE* mounts = setmntent("/proc/mounts", "r");
  CHECK(mounts != nullptr && getmntent(mounts) == nullptr);
  endmntent(mounts);

  printf(fails != 0 ? "%d failures\n" : "all passed\n", fails);
  return fails != 0;
}
