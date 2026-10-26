// gesher-base probe (ADR 0045, docs/posix.md): the path calls through the
// path model (other drives as /mnt/<drive>, long and non-ASCII names, rename,
// links, access, fopen), /proc/self and /etc/timezone, inotify (alone and in
// epoll), and the process model (getppid, kill between processes, resource
// limits), built against gesher-base's Linux headers like AOSP code.
//
//   posix_probe                 all checks; prints "all passed" and exits 0
//   posix_probe --timezone      prints the content of /etc/timezone
//   posix_probe --child <mode>  a child for the kill checks (below)
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <windows.h>

#include <string>
#include <vector>

static int fails = 0;
#define CHECK(c)                                                              \
  do {                                                                        \
    if (!(c)) {                                                               \
      printf("FAIL line %d: %s (errno %d)\n", __LINE__, #c, errno);           \
      ++fails;                                                                \
    }                                                                         \
  } while (0)

static std::string ReadAll(const char* path) {
  std::string s;
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return "<" + std::to_string(errno) + ">";
  char buf[4096];
  int n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) s.append(buf, n);
  close(fd);
  return s;
}

static bool PutFile(const std::string& path, const std::string& content) {
  FILE* f = fopen(path.c_str(), "w");
  if (f == nullptr) return false;
  bool ok = fwrite(content.data(), 1, content.size(), f) == content.size();
  return fclose(f) == 0 && ok;
}

static std::string FileContent(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (f == nullptr) return "<none>";
  std::string s;
  char buf[256];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
  fclose(f);
  return s;
}

static bool Exists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

static int RemoveTree(const char* path, const struct stat*, int flag, struct FTW*) {
  return flag == FTW_DP ? rmdir(path) : unlink(path);
}

// --- paths ---------------------------------------------------------------------------

