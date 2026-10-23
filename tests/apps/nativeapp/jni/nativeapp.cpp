// libnativeapp.so, the native code of tests/apps/nativeapp: C++ against the
// NDK's libc++_shared.so, which the APK carries next to it in lib/x86_64, and
// libandroid.so (the platform's: Gesher's bionic shim provides it).
//
// stringFromJNI/callBack: C++ exceptions and calls back into Java.
// platformChecks: the platform APIs typical NDK code uses, each reported as
// name=value (a failure as name=FAIL:reason): an asset read through
// AAssetManager, AConfiguration, ALooper_addFd with a pipe and an eventfd on
// a looper of the library's own thread, ALooper_wake, a socket pair, TCP over
// loopback, getaddrinfo, readdir, fscanf/getline, std::filesystem, signals,
// strftime and sysconf. It also adds a pipe to the main thread's (Java's)
// Looper, whose callback runs when the Java message loop polls next;
// mainLooperResult reports it.

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/configuration.h>
#include <android/looper.h>
#include <android/sensor.h>
#include <android/trace.h>
#include <jni.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

// A C++ exception thrown by libc++ and caught here.
std::string ExceptionCheck() {
  try {
    std::vector<int> v(2);
    return "no exception " + std::to_string(v.at(5));
  } catch (const std::out_of_range&) {
    return "caught std::out_of_range";
  }
}

std::string Fail(const std::string& why) { return "FAIL:" + why; }
std::string Errno(const char* what) { return Fail(std::string(what) + " errno " + std::to_string(errno)); }

// --- assets --------------------------------------------------------------------

std::string CheckAsset(JNIEnv* env, jobject jam, std::string* dir_listing, std::string* config) {
  AAssetManager* am = AAssetManager_fromJava(env, jam);
  if (am == nullptr) return Fail("AAssetManager_fromJava");
  AAsset* a = AAssetManager_open(am, "hello.txt", AASSET_MODE_BUFFER);
  if (a == nullptr) return Fail("open hello.txt");
  off_t len = AAsset_getLength(a);
  const char* buf = static_cast<const char*>(AAsset_getBuffer(a));
  std::string whole = buf != nullptr ? std::string(buf, static_cast<size_t>(len)) : "";
  // Read it again in pieces after a seek.
  AAsset_seek(a, 6, SEEK_SET);
  char part[8] = {};
  int n = AAsset_read(a, part, 4);
  off_t remaining = AAsset_getRemainingLength(a);
  AAsset_close(a);
  if (n != 4 || std::string(part) != "from" || remaining != len - 10) {
    return Fail("seek/read got '" + std::string(part) + "' remaining " + std::to_string(remaining));
  }
  if (AAssetManager_open(am, "missing.txt", AASSET_MODE_STREAMING) != nullptr) {
    return Fail("missing asset opened");
  }
  AAssetDir* dir = AAssetManager_openDir(am, "");
  std::vector<std::string> names;
  if (dir != nullptr) {
    while (const char* name = AAssetDir_getNextFileName(dir)) names.push_back(name);
    AAssetDir_close(dir);
  }
  std::sort(names.begin(), names.end());
  for (const auto& s : names) *dir_listing += (dir_listing->empty() ? "" : "/") + s;
  AConfiguration* c = AConfiguration_new();
  AConfiguration_fromAssetManager(c, am);
  AConfiguration_setOrientation(c, ACONFIGURATION_ORIENTATION_LAND);
  *config = AConfiguration_getOrientation(c) == ACONFIGURATION_ORIENTATION_LAND ? "ok" : Fail("set/get");
  AConfiguration_delete(c);
  while (!whole.empty() && (whole.back() == '\n' || whole.back() == '\r')) whole.pop_back();
  return whole;
}

// --- loopers -------------------------------------------------------------------

struct PipeState {
  int fd = -1;
  int calls = 0;
  int events = 0;
  char got = 0;
};

