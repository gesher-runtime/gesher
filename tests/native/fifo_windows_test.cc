// POSIX FIFO (mkfifo / mknod / S_IFIFO / open / read / write / poll / epoll / fcntl / unlink)
// contracts on Windows through gesher-base.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

namespace {

int g_assertions = 0;
int g_failures = 0;

void Check(const char* name, bool ok) {
  ++g_assertions;
  if (!ok) {
    ++g_failures;
    std::fprintf(stderr, "fifo-host: FAIL %s (errno=%d)\n", name, errno);
  } else {
    std::printf("fifo-host: PASS %s\n", name);
  }
}

std::string MakeTempDir() {
  wchar_t temp[MAX_PATH];
  DWORD n = GetTempPathW(MAX_PATH, temp);
  if (n == 0 || n >= MAX_PATH) return {};
  std::wstring dir = std::wstring(temp, n) + L"gesher-fifo-test-" +
                     std::to_wstring(GetCurrentProcessId());
  CreateDirectoryW(dir.c_str(), nullptr);
  int utf8_len = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (utf8_len <= 1) return {};
  std::string out(static_cast<size_t>(utf8_len - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, out.data(), utf8_len, nullptr, nullptr);
  return out;
}

void RemoveTempDir(const std::string& utf8) {
  int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
  if (wlen <= 1) return;
  std::wstring dir(static_cast<size_t>(wlen - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, dir.data(), wlen);
  RemoveDirectoryW(dir.c_str());
}

}  // namespace

int main() {
  const std::string dir = MakeTempDir();
  Check("create temp dir", !dir.empty());
  if (dir.empty()) return 1;

  const std::string fifo_path = dir + "/realm.cv";
  const std::string mknod_path = dir + "/mknod.fifo";
  const std::string reg16_path = dir + "/ordinary16.txt";

  // 1. mkfifo creation, EEXIST, ENOENT, mknod(S_IFIFO), stat/lstat.
  errno = 0;
  Check("mkfifo nullptr -> EFAULT", mkfifo(nullptr, 0600) == -1 && errno == EFAULT);
  errno = 0;
  Check("mkfifo empty -> ENOENT", mkfifo("", 0600) == -1 && errno == ENOENT);
  errno = 0;
  Check("mkfifo missing parent -> ENOENT",
        mkfifo((dir + "/no-such-dir/a.fifo").c_str(), 0600) == -1 && errno == ENOENT);
  Check("mkfifo creates fifo", mkfifo(fifo_path.c_str(), 0600) == 0);
  errno = 0;
  Check("mkfifo duplicate -> EEXIST", mkfifo(fifo_path.c_str(), 0600) == -1 && errno == EEXIST);
  Check("mknod S_IFIFO creates fifo", mknod(mknod_path.c_str(), S_IFIFO | 0600, 0) == 0);

  struct stat st = {};
  Check("stat reports S_ISFIFO and size 0",
        stat(fifo_path.c_str(), &st) == 0 && S_ISFIFO(st.st_mode) && st.st_size == 0);
  std::memset(&st, 0, sizeof(st));
  Check("lstat reports S_ISFIFO and size 0",
        lstat(mknod_path.c_str(), &st) == 0 && S_ISFIFO(st.st_mode) && st.st_size == 0);

  // Ordinary 16-byte file must stay a regular file readable from offset 0.
  {
    int reg = open(reg16_path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    Check("create ordinary 16-byte file", reg >= 0);
    if (reg >= 0) {
      const char payload[17] = "0123456789abcdef";
      Check("write 16 bytes to regular file", write(reg, payload, 16) == 16);
      close(reg);
      std::memset(&st, 0, sizeof(st));
      Check("stat ordinary 16-byte file -> S_ISREG",
            stat(reg16_path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 16);
      reg = open(reg16_path.c_str(), O_RDONLY);
      char readback[17] = {};
      Check("read ordinary 16-byte file from offset 0",
            reg >= 0 && read(reg, readback, 16) == 16 && std::memcmp(readback, payload, 16) == 0);
      if (reg >= 0) close(reg);
    }
    unlink(reg16_path.c_str());
  }

  // 2. O_WRONLY | O_NONBLOCK with no reader fails with ENXIO.
  errno = 0;
  Check("O_WRONLY|O_NONBLOCK without reader -> ENXIO",
        open(fifo_path.c_str(), O_WRONLY | O_NONBLOCK) == -1 && errno == ENXIO);

  // 3. Realm InterprocessCondVar flow: O_RDWR | O_NONBLOCK + poll + epoll + 1-byte notify.
  {
    int fd = open(fifo_path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    Check("open O_RDWR|O_NONBLOCK", fd >= 0);
    if (fd >= 0) {
      std::memset(&st, 0, sizeof(st));
      Check("fstat on open fifo -> S_ISFIFO",
            fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode) && st.st_size == 0);
      Check("O_CLOEXEC honored on fifo", (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
      Check("F_GETFL on O_RDWR|O_NONBLOCK",
            (fcntl(fd, F_GETFL) & (O_ACCMODE | O_NONBLOCK)) == (O_RDWR | O_NONBLOCK));

      char byte = 0;
      errno = 0;
      Check("empty nonblocking read -> EAGAIN", read(fd, &byte, 1) == -1 && errno == EAGAIN);

      struct pollfd pfd = {fd, POLLIN, 0};
      Check("poll empty O_RDWR fifo -> 0", poll(&pfd, 1, 0) == 0 && pfd.revents == 0);

      int ep = epoll_create1(0);
      Check("epoll_create1", ep >= 0);
      struct epoll_event ev = {};
      ev.events = EPOLLIN;
      ev.data.fd = fd;
      Check("epoll_ctl add fifo", ep >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) == 0);

      const char ping = 'P';
      Check("write 1 byte to fifo", write(fd, &ping, 1) == 1);

      int avail = -1;
      Check("ioctl FIONREAD == 1", ioctl(fd, FIONREAD, &avail) == 0 && avail == 1);

      pfd = {fd, POLLIN, 0};
      Check("poll readable fifo -> POLLIN", poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN) != 0);

      struct epoll_event out_ev = {};
      Check("epoll_wait readable fifo -> EPOLLIN",
            ep >= 0 && epoll_wait(ep, &out_ev, 1, 100) == 1 && (out_ev.events & EPOLLIN) != 0);

      Check("read 1 byte from fifo", read(fd, &byte, 1) == 1 && byte == 'P');
      errno = 0;
      Check("drained nonblocking read -> EAGAIN", read(fd, &byte, 1) == -1 && errno == EAGAIN);

      errno = 0;
      Check("pread on fifo -> ESPIPE", pread(fd, &byte, 1, 0) == -1 && errno == ESPIPE);
      errno = 0;
      Check("pwrite on fifo -> ESPIPE", pwrite(fd, &ping, 1, 0) == -1 && errno == ESPIPE);

      if (ep >= 0) close(ep);
      close(fd);
    }
  }

  // 4. Separate reader/writer, fcntl F_SETFL, dup/dup2, EOF/POLLHUP, EPIPE.
  {
    int rfd = open(fifo_path.c_str(), O_RDONLY | O_NONBLOCK);
    int wfd = open(fifo_path.c_str(), O_WRONLY | O_NONBLOCK);
    Check("open separate reader and writer", rfd >= 0 && wfd >= 0);
    if (rfd >= 0 && wfd >= 0) {
      int wdup = dup(wfd);
      Check("dup writer fd", wdup >= 0);
      int rdup = dup2(rfd, rfd + 20);
      Check("dup2 reader fd", rdup == rfd + 20);

      Check("fcntl clear O_NONBLOCK on reader", fcntl(rfd, F_SETFL, 0) == 0 &&
            (fcntl(rfd, F_GETFL) & O_NONBLOCK) == 0);
      Check("fcntl restore O_NONBLOCK on reader", fcntl(rfd, F_SETFL, O_NONBLOCK) == 0 &&
            (fcntl(rfd, F_GETFL) & O_NONBLOCK) != 0);

      const char msg[] = "hello";
      Check("write via dup'd writer", wdup >= 0 && write(wdup, msg, 5) == 5);
      char buf[16] = {};
      Check("read via dup2'd reader", rdup >= 0 && read(rdup, buf, sizeof(buf)) == 5 &&
            std::memcmp(buf, msg, 5) == 0);

      // Closing only one writer keeps writers > 0 -> EAGAIN, not EOF.
      close(wfd);
      wfd = -1;
      errno = 0;
      Check("one writer still open -> EAGAIN", read(rfd, buf, 1) == -1 && errno == EAGAIN);

      // Closing last writer -> EOF (0) and POLLHUP.
      if (wdup >= 0) close(wdup);
      Check("last writer closed -> read EOF", read(rfd, buf, 1) == 0);
      struct pollfd pfd = {rfd, POLLIN, 0};
      Check("last writer closed -> poll POLLHUP",
            poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLHUP) != 0);

      // Open new writer while reader is open, then close all readers -> EPIPE.
      int w2 = open(fifo_path.c_str(), O_WRONLY | O_NONBLOCK);
      Check("re-open writer while reader open", w2 >= 0);
      if (rdup >= 0) close(rdup);
      close(rfd);
      rfd = -1;
      if (w2 >= 0) {
        errno = 0;
        Check("write after all readers closed -> EPIPE",
              write(w2, "x", 1) == -1 && errno == EPIPE);
        struct iovec iov = {const_cast<char*>("x"), 1};
        errno = 0;
        Check("writev after all readers closed -> EPIPE",
              writev(w2, &iov, 1) == -1 && errno == EPIPE);
        close(w2);
      }
    }
    if (rfd >= 0) close(rfd);
    if (wfd >= 0) close(wfd);
  }

  // 5. Blocking open(O_RDONLY) / open(O_WRONLY) rendezvous and blocking read/write across threads.
  {
    std::atomic<bool> writer_done{false};
    int rfd = -1;
    std::thread writer([&] {
      Sleep(30);
      int wfd = open(fifo_path.c_str(), O_WRONLY);
      if (wfd >= 0) {
        Sleep(30);
        const char msg[] = "sync";
        (void)write(wfd, msg, 4);
        close(wfd);
      }
      writer_done.store(true);
    });
    rfd = open(fifo_path.c_str(), O_RDONLY);
    Check("blocking reader rendezvous succeeds", rfd >= 0);
    char buf[8] = {};
    ssize_t n = rfd >= 0 ? read(rfd, buf, sizeof(buf)) : -1;
    Check("blocking read receives writer payload", n == 4 && std::memcmp(buf, "sync", 4) == 0);
    if (rfd >= 0) close(rfd);
    writer.join();
    Check("writer thread completed", writer_done.load());
  }

  // 6. unlink() while open keeps active descriptors working and removes path.
  {
    int fd = open(mknod_path.c_str(), O_RDWR | O_NONBLOCK);
    Check("open mknod fifo before unlink", fd >= 0);
    Check("unlink open fifo succeeds", unlink(mknod_path.c_str()) == 0);
    errno = 0;
    Check("stat unlinked fifo -> ENOENT", stat(mknod_path.c_str(), &st) == -1 && errno == ENOENT);
    if (fd >= 0) {
      const char ch = 'U';
      char out = 0;
      Check("unlinked open fifo still transfers data",
            write(fd, &ch, 1) == 1 && read(fd, &out, 1) == 1 && out == 'U');
      close(fd);
    }
  }

  Check("unlink fifo_path", unlink(fifo_path.c_str()) == 0);
  RemoveTempDir(dir);

  std::printf("fifo-host: %d assertions, %d failures\n", g_assertions, g_failures);
  return g_failures ? 1 : 0;
}