static void Paths(const std::string& dir) {
  // A Hebrew name, and a path past MAX_PATH.
  std::string hebrew = dir + "/\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d.txt";  // shalom
  CHECK(PutFile(hebrew, "hello"));
  CHECK(FileContent(hebrew) == "hello");
  DIR* d = opendir(dir.c_str());
  bool found = false;
  while (d != nullptr) {
    dirent* e = readdir(d);
    if (e == nullptr) break;
    found = found || strcmp(e->d_name, "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d.txt") == 0;
    CHECK(e->d_ino != 0);
  }
  if (d != nullptr) closedir(d);
  CHECK(found);
  CHECK(access(hebrew.c_str(), R_OK | W_OK) == 0);
  CHECK(access(hebrew.c_str(), X_OK) == 0);  // no execute permission on Windows: any file runs
  CHECK(access((dir + "/missing").c_str(), F_OK) == -1 && errno == ENOENT);
  CHECK(access("/dev/null", R_OK | W_OK) == 0);
  CHECK(access("/proc/self/status", R_OK) == 0);

  std::string deep = dir;
  for (int i = 0; i < 12; ++i) {
    deep += "/a-rather-long-directory-name-" + std::to_string(i);
    CHECK(mkdir(deep.c_str(), 0755) == 0);
  }
  CHECK(deep.size() > 300);
  std::string deep_file = deep + "/file.txt";
  CHECK(PutFile(deep_file, "deep"));
  CHECK(FileContent(deep_file) == "deep");
  struct stat st;
  CHECK(stat(deep_file.c_str(), &st) == 0 && st.st_size == 4);
  char* real = realpath(deep_file.c_str(), nullptr);
  CHECK(real != nullptr && strstr(real, "a-rather-long-directory-name-11/file.txt") != nullptr &&
        strchr(real, ':') == nullptr);
  free(real);
  CHECK(rename(deep_file.c_str(), (deep + "/renamed.txt").c_str()) == 0);
  d = opendir(deep.c_str());
  CHECK(d != nullptr);
  if (d != nullptr) closedir(d);
  CHECK(nftw((dir + "/a-rather-long-directory-name-0").c_str(), RemoveTree, 8,
             FTW_DEPTH | FTW_PHYS) == 0);
  CHECK(!Exists(dir + "/a-rather-long-directory-name-0"));

  // rename: replaces atomically, an open file included; a directory replaces
  // an empty one; RENAME_NOREPLACE.
  std::string a = dir + "/a.txt", b = dir + "/b.txt";
  CHECK(PutFile(a, "A") && PutFile(b, "B"));
  int held = open(b.c_str(), O_RDONLY);
  CHECK(held >= 0);
  CHECK(rename(a.c_str(), b.c_str()) == 0);
  CHECK(FileContent(b) == "A" && !Exists(a));
  char c = 0;
  CHECK(read(held, &c, 1) == 1 && c == 'B');  // the old file, still open
  close(held);
  CHECK(PutFile(a, "A2"));
  CHECK(renameat2(AT_FDCWD, a.c_str(), AT_FDCWD, b.c_str(), RENAME_NOREPLACE) == -1 && errno == EEXIST);
  CHECK(rename(b.c_str(), b.c_str()) == 0);
  std::string d1 = dir + "/d1", d2 = dir + "/d2";
  CHECK(mkdir(d1.c_str(), 0755) == 0 && mkdir(d2.c_str(), 0755) == 0);
  CHECK(PutFile(d1 + "/x", "x"));
  CHECK(rename(d1.c_str(), d2.c_str()) == 0 && FileContent(d2 + "/x") == "x");
  CHECK(mkdir(d1.c_str(), 0755) == 0);
  CHECK(rename(d1.c_str(), d2.c_str()) == -1 && errno == ENOTEMPTY);
  CHECK(rename(a.c_str(), d2.c_str()) == -1 && errno == EISDIR);
  CHECK(rename(d1.c_str(), a.c_str()) == -1 && errno == ENOTDIR);
  CHECK(rename(d2.c_str(), (d2 + "/x/inside").c_str()) == -1);
  CHECK(remove(d1.c_str()) == 0 && remove(a.c_str()) == 0 && !Exists(d1) && !Exists(a));
  CHECK(unlinkat(AT_FDCWD, (d2 + "/x").c_str(), 0) == 0 &&
        unlinkat(AT_FDCWD, d2.c_str(), AT_REMOVEDIR) == 0);
  CHECK(rmdir(b.c_str()) == -1 && errno == ENOTDIR);

  // chmod: the owner's write permission is the read-only attribute.
  CHECK(chmod(b.c_str(), 0444) == 0 && stat(b.c_str(), &st) == 0 && (st.st_mode & 0666) == 0444 &&
        (st.st_mode & 0111) == 0111);
  CHECK(access(b.c_str(), W_OK) == -1 && errno == EACCES);
  CHECK(open(b.c_str(), O_WRONLY) == -1 && errno == EACCES);
  CHECK(chmod(b.c_str(), 0644) == 0 && access(b.c_str(), W_OK) == 0);
  int ro = open((dir + "/created-ro").c_str(), O_CREAT | O_WRONLY, 0444);
  CHECK(ro >= 0 && write(ro, "w", 1) == 1);  // the fd may write, as on Linux
  close(ro);
  CHECK(stat((dir + "/created-ro").c_str(), &st) == 0 && (st.st_mode & 0200) == 0);
  CHECK(unlink((dir + "/created-ro").c_str()) == 0);  // read-only files go too

  // fopen modes, with Linux's names.
  FILE* f = fopen((dir + "/f.txt").c_str(), "wx");
  CHECK(f != nullptr);
  if (f != nullptr) fclose(f);
  CHECK(fopen((dir + "/f.txt").c_str(), "wx") == nullptr && errno == EEXIST);
  f = fopen((dir + "/f.txt").c_str(), "a+e");
  CHECK(f != nullptr && fputs("line\r\n", f) >= 0);
  if (f != nullptr) fclose(f);
  CHECK(FileContent(dir + "/f.txt") == "line\r\n");  // binary: no CR translation
  f = fopen("/dev/null", "w");
  CHECK(f != nullptr && fputs("discarded", f) >= 0);
  if (f != nullptr) fclose(f);
  f = fopen("/proc/self/cmdline", "r");
  CHECK(f != nullptr);
  if (f != nullptr) fclose(f);
  CHECK(fopen((dir + "/missing/f").c_str(), "r") == nullptr && errno == ENOENT);

  // Hard and symbolic links.
  CHECK(link(b.c_str(), (dir + "/hard").c_str()) == 0);
  CHECK(stat(b.c_str(), &st) == 0 && st.st_nlink == 2);
  CHECK(unlink((dir + "/hard").c_str()) == 0);
  int sl = symlink("b.txt", (dir + "/link").c_str());
  if (sl == -1 && errno == EPERM) {
    printf("INFO symbolic links: no privilege to create them (EPERM); not checked\n");
  } else {
    CHECK(sl == 0);
    char target[256];
    ssize_t n = readlink((dir + "/link").c_str(), target, sizeof(target));
    CHECK(n == 5 && memcmp(target, "b.txt", 5) == 0);
    CHECK(lstat((dir + "/link").c_str(), &st) == 0 && S_ISLNK(st.st_mode) && st.st_size == 5);
    CHECK(stat((dir + "/link").c_str(), &st) == 0 && S_ISREG(st.st_mode));
    CHECK(FileContent(dir + "/link") == "A");
    CHECK(open((dir + "/link").c_str(), O_RDONLY | O_NOFOLLOW) == -1 && errno == ELOOP);
    CHECK(readlink(b.c_str(), target, sizeof(target)) == -1 && errno == EINVAL);
    CHECK(symlink((dir + "/d-target").c_str(), (dir + "/dlink").c_str()) == 0);
    n = readlink((dir + "/dlink").c_str(), target, sizeof(target));
    CHECK(n > 0 && std::string(target, n) == dir + "/d-target");
    CHECK(unlink((dir + "/dlink").c_str()) == 0);
    struct stat at;
    CHECK(fstatat(AT_FDCWD, (dir + "/link").c_str(), &at, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(at.st_mode));
    CHECK(unlink((dir + "/link").c_str()) == 0 && Exists(b));
  }

  // mkdtemp, mkstemp.
  std::string templ = dir + "/tmp-XXXXXX";
  CHECK(mkdtemp(templ.data()) != nullptr && templ.find("XXXXXX") == std::string::npos);
  CHECK(Exists(templ) && rmdir(templ.c_str()) == 0);
  std::string ftempl = dir + "/f-XXXXXX.dat";
  int tf = mkstemps(ftempl.data(), 4);
  CHECK(tf >= 0 && ftempl.find("XXXXXX") == std::string::npos);
  close(tf);
  CHECK(unlink(ftempl.c_str()) == 0);

  // chdir and getcwd; fchdir.
  char cwd[4096];
  CHECK(getcwd(cwd, sizeof(cwd)) != nullptr);
  CHECK(chdir(dir.c_str()) == 0);
  CHECK(FileContent("b.txt") == "A");
  int dfd = open(cwd, O_RDONLY | O_DIRECTORY);
  CHECK(dfd >= 0 && fchdir(dfd) == 0);
  close(dfd);
  char back[4096];
  CHECK(getcwd(back, sizeof(back)) != nullptr && strcmp(back, cwd) == 0);
  CHECK(unlink(b.c_str()) == 0 && unlink((dir + "/f.txt").c_str()) == 0 &&
        unlink(hebrew.c_str()) == 0);
}

// /mnt/<drive>: a path on another drive, with no colon.
static void OtherDrive() {
  char cwd[4096];
  getcwd(cwd, sizeof(cwd));
  int current = _getdrive();  // 1 = A:
  wchar_t temp[MAX_PATH];
  GetTempPathW(MAX_PATH, temp);
  wchar_t letter = towupper(temp[0]);
  if (temp[1] != L':' || letter - L'A' + 1 == current) {
    printf("INFO /mnt/<drive>: TEMP is on the current drive; no other drive to check\n");
    // The current drive under /mnt is the same file system.
    std::string self = std::string("/mnt/") + static_cast<char>('a' + current - 1) + cwd;
    struct stat a, b;
    CHECK(stat(self.c_str(), &a) == 0 && stat(cwd, &b) == 0 && a.st_ino == b.st_ino);
    return;
  }
  char drive = static_cast<char>(towlower(letter));
  char temp_utf8[MAX_PATH * 3];
  WideCharToMultiByte(CP_UTF8, 0, temp + 2, -1, temp_utf8, sizeof(temp_utf8), nullptr, nullptr);
  std::string dir = std::string("/mnt/") + drive;
  for (char* p = temp_utf8; *p != '\0'; ++p) dir += *p == '\\' ? '/' : *p;
  if (dir.back() == '/') dir.pop_back();
  dir += "/gesher-posix-probe-" + std::to_string(GetCurrentProcessId());
  CHECK(mkdir(dir.c_str(), 0755) == 0);
  CHECK(PutFile(dir + "/x.txt", "other drive"));
  CHECK(FileContent(dir + "/x.txt") == "other drive");
  char* real = realpath((dir + "/x.txt").c_str(), nullptr);
  CHECK(real != nullptr && strncmp(real, "/mnt/", 5) == 0 && real[5] == drive &&
        strchr(real, ':') == nullptr);
  free(real);
  CHECK(chdir(dir.c_str()) == 0);
  char now[4096];
  CHECK(getcwd(now, sizeof(now)) != nullptr && strcmp(now, "/") != 0 && strchr(now, ':') == nullptr);
  CHECK(FileContent("x.txt") == "other drive");
  // Back to where we were: a drive-less path now means the other drive, so
  // the way back names the first drive under /mnt.
  std::string home = std::string("/mnt/") + static_cast<char>('a' + current - 1) + cwd;
  CHECK(chdir(home.c_str()) == 0);
  CHECK(getcwd(now, sizeof(now)) != nullptr && strcmp(now, cwd) == 0);
  CHECK(unlink((dir + "/x.txt").c_str()) == 0 && rmdir(dir.c_str()) == 0);
  printf("INFO /mnt/%c: checked\n", drive);
}

// --- /proc and /etc/timezone -------------------------------------------------------------

static void Proc(const std::string& dir) {
  std::string status = ReadAll("/proc/self/status");
  CHECK(status.find("Pid:\t" + std::to_string(GetCurrentProcessId()) + "\n") != std::string::npos);
  CHECK(status.find("PPid:\t" + std::to_string(getppid()) + "\n") != std::string::npos);
  CHECK(status.find("VmRSS:") != std::string::npos && status.find("Threads:\t") != std::string::npos);
  CHECK(status.find("Uid:\t" + std::to_string(getuid())) != std::string::npos);
  CHECK(ReadAll("/proc/self/comm") == "posix_probe\n");
  std::string stat_line = ReadAll("/proc/self/stat");
  CHECK(stat_line.rfind(std::to_string(GetCurrentProcessId()) + " (posix_probe) R ", 0) == 0);
  int fields = 0;
  for (char c : stat_line) fields += c == ' ';
  CHECK(fields == 51);
  std::string maps = ReadAll("/proc/self/maps");
  CHECK(maps.find("posix_probe.exe") != std::string::npos && maps.find(" r-xp ") != std::string::npos);
  CHECK(maps.find("[stack]") != std::string::npos);
  CHECK(ReadAll("/proc/meminfo").rfind("MemTotal:", 0) == 0);
  CHECK(ReadAll("/proc/cpuinfo").find("processor\t: 0\n") != std::string::npos);
  CHECK(ReadAll("/proc/self/cmdline").find("posix_probe") != std::string::npos);

  // /proc/self/fd: the open fds, as links.
  int fd = open((dir + "/fd.txt").c_str(), O_CREAT | O_RDWR, 0644);
  CHECK(fd >= 0);
  bool listed = false;
  DIR* d = opendir("/proc/self/fd");
  CHECK(d != nullptr);
  while (d != nullptr) {
    dirent* e = readdir(d);
    if (e == nullptr) break;
    listed = listed || (atoi(e->d_name) == fd && e->d_type == DT_LNK);
  }
  if (d != nullptr) closedir(d);
  CHECK(listed);
  char target[1024];
  std::string link = "/proc/self/fd/" + std::to_string(fd);
  ssize_t n = readlink(link.c_str(), target, sizeof(target));
  char* real = realpath((dir + "/fd.txt").c_str(), nullptr);
  CHECK(n > 0 && real != nullptr && std::string(target, n) == real);
  free(real);
  struct stat st, fst;
  CHECK(stat(link.c_str(), &st) == 0 && fstat(fd, &fst) == 0 && st.st_ino == fst.st_ino);
  CHECK(lstat(link.c_str(), &st) == 0 && S_ISLNK(st.st_mode));
  close(fd);
  CHECK(readlink(link.c_str(), target, sizeof(target)) == -1 && errno == ENOENT);
  unlink((dir + "/fd.txt").c_str());

  n = readlink("/proc/self/exe", target, sizeof(target));
  CHECK(n > 0 && std::string(target, n).find("posix_probe.exe") != std::string::npos &&
        memchr(target, ':', n) == nullptr);

  // /proc/self/task: the threads; a thread's name.
  std::string tid = std::to_string(GetCurrentThreadId());
  CHECK(Exists("/proc/self/task/" + tid));
  CHECK(ReadAll(("/proc/self/task/" + tid + "/stat").c_str()).rfind(tid + " (", 0) == 0);
  bool own_thread = false;
  d = opendir("/proc/self/task");
  while (d != nullptr) {
    dirent* e = readdir(d);
    if (e == nullptr) break;
    own_thread = own_thread || tid == e->d_name;
  }
  if (d != nullptr) closedir(d);
  CHECK(own_thread);
  CHECK(open("/proc/self/status", O_WRONLY) == -1 && errno == EACCES);
  CHECK(unlink("/proc/self/status") == -1);

  // /etc/timezone: Windows' zone as an IANA id.
  std::string tz = ReadAll("/etc/timezone");
  CHECK(!tz.empty() && tz.find('\n') == std::string::npos && tz[0] != '<');
  CHECK(stat("/etc/timezone", &st) == 0 && S_ISREG(st.st_mode));
}

// --- inotify ---------------------------------------------------------------------

struct Event {
  int wd;
  uint32_t mask;
  uint32_t cookie;
  std::string name;
};

// Reads events until `want` is seen (or 3 s pass), appending all to `seen`.
static bool WaitFor(int in, uint32_t want, const std::string& name, std::vector<Event>* seen) {
  for (int i = 0; i < 60; ++i) {
    for (const Event& e : *seen) {
      if ((e.mask & want) == want && e.name == name) return true;
    }
    pollfd p = {in, POLLIN, 0};
    if (poll(&p, 1, 50) != 1) continue;
    alignas(8) char buf[4096];
    int n = read(in, buf, sizeof(buf));
    for (int off = 0; n > 0 && off < n;) {
      auto* e = reinterpret_cast<inotify_event*>(buf + off);
      seen->push_back({e->wd, e->mask, e->cookie, e->len > 0 ? std::string(e->name) : ""});
      off += sizeof(inotify_event) + e->len;
    }
  }
  printf("  waiting for 0x%x '%s', saw:", static_cast<unsigned>(want), name.c_str());
  for (const Event& e : *seen) printf(" [%d 0x%x %u '%s']", e.wd, static_cast<unsigned>(e.mask), e.cookie, e.name.c_str());
  printf("\n");
  return false;
}

static void Inotify(const std::string& dir) {
  int in = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  CHECK(in >= 0);
  char buf[256];
  CHECK(read(in, buf, sizeof(buf)) == -1 && errno == EAGAIN);
  std::string watched = dir + "/watched";
  CHECK(mkdir(watched.c_str(), 0755) == 0);
  int wd = inotify_add_watch(in, watched.c_str(), IN_ALL_EVENTS);
  CHECK(wd > 0);
  CHECK(inotify_add_watch(in, watched.c_str(), IN_MODIFY) == wd);  // the same inode
  CHECK(inotify_add_watch(in, watched.c_str(), IN_ALL_EVENTS) == wd);
  CHECK(inotify_add_watch(in, (dir + "/nowhere").c_str(), IN_ALL_EVENTS) == -1 && errno == ENOENT);

  // In an epoll, as Looper and FileObserver wait.
  int ep = epoll_create1(EPOLL_CLOEXEC);
  epoll_event ev = {};
  ev.events = EPOLLIN;
  ev.data.u64 = 42;
  CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, in, &ev) == 0);
  CHECK(epoll_wait(ep, &ev, 1, 0) == 0);

  std::vector<Event> seen;
  std::string file = watched + "/new.txt";
  int fd = open(file.c_str(), O_CREAT | O_WRONLY, 0644);
  CHECK(fd >= 0);
  epoll_event out = {};
  CHECK(epoll_wait(ep, &out, 1, 3000) == 1 && out.data.u64 == 42 && (out.events & EPOLLIN) != 0);
  CHECK(WaitFor(in, IN_CREATE, "new.txt", &seen));
  // NTFS reports a write when it updates the directory entry, which may
  // wait until the file is closed (docs/posix.md).
  CHECK(write(fd, "data", 4) == 4);
  close(fd);
  CHECK(WaitFor(in, IN_MODIFY, "new.txt", &seen));
  CHECK(WaitFor(in, IN_CLOSE_WRITE, "new.txt", &seen));
  CHECK(rename(file.c_str(), (watched + "/moved.txt").c_str()) == 0);
  CHECK(WaitFor(in, IN_MOVED_FROM, "new.txt", &seen) && WaitFor(in, IN_MOVED_TO, "moved.txt", &seen));
  uint32_t from_cookie = 0, to_cookie = 1;
  for (const Event& e : seen) {
    if (e.mask & IN_MOVED_FROM) from_cookie = e.cookie;
    if (e.mask & IN_MOVED_TO) to_cookie = e.cookie;
  }
  CHECK(from_cookie != 0 && from_cookie == to_cookie);
  CHECK(chmod((watched + "/moved.txt").c_str(), 0444) == 0);
  CHECK(WaitFor(in, IN_ATTRIB, "moved.txt", &seen));
  CHECK(unlink((watched + "/moved.txt").c_str()) == 0);
  CHECK(WaitFor(in, IN_DELETE, "moved.txt", &seen));
  CHECK(mkdir((watched + "/sub").c_str(), 0755) == 0);
  CHECK(WaitFor(in, IN_CREATE | IN_ISDIR, "sub", &seen));
  CHECK(rmdir((watched + "/sub").c_str()) == 0);
  CHECK(WaitFor(in, IN_DELETE | IN_ISDIR, "sub", &seen));
  for (const Event& e : seen) CHECK(e.wd == wd);

  // A watched file: its own events, without a name; IN_ONESHOT.
  std::string single = watched + "/single.txt";
  CHECK(PutFile(single, "1"));
  int fwd = inotify_add_watch(in, single.c_str(), IN_MODIFY | IN_ONESHOT);
  CHECK(fwd > 0 && fwd != wd);
  seen.clear();
  CHECK(PutFile(single, "22"));
  CHECK(WaitFor(in, IN_MODIFY, "", &seen));
  CHECK(WaitFor(in, IN_IGNORED, "", &seen));
  CHECK(inotify_rm_watch(in, fwd) == -1 && errno == EINVAL);
  CHECK(unlink(single.c_str()) == 0);

  // inotify_rm_watch: IN_IGNORED; then the watched directory goes.
  seen.clear();
  CHECK(inotify_rm_watch(in, wd) == 0);
  CHECK(WaitFor(in, IN_IGNORED, "", &seen));
  int wd2 = inotify_add_watch(in, watched.c_str(), IN_DELETE_SELF | IN_ONLYDIR);
  CHECK(wd2 > 0);
  CHECK(inotify_add_watch(in, (dir + "/probe.txt").c_str(), IN_MODIFY | IN_ONLYDIR) == -1);
  seen.clear();
  CHECK(rmdir(watched.c_str()) == 0);
  CHECK(WaitFor(in, IN_DELETE_SELF, "", &seen));
  CHECK(WaitFor(in, IN_IGNORED, "", &seen));
  close(ep);
  close(in);

  // A blocking read waits for an event.
  in = inotify_init();
  CHECK(in >= 0 && inotify_add_watch(in, dir.c_str(), IN_CREATE) > 0);
  HANDLE writer = CreateThread(
      nullptr, 0,
      [](void* arg) -> DWORD {
        Sleep(200);
        PutFile(*static_cast<std::string*>(arg) + "/late.txt", "x");
        return 0;
      },
      const_cast<std::string*>(&dir), 0, nullptr);
  alignas(8) char ebuf[512];
  int n = read(in, ebuf, sizeof(ebuf));
  CHECK(n > 0 && strcmp(reinterpret_cast<inotify_event*>(ebuf)->name, "late.txt") == 0);
  WaitForSingleObject(writer, INFINITE);
  CloseHandle(writer);
  close(in);
  unlink((dir + "/late.txt").c_str());
}