int OnPipe(int fd, int events, void* data) {
  auto* s = static_cast<PipeState*>(data);
  ++s->calls;
  s->events = events;
  if (read(fd, &s->got, 1) != 1) s->got = '?';
  return 0;  // unregister
}

std::string CheckLooper() {
  if (ALooper_forThread() == nullptr) return Fail("no Looper on the main thread");
  std::string result;
  std::thread t([&] {
    ALooper* looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    if (looper == nullptr || ALooper_forThread() != looper) {
      result = Fail("prepare");
      return;
    }
    int p[2];
    if (pipe(p) != 0) {
      result = Errno("pipe");
      return;
    }
    int efd = eventfd(0, EFD_NONBLOCK);
    if (efd < 0) {
      result = Errno("eventfd");
      return;
    }
    PipeState state;
    state.fd = p[0];
    int marker = 0;
    if (ALooper_addFd(looper, p[0], 0, ALOOPER_EVENT_INPUT, OnPipe, &state) != 1 ||
        ALooper_addFd(looper, efd, 7, ALOOPER_EVENT_INPUT, nullptr, &marker) != 1) {
      result = Fail("addFd");
      return;
    }
    if (write(p[1], "x", 1) != 1 || eventfd_write(efd, 1) != 0) {
      result = Errno("write");
      return;
    }
    bool ident_seen = false;
    for (int i = 0; i < 10 && !(ident_seen && state.calls > 0); ++i) {
      int fd = -1, events = 0;
      void* data = nullptr;
      int r = ALooper_pollOnce(1000, &fd, &events, &data);
      if (r == 7) {
        if (fd != efd || data != &marker || (events & ALOOPER_EVENT_INPUT) == 0) {
          result = Fail("ident 7 fd/data/events");
          return;
        }
        eventfd_t v;
        eventfd_read(efd, &v);
        ident_seen = true;
      } else if (r == ALOOPER_POLL_ERROR) {
        result = Fail("pollOnce error");
        return;
      }
    }
    if (!ident_seen) {
      result = Fail("eventfd ident not returned");
      return;
    }
    if (state.calls != 1 || state.got != 'x' || (state.events & ALOOPER_EVENT_INPUT) == 0) {
      result = Fail("pipe callback calls " + std::to_string(state.calls));
      return;
    }
    ALooper_removeFd(looper, efd);
    // ALooper_wake from another thread ends a long poll.
    ALooper_acquire(looper);
    std::thread waker([looper] {
      usleep(100 * 1000);
      ALooper_wake(looper);
    });
    timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int r = ALooper_pollOnce(10000, nullptr, nullptr, nullptr);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    waker.join();
    ALooper_release(looper);
    if (r != ALOOPER_POLL_WAKE || t1.tv_sec - t0.tv_sec > 5) {
      result = Fail("wake returned " + std::to_string(r));
      return;
    }
    close(p[0]);
    close(p[1]);
    close(efd);
    result = "ok";
  });
  t.join();
  return result;
}

// A pipe on the main thread's Looper (Java's main Looper): its callback runs
// when the Java message loop next polls.
std::atomic<int> g_main_calls{0};
std::atomic<int> g_main_events{0};
int g_main_pipe[2] = {-1, -1};

int OnMainPipe(int fd, int events, void*) {
  char c;
  if (read(fd, &c, 1) == 1 && c == 'm') g_main_calls.fetch_add(1);
  g_main_events = events;
  return 0;
}

std::string ArmMainLooper() {
  ALooper* main = ALooper_forThread();
  if (main == nullptr) return Fail("no main Looper");
  if (pipe(g_main_pipe) != 0) return Errno("pipe");
  if (ALooper_addFd(main, g_main_pipe[0], 0, ALOOPER_EVENT_INPUT, OnMainPipe, nullptr) != 1) {
    return Fail("addFd");
  }
  if (write(g_main_pipe[1], "m", 1) != 1) return Errno("write");
  return "armed";
}

// --- sockets ---------------------------------------------------------------------

std::string CheckSocketpair() {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return Errno("socketpair");
  if (write(sv[0], "ping", 4) != 4) return Errno("write");
  pollfd pfd{sv[1], POLLIN, 0};
  if (poll(&pfd, 1, 5000) != 1 || (pfd.revents & POLLIN) == 0) return Fail("poll");
  char buf[8] = {};
  if (read(sv[1], buf, 4) != 4 || std::strcmp(buf, "ping") != 0) return Fail("read");
  if (send(sv[1], "pong", 4, 0) != 4) return Errno("send");
  fd_set rs;
  FD_ZERO(&rs);
  FD_SET(sv[0], &rs);
  timeval tv{5, 0};
  if (select(sv[0] + 1, &rs, nullptr, nullptr, &tv) != 1 || !FD_ISSET(sv[0], &rs)) {
    return Fail("select");
  }
  std::memset(buf, 0, sizeof(buf));
  if (recv(sv[0], buf, 4, 0) != 4 || std::strcmp(buf, "pong") != 0) return Fail("recv");
  close(sv[0]);
  close(sv[1]);
  return "ok";
}

std::string CheckTcp() {
  int server = socket(AF_INET, SOCK_STREAM, 0);
  if (server < 0) return Errno("socket");
  int one = 1;
  setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return Errno("bind");
  if (listen(server, 1) != 0) return Errno("listen");
  socklen_t len = sizeof(addr);
  if (getsockname(server, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return Errno("getsockname");
  int client = socket(AF_INET, SOCK_STREAM, 0);
  timeval timeout{5, 0};
  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  timeval check{};
  socklen_t check_len = sizeof(check);
  if (getsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &check, &check_len) != 0 || check.tv_sec != 5) {
    return Fail("SO_RCVTIMEO round trip " + std::to_string(check.tv_sec));
  }
  if (connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return Errno("connect");
  int conn = accept(server, nullptr, nullptr);
  if (conn < 0) return Errno("accept");
  const char msg[] = "over tcp";
  if (send(client, msg, sizeof(msg), 0) != static_cast<ssize_t>(sizeof(msg))) return Errno("send");
  char buf[32] = {};
  size_t got = 0;
  while (got < sizeof(msg)) {
    ssize_t r = recv(conn, buf + got, sizeof(buf) - got, 0);
    if (r <= 0) return Errno("recv");
    got += static_cast<size_t>(r);
  }
  close(conn);
  close(client);
  close(server);
  return std::strcmp(buf, msg) == 0 ? "ok" : Fail("data");
}

std::string CheckGetaddrinfo() {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
  addrinfo* res = nullptr;
  int rc = getaddrinfo("127.0.0.1", "8080", &hints, &res);
  if (rc != 0) return Fail(std::string("getaddrinfo ") + gai_strerror(rc));
  std::string out = "ok";
  if (res == nullptr || res->ai_family != AF_INET || res->ai_addr == nullptr) {
    out = Fail("result");
  } else {
    auto* sin = reinterpret_cast<sockaddr_in*>(res->ai_addr);
    char text[32];
    inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text));
    if (ntohs(sin->sin_port) != 8080 || std::strcmp(text, "127.0.0.1") != 0) out = Fail(text);
  }
  freeaddrinfo(res);
  if (getaddrinfo("not a number", nullptr, &hints, &res) != EAI_NONAME) return Fail("EAI_NONAME");
  return out;
}

// --- files ---------------------------------------------------------------------