// --- process model --------------------------------------------------------------------

static int RunChild(const char* mode) {
  if (strcmp(mode, "sigwait") == 0) {
    // Takes SIGUSR1 as ART's signal catcher does; exits 7.
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    printf("ready\n");
    fflush(stdout);
    int sig = 0;
    sigwait(&set, &sig);
    return sig == SIGUSR1 ? 7 : 1;
  }
  if (strcmp(mode, "ignore") == 0) {
    struct sigaction ignore = {};
    ignore.sa_handler = SIG_IGN;
    gesher_sigaction_raw(SIGTERM, &ignore, nullptr);  // sigaction, without a signal chain
    printf("ready\n");
    fflush(stdout);
    Sleep(1500);  // SIGTERM is ignored: still running at the end
    return 9;
  }
  if (strcmp(mode, "uid") == 0) return static_cast<int>(getuid() % 256);
  if (strcmp(mode, "ppid") == 0) return getppid() == atoi(getenv("PROBE_PARENT")) ? 0 : 1;
  printf("ready\n");
  fflush(stdout);
  Sleep(30000);  // "sleep": killed before this ends
  return 0;
}

struct Child {
  HANDLE process = nullptr;
  HANDLE out = nullptr;
  DWORD pid = 0;
};

static Child Start(const char* mode, const wchar_t* env_uid = nullptr) {
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --child ";
  for (const char* p = mode; *p != '\0'; ++p) cmd += static_cast<wchar_t>(*p);
  SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
  HANDLE read_end, write_end;
  CreatePipe(&read_end, &write_end, &sa, 0);
  SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = write_end;
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION pi = {};
  SetEnvironmentVariableW(L"PROBE_PARENT", std::to_wstring(GetCurrentProcessId()).c_str());
  SetEnvironmentVariableW(L"GESHER_UID", env_uid);
  Child c;
  if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    c.process = pi.hProcess;
    c.pid = pi.dwProcessId;
  }
  SetEnvironmentVariableW(L"GESHER_UID", nullptr);
  CloseHandle(write_end);
  c.out = read_end;
  return c;
}