std::string CheckReaddir(const std::string& base, std::string* scan) {
  std::string dir = base + "/native-readdir";
  mkdir(dir.c_str(), 0700);
  for (const char* name : {"a.txt", "b.txt"}) {
    FILE* f = std::fopen((dir + "/" + name).c_str(), "w");
    if (f == nullptr) return Errno("fopen");
    std::fprintf(f, "42 3.5 word\nsecond line\n");
    std::fclose(f);
  }
  mkdir((dir + "/sub").c_str(), 0700);
  DIR* d = opendir(dir.c_str());
  if (d == nullptr) return Errno("opendir");
  std::vector<std::string> names;
  bool types_ok = true;
  while (dirent* e = readdir(d)) {
    std::string n = e->d_name;
    if (n == "." || n == "..") continue;
    names.push_back(n);
    if ((n == "sub") != (e->d_type == DT_DIR)) types_ok = false;
  }
  closedir(d);
  std::sort(names.begin(), names.end());
  std::string listing;
  for (const auto& s : names) listing += (listing.empty() ? "" : "/") + s;
  if (!types_ok) listing = Fail("d_type " + listing);
  // fscanf and getline on one of the files.
  FILE* f = std::fopen((dir + "/a.txt").c_str(), "r");
  int i = 0;
  double x = 0;
  char word[16] = {};
  int n = std::fscanf(f, "%d %lf %15s", &i, &x, word);
  char* line = nullptr;
  size_t cap = 0;
  getline(&line, &cap, f);  // the rest of the first line
  ssize_t l2 = getline(&line, &cap, f);
  std::string second = l2 > 0 ? std::string(line) : "";
  free(line);
  std::fclose(f);
  *scan = (n == 3 && i == 42 && x == 3.5 && std::strcmp(word, "word") == 0 && second == "second line\n")
              ? "ok"
              : Fail("fscanf n=" + std::to_string(n));
  return listing;
}

std::string CheckFilesystem(const std::string& base) {
  try {
    fs::path root = fs::path(base) / "native-fs";
    fs::remove_all(root);
    fs::create_directories(root / "x" / "y");
    {
      std::ofstream out(root / "x" / "f.txt");
      out << "12345";
    }
    if (fs::file_size(root / "x" / "f.txt") != 5) return Fail("file_size");
    fs::copy_file(root / "x" / "f.txt", root / "x" / "g.txt");
    fs::rename(root / "x" / "g.txt", root / "x" / "y" / "h.txt");
    std::ifstream in(root / "x" / "y" / "h.txt");
    std::string content;
    in >> content;
    if (content != "12345") return Fail("copied content '" + content + "'");
    int entries = 0;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
      (void)e;
      ++entries;
    }
    if (entries != 4) return Fail("entries " + std::to_string(entries));  // x, x/y, x/f.txt, x/y/h.txt
    if (!fs::is_directory(fs::canonical(root / "x" / ".." / "x"))) return Fail("canonical");
    fs::path cwd = fs::current_path();
    if (cwd.empty()) return Fail("current_path");
    auto removed = fs::remove_all(root);
    if (removed != 5 || fs::exists(root)) return Fail("remove_all " + std::to_string(removed));
    return "ok";
  } catch (const fs::filesystem_error& e) {
    return Fail(e.what());
  }
}

// --- signals, time, sysconf --------------------------------------------------------

std::atomic<int> g_usr1{0};
std::atomic<int> g_usr2{0};
std::atomic<int> g_signo{0};
void OnUsr1(int sig, siginfo_t* info, void*) {
  g_usr1.fetch_add(1);
  g_signo = info != nullptr ? info->si_signo : -1;
  (void)sig;
}
void OnUsr2(int) { g_usr2.fetch_add(1); }

std::string CheckSignals() {
  struct sigaction sa {};
  sa.sa_sigaction = OnUsr1;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGUSR1, &sa, nullptr) != 0) return Errno("sigaction");
  if (raise(SIGUSR1) != 0 || g_usr1 != 1 || g_signo != SIGUSR1) return Fail("raise SIGUSR1");
  signal(SIGUSR2, OnUsr2);
  sigset_t block, old, pending;
  sigemptyset(&block);
  sigaddset(&block, SIGUSR2);
  sigprocmask(SIG_BLOCK, &block, &old);
  kill(getpid(), SIGUSR2);
  sigpending(&pending);
  bool was_pending = sigismember(&pending, SIGUSR2) == 1 && g_usr2 == 0;
  sigprocmask(SIG_SETMASK, &old, nullptr);  // delivers it
  if (!was_pending || g_usr2 != 1) return Fail("blocked SIGUSR2");
  signal(SIGUSR2, SIG_IGN);
  raise(SIGUSR2);
  if (g_usr2 != 1) return Fail("SIG_IGN");
  return "ok";
}

std::string CheckStrftime() {
  time_t t = 365 * 86400 + 3 * 3600 + 4 * 60 + 5;  // 1971-01-01 03:04:05 UTC, a Friday
  tm g{};
  gmtime_r(&t, &g);
  char buf[64];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %a %j %z %e|%k|%P", &g);
  if (std::strcmp(buf, "1971-01-01 03:04:05 Fri 001 +0000  1| 3|am") != 0) return Fail(buf);
  if (timegm(&g) != t) return Fail("timegm");
  tm l{};
  if (localtime_r(&t, &l) == nullptr || mktime(&l) != t) return Fail("localtime/mktime");
  return "ok";
}

std::string CheckSysconf() {
  long page = sysconf(_SC_PAGESIZE);
  long cpus = sysconf(_SC_NPROCESSORS_ONLN);
  if (page != 4096 || cpus < 1) return Fail(std::to_string(page) + "/" + std::to_string(cpus));
  return "ok";
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_gesher_nativeapp_NativeActivity_stringFromJNI(JNIEnv* env, jclass) {
  std::string s = "Hello from C++, " + ExceptionCheck();
  return env->NewStringUTF(s.c_str());
}

// Calls NativeActivity.square(x) + 1 through the C++ JNIEnv wrapper (the
// va_list form of CallIntMethod).
extern "C" JNIEXPORT jint JNICALL
Java_com_gesher_nativeapp_NativeActivity_callBack(JNIEnv* env, jobject self, jint x) {
  jclass cls = env->GetObjectClass(self);
  jmethodID square = env->GetMethodID(cls, "square", "(I)I");
  return env->CallIntMethod(self, square, x) + 1;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_gesher_nativeapp_NativeActivity_platformChecks(JNIEnv* env, jclass, jobject assets,
                                                        jstring jdir) {
  ATrace_beginSection("platformChecks");
  const char* chars = env->GetStringUTFChars(jdir, nullptr);
  std::string dir = chars;
  env->ReleaseStringUTFChars(jdir, chars);
  std::string listing, config, scan;
  std::string out = "asset=" + CheckAsset(env, assets, &listing, &config);
  out += ", assets=" + listing;
  out += ", config=" + config;
  ASensorList sensors = nullptr;
  int nsensors = ASensorManager_getSensorList(ASensorManager_getInstance(), &sensors);
  out += ", sensors=" + std::to_string(nsensors);
  out += ", looper=" + CheckLooper();
  out += ", mainlooper=" + ArmMainLooper();
  out += ", socketpair=" + CheckSocketpair();
  out += ", tcp=" + CheckTcp();
  out += ", getaddrinfo=" + CheckGetaddrinfo();
  out += ", readdir=" + CheckReaddir(dir, &scan);
  out += ", fscanf=" + scan;
  out += ", filesystem=" + CheckFilesystem(dir);
  out += ", signals=" + CheckSignals();
  out += ", strftime=" + CheckStrftime();
  out += ", sysconf=" + CheckSysconf();
  ATrace_endSection();
  return env->NewStringUTF(out.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_gesher_nativeapp_NativeActivity_mainLooperResult(JNIEnv* env, jclass) {
  std::string s = g_main_calls == 1 && (g_main_events & ALOOPER_EVENT_INPUT) != 0
                      ? "fd callback ran"
                      : "fd callback calls " + std::to_string(g_main_calls.load());
  return env->NewStringUTF(s.c_str());
}