static bool WaitReady(const Child& c) {
  std::string line;
  char ch;
  DWORD n = 0;
  while (line.size() < 16 && ReadFile(c.out, &ch, 1, &n, nullptr) && n == 1 && ch != '\n') line += ch;
  if (line.rfind("ready", 0) == 0) return true;
  printf("  child said '%s' (error %lu)\n", line.c_str(), GetLastError());
  return false;
}

static DWORD Finish(Child& c, DWORD timeout_ms = 10000) {
  DWORD code = 0xDEAD;
  if (WaitForSingleObject(c.process, timeout_ms) == WAIT_OBJECT_0) GetExitCodeProcess(c.process, &code);
  else TerminateProcess(c.process, 1);
  CloseHandle(c.process);
  CloseHandle(c.out);
  return code;
}

static void Processes() {
  // getppid: the process that started this one (a child checks it).
  Child c = Start("ppid");
  CHECK(c.process != nullptr && Finish(c) == 0);
  // GESHER_UID: a process's uid, when its starter gives one.
  c = Start("uid", L"1000");
  CHECK(Finish(c) == 1000 % 256);
  c = Start("uid");
  CHECK(Finish(c) == 10000 % 256);

  // kill to another Gesher process: SIGUSR1 to a sigwait, SIGTERM's default
  // and SIG_IGN, SIGKILL, signal 0, SIGSTOP/SIGCONT.
  c = Start("sigwait");
  CHECK(WaitReady(c));
  CHECK(kill(static_cast<pid_t>(c.pid), 0) == 0);
  CHECK(kill(static_cast<pid_t>(c.pid), SIGUSR1) == 0);
  CHECK(Finish(c) == 7);
  c = Start("sleep");
  CHECK(WaitReady(c));
  CHECK(kill(static_cast<pid_t>(c.pid), SIGTERM) == 0);
  CHECK(Finish(c) == 128 + SIGTERM);
  c = Start("ignore");
  CHECK(WaitReady(c));
  CHECK(kill(static_cast<pid_t>(c.pid), SIGTERM) == 0);
  CHECK(Finish(c) == 9);
  c = Start("sleep");
  CHECK(WaitReady(c));
  CHECK(kill(static_cast<pid_t>(c.pid), SIGSTOP) == 0 && kill(static_cast<pid_t>(c.pid), SIGCONT) == 0);
  CHECK(kill(static_cast<pid_t>(c.pid), SIGKILL) == 0);
  CHECK(Finish(c) == 128 + SIGKILL);
  CHECK(kill(static_cast<pid_t>(c.pid), 0) == -1 && errno == ESRCH);
  CHECK(kill(-1, SIGTERM) == -1 && errno == EPERM);

  // Resource limits agree with sysconf, and lower and rise within the hard limit.
  rlimit rl;
  CHECK(getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur == static_cast<rlim_t>(sysconf(_SC_OPEN_MAX)));
  CHECK(getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur >= 1024 * 1024);
  CHECK(getrlimit(RLIMIT_CORE, &rl) == 0 && rl.rlim_cur == 0);
  CHECK(getrlimit(99, &rl) == -1 && errno == EINVAL);
  rlimit lower = {1024, 8192};
  CHECK(setrlimit(RLIMIT_NOFILE, &lower) == 0 && getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur == 1024);
  rlimit higher = {8192, 100000};
  CHECK(setrlimit(RLIMIT_NOFILE, &higher) == -1 && errno == EPERM);
  rlimit bad = {9000, 8192};
  CHECK(setrlimit(RLIMIT_NOFILE, &bad) == -1 && errno == EINVAL);
  rlimit back = {8192, 8192};
  CHECK(setrlimit(RLIMIT_NOFILE, &back) == 0);
  rusage ru;
  CHECK(getrusage(RUSAGE_SELF, &ru) == 0 && ru.ru_maxrss > 0);
  CHECK(getrusage(42, &ru) == -1 && errno == EINVAL);
  CHECK(sysconf(_SC_PAGESIZE) == 4096 && sysconf(_SC_NPROCESSORS_ONLN) >= 1);
  CHECK(sysconf(_SC_PHYS_PAGES) > 0 && sysconf(_SC_CLK_TCK) == 100);

  // libcap: no capabilities.
  cap_t caps = cap_get_proc();
  cap_flag_value_t value = CAP_SET;
  CHECK(caps != nullptr && cap_get_flag(caps, CAP_SYS_ADMIN, CAP_EFFECTIVE, &value) == 0 &&
        value == CAP_CLEAR);
  CHECK(cap_set_proc(caps) == 0);
  cap_value_t admin = CAP_SYS_ADMIN;
  CHECK(cap_set_flag(caps, CAP_EFFECTIVE, 1, &admin, CAP_SET) == 0 && cap_set_proc(caps) == -1 &&
        errno == EPERM);
  cap_free(caps);
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc == 3 && strcmp(argv[1], "--child") == 0) return RunChild(argv[2]);
  if (argc == 2 && strcmp(argv[1], "--timezone") == 0) {
    printf("%s\n", ReadAll("/etc/timezone").c_str());
    return 0;
  }
  char cwd[4096];
  CHECK(getcwd(cwd, sizeof(cwd)) != nullptr);
  std::string dir = std::string(cwd) + "/posix-probe-" + std::to_string(GetCurrentProcessId());
  CHECK(mkdir(dir.c_str(), 0755) == 0);
  fprintf(stderr, "-- %s\n", "Paths");
  Paths(dir);
  fprintf(stderr, "-- %s\n", "OtherDrive");
  OtherDrive();
  fprintf(stderr, "-- %s\n", "Proc");
  Proc(dir);
  fprintf(stderr, "-- %s\n", "Inotify");
  Inotify(dir);
  fprintf(stderr, "-- %s\n", "Processes");
  Processes();
  CHECK(nftw(dir.c_str(), RemoveTree, 8, FTW_DEPTH | FTW_PHYS) == 0 && !Exists(dir));
  if (fails == 0) {
    printf("all passed\n");
    return 0;
  }
  printf("%d failures\n", fails);
  return 1;
}
