// POSIX FIFOs on Windows (fifo.cc, ADR 0097), through the production
// descriptor layer and with real separate processes: independent opens in
// different processes rendezvous on one pipe, a descriptor inherited through
// the importer keeps its endpoint open, a crashed peer is counted out, and
// the Realm sequence (O_RDWR, O_NONBLOCK, 1-byte notify, poll/epoll wait,
// nonblocking drain) works between processes.
#include "exec_spawn.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <string>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

extern "C" int gesher_ftruncate(int fd, off_t length);  // bionic's ftruncate

static int checks, failures;
static void Check(const char* name, bool result) {
  ++checks;
  if (!result) {
    ++failures;
    std::printf("FAIL fifo: %s (errno=%d win32=%lu)\n", name, errno, GetLastError());
    std::fflush(stdout);
  }
}

static std::wstring Self() {
  wchar_t image[32768];
  DWORD length = GetModuleFileNameW(nullptr, image, sizeof(image) / sizeof(image[0]));
  return std::wstring(image, length);
}

static std::wstring Widen(const char* text) {
  std::wstring out;
  for (; *text; ++text) out += static_cast<wchar_t>(static_cast<unsigned char>(*text));
  return out;
}

// An independent process: it inherits no handle and opens the FIFO by name.
static bool StartIndependent(const char* mode, const char* path, PROCESS_INFORMATION* child) {
  std::wstring command = L"\"" + Self() + L"\" " + Widen(mode) + L" \"" + Widen(path) + L"\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  *child = {};
  return CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &startup, child) != FALSE;
}

static DWORD Finish(PROCESS_INFORMATION* child, DWORD timeout = 20000) {
  if (child->hThread) CloseHandle(child->hThread);
  DWORD status = 255;
  if (WaitForSingleObject(child->hProcess, timeout) == WAIT_OBJECT_0) {
    GetExitCodeProcess(child->hProcess, &status);
  } else {
    TerminateProcess(child->hProcess, 254);
    WaitForSingleObject(child->hProcess, 2000);
    status = 254;
  }
  CloseHandle(child->hProcess);
  *child = {};
  if (status != 0) std::printf("fifo child exit status %lu\n", status);
  return status;
}

static size_t ReadAll(int fd, std::string* out) {
  char buffer[8192];
  for (;;) {
    const int n = static_cast<int>(read(fd, buffer, sizeof(buffer)));
    if (n <= 0) return n == 0 ? out->size() : static_cast<size_t>(-1);
    out->append(buffer, static_cast<size_t>(n));
  }
}

static unsigned char Pattern(size_t i) { return static_cast<unsigned char>((i * 131 + 7) & 0xff); }
constexpr size_t kLarge = 200000;  // several times the pipe capacity

// --- child modes -------------------------------------------------------------

// Blocking O_RDONLY open waits for the parent's writer in another process.
static int ChildReader(const char* path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return 10;
  std::string data;
  if (ReadAll(fd, &data) != 17 || data != "hello from parent") return 11;
  close(fd);
  return 0;
}

// Blocking O_WRONLY open waits for the parent's reader; the writes block on
// a full pipe until the other process drains it.
static int ChildWriter(const char* path) {
  int fd = open(path, O_WRONLY);
  if (fd < 0) return 20;
  std::vector<unsigned char> data(kLarge);
  for (size_t i = 0; i < kLarge; ++i) data[i] = Pattern(i);
  size_t done = 0;
  while (done < kLarge) {
    const int n = static_cast<int>(write(fd, data.data() + done, static_cast<unsigned>(kLarge - done)));
    if (n <= 0) return 21;
    done += static_cast<size_t>(n);
  }
  return close(fd) == 0 ? 0 : 22;
}

// Realm's InterprocessCondVar/ExternalCommitHelper sequence, from the peer:
// create-or-verify, open O_RDWR, O_NONBLOCK, notify with one byte. The
// answer comes back on a second FIFO (both sides would otherwise race to
// drain the same byte, as Realm's own listeners do).
static int RealmOpen(const char* path) {
  struct stat st{};
  if (mkfifo(path, 0600) != 0 && errno != EEXIST) return -1;
  if (stat(path, &st) != 0 || !S_ISFIFO(st.st_mode)) return -1;
  int fd = open(path, O_RDWR);
  if (fd >= 0 && fcntl(fd, F_SETFL, O_NONBLOCK) != 0) { close(fd); return -1; }
  return fd;
}
static int ChildRealm(const char* path) {
  const std::string answer_path = std::string(path) + "-answer";
  int fd = RealmOpen(path), answer = RealmOpen(answer_path.c_str());
  if (fd < 0 || answer < 0) return 30;
  if (write(fd, "n", 1) != 1) return 31;  // notify the parent
  pollfd p{answer, POLLIN, 0};
  if (poll(&p, 1, 10000) != 1 || (p.revents & POLLIN) == 0) return 32;
  char byte = 0;
  if (read(answer, &byte, 1) != 1 || byte != 'a') return 33;
  if (read(answer, &byte, 1) != -1 || errno != EAGAIN) return 34;
  close(answer);
  close(fd);
  return 0;
}

// Writes through an inherited descriptor, then exits without closing it.
static int ChildInherited() {
  const char text[] = "inherited";
  if (fcntl(9, F_GETFL) < 0 || (fcntl(9, F_GETFL) & O_ACCMODE) != O_WRONLY) return 40;
  struct stat st{};
  if (fstat(9, &st) != 0 || !S_ISFIFO(st.st_mode)) return 41;
  if (lseek(9, 0, SEEK_CUR) != -1 || errno != ESPIPE) return 42;
  return write(9, text, sizeof(text) - 1) == static_cast<int>(sizeof(text) - 1) ? 0 : 43;
}

// Opens a writer, writes one byte and dies without any cleanup.
static int ChildCrash(const char* path) {
  int fd = open(path, O_WRONLY | O_NONBLOCK);
  if (fd < 0) return 50;
  if (write(fd, "z", 1) != 1) return 51;
  TerminateProcess(GetCurrentProcess(), 0);
  return 52;
}

// --- parent checks -----------------------------------------------------------

static void FileSystem(const char* path) {
  struct stat st{}, fst{};
  Check("mkfifo creates a FIFO", mkfifo(path, 0600) == 0);
  Check("second mkfifo reports EEXIST", mkfifo(path, 0600) == -1 && errno == EEXIST);
  Check("mknod(S_IFIFO) also reports EEXIST", mknod(path, S_IFIFO | 0600, 0) == -1 && errno == EEXIST);
  Check("stat reports S_IFIFO with no size", stat(path, &st) == 0 && S_ISFIFO(st.st_mode) && st.st_size == 0);
  Check("lstat reports S_IFIFO", lstat(path, &st) == 0 && S_ISFIFO(st.st_mode));
  Check("access(F_OK) finds the FIFO", access(path, F_OK) == 0);
  Check("chmod works on a FIFO", chmod(path, 0644) == 0);
  bool listed = false;
  if (DIR* dir = opendir(".")) {
    while (dirent* entry = readdir(dir)) {
      if (std::strcmp(entry->d_name, path) == 0) listed = entry->d_type == DT_FIFO;
    }
    closedir(dir);
  }
  Check("readdir reports DT_FIFO", listed);
  int fd = open(path, O_RDWR | O_NONBLOCK);
  Check("O_RDWR open never waits", fd >= 0);
  Check("fstat agrees with stat", fstat(fd, &fst) == 0 && S_ISFIFO(fst.st_mode) && stat(path, &st) == 0 &&
                                      fst.st_ino == st.st_ino && fst.st_dev == st.st_dev &&
                                      (fst.st_mode & 0777) == (st.st_mode & 0777));
  Check("F_GETFL reports the open flags", (fcntl(fd, F_GETFL) & (O_ACCMODE | O_NONBLOCK)) == (O_RDWR | O_NONBLOCK));
  Check("lseek fails with ESPIPE", lseek(fd, 0, SEEK_SET) == -1 && errno == ESPIPE);
  char byte = 0;
  Check("pread fails with ESPIPE", pread(fd, &byte, 1, 0) == -1 && errno == ESPIPE);
  Check("ftruncate fails with EINVAL", gesher_ftruncate(fd, 0) == -1 && errno == EINVAL);
  Check("fsync fails with EINVAL", fsync(fd) == -1 && errno == EINVAL);
  Check("O_CREAT|O_EXCL on a FIFO fails with EEXIST", open(path, O_RDWR | O_CREAT | O_EXCL, 0600) == -1 &&
                                                         errno == EEXIST);
  // O_TRUNC is ignored for a FIFO, and the name stays a FIFO.
  int truncating = open(path, O_WRONLY | O_TRUNC | O_CREAT | O_NONBLOCK, 0600);
  Check("O_TRUNC open of a FIFO succeeds", truncating >= 0);
  Check("O_TRUNC leaves the path a FIFO", stat(path, &st) == 0 && S_ISFIFO(st.st_mode));
  Check("data written after O_TRUNC arrives", write(truncating, "T", 1) == 1 && read(fd, &byte, 1) == 1 &&
                                                byte == 'T');
  int nofollow = open(path, O_WRONLY | O_TRUNC | O_NOFOLLOW | O_NONBLOCK);
  Check("O_NOFOLLOW|O_TRUNC opens the FIFO itself as a FIFO", nofollow >= 0 && write(nofollow, "N", 1) == 1 &&
                                                                read(fd, &byte, 1) == 1 && byte == 'N');
  Check("O_NOFOLLOW|O_TRUNC leaves the path a FIFO", stat(path, &st) == 0 && S_ISFIFO(st.st_mode));
  close(nofollow);
  close(truncating);
  close(fd);
}

static void SameProcess(const char* path) {
  Check("O_WRONLY|O_NONBLOCK without a reader fails with ENXIO", open(path, O_WRONLY | O_NONBLOCK) == -1 &&
                                                                    errno == ENXIO);
  int reader = open(path, O_RDONLY | O_NONBLOCK);
  Check("O_RDONLY|O_NONBLOCK opens without a writer", reader >= 0);
  char buffer[16];
  Check("read without any writer is end of file", read(reader, buffer, sizeof(buffer)) == 0);
  pollfd p{reader, POLLIN, 0};
  Check("a reader that never had a writer does not hang up", poll(&p, 1, 0) == 0);
  // The failed ENXIO open above must not have left a writer behind.
  int writer = open(path, O_WRONLY | O_NONBLOCK);
  Check("a writer opens once a reader exists", writer >= 0);
  Check("an empty pipe would block the reader", read(reader, buffer, sizeof(buffer)) == -1 && errno == EAGAIN);
  Check("write", write(writer, "abc", 3) == 3);
  int available = -1;
  Check("FIONREAD counts buffered bytes", ioctl(reader, FIONREAD, &available) == 0 && available == 3);
  Check("poll reports POLLIN", poll(&p, 1, 0) == 1 && (p.revents & POLLIN));
  Check("read returns the bytes", read(reader, buffer, sizeof(buffer)) == 3 && std::memcmp(buffer, "abc", 3) == 0);
  Check("read on the write end fails with EBADF", read(writer, buffer, 1) == -1 && errno == EBADF);
  Check("write on the read end fails with EBADF", write(reader, "x", 1) == -1 && errno == EBADF);
  int duplicate = dup(writer);
  Check("dup of a writer", duplicate >= 0);
  close(writer);
  Check("a dup keeps the writer open", poll(&p, 1, 0) == 0 &&
                                         read(reader, buffer, 1) == -1 && errno == EAGAIN);
  close(duplicate);
  Check("closing the last writer hangs up the reader", poll(&p, 1, 0) == 1 && (p.revents & POLLHUP));
  Check("and reads end of file", read(reader, buffer, sizeof(buffer)) == 0);
  writer = open(path, O_WRONLY | O_NONBLOCK);
  Check("a new writer joins the same pipe", writer >= 0 && write(writer, "d", 1) == 1 &&
                                              read(reader, buffer, 1) == 1 && buffer[0] == 'd');
  close(reader);
  Check("writing without readers fails with EPIPE", write(writer, "e", 1) == -1 && errno == EPIPE);
  pollfd w{writer, POLLOUT, 0};
  Check("a writer without readers polls POLLERR", poll(&w, 1, 0) == 1 && (w.revents & POLLERR));
  close(writer);
}

static void AtomicWrites(const char* path) {
  int fd = open(path, O_RDWR | O_NONBLOCK);
  Check("open O_RDWR for capacity", fd >= 0);
  if (fd < 0) return;
  std::vector<char> chunk(65536, 'q');
  int total = 0;
  for (;;) {
    int n = static_cast<int>(write(fd, chunk.data(), 1000));
    if (n < 0) break;
    total += n;
  }
  Check("the pipe holds 64 KiB", errno == EAGAIN && total == 65000);
  char drain[1000];
  Check("drain 900 bytes", read(fd, drain, 900) == 900);  // 1436 bytes free
  int before = -1, after = -1;
  ioctl(fd, FIONREAD, &before);
  Check("a 4096-byte write without room fails with EAGAIN", write(fd, chunk.data(), 4096) == -1 && errno == EAGAIN);
  ioctl(fd, FIONREAD, &after);
  Check("and writes nothing", before == after);
  pollfd p{fd, POLLOUT, 0};
  Check("POLLOUT waits for PIPE_BUF bytes of room", poll(&p, 1, 0) == 0);
  Check("a larger nonblocking write is split", write(fd, chunk.data(), 8192) == 1436);
  Check("a full pipe refuses one byte", write(fd, chunk.data(), 1) == -1 && errno == EAGAIN);
  close(fd);
  fd = open(path, O_RDWR | O_NONBLOCK);
  char byte;
  Check("the last close discards the pipe's bytes", fd >= 0 && read(fd, &byte, 1) == -1 && errno == EAGAIN);
  close(fd);
}

struct BlockedRead {
  int fd;
  int result = 0;
  int error = 0;
};
static void* Reader(void* raw) {
  auto* blocked = static_cast<BlockedRead*>(raw);
  char byte;
  blocked->result = static_cast<int>(read(blocked->fd, &byte, 1));
  blocked->error = errno;
  return nullptr;
}
static void* OpenReader(void* raw) {
  auto* path = static_cast<const char*>(raw);
  int fd = open(path, O_RDONLY);  // no writer: waits
  if (fd >= 0) close(fd);
  return nullptr;
}

static void Cancellation(const char* path) {
  int fd = open(path, O_RDWR);
  Check("blocking O_RDWR open", fd >= 0);
  BlockedRead blocked{fd};
  pthread_t thread;
  Check("start a blocked reader", pthread_create(&thread, nullptr, Reader, &blocked) == 0);
  Sleep(100);
  void* result = nullptr;
  Check("cancel the blocked reader", pthread_cancel(thread) == 0);
  Check("the canceled reader exits", pthread_join(thread, &result) == 0 && result == PTHREAD_CANCELED);
  close(fd);
  Check("start a blocked open", pthread_create(&thread, nullptr, OpenReader, const_cast<char*>(path)) == 0);
  Sleep(100);
  Check("cancel the blocked open", pthread_cancel(thread) == 0);
  Check("the canceled open exits", pthread_join(thread, &result) == 0 && result == PTHREAD_CANCELED);
  Check("the canceled reader endpoint was withdrawn", open(path, O_WRONLY | O_NONBLOCK) == -1 && errno == ENXIO);
}

static void CrossProcess(const char* path) {
  PROCESS_INFORMATION child{};
  Check("start an independent reader process", StartIndependent("--reader", path, &child));
  if (child.hProcess) {
    // Blocks until the other process's reader has opened.
    int fd = open(path, O_WRONLY);
    Check("blocking writer open meets the other process's reader", fd >= 0);
    Check("write to the other process", write(fd, "hello from parent", 17) == 17);
    close(fd);
    Check("the other process read the bytes and end of file", Finish(&child) == 0);
  }
  Check("start an independent writer process", StartIndependent("--writer", path, &child));
  if (child.hProcess) {
    int fd = open(path, O_RDONLY);
    Check("blocking reader open meets the other process's writer", fd >= 0);
    std::string data;
    Check("read everything the other process wrote", ReadAll(fd, &data) == kLarge);
    bool same = data.size() == kLarge;
    for (size_t i = 0; same && i < kLarge; ++i) same = static_cast<unsigned char>(data[i]) == Pattern(i);
    Check("in order, across a full pipe", same);
    close(fd);
    Check("the writer process finished", Finish(&child) == 0);
  }
}

static void Realm(const char* path) {
  // The parent's side of the Realm sequence, waiting in epoll as
  // ExternalCommitHelper does (edge-triggered).
  const std::string answer_path = std::string(path) + "-answer";
  int fd = RealmOpen(path), answer = RealmOpen(answer_path.c_str());
  Check("Realm: create and open both FIFOs O_RDWR|O_NONBLOCK", fd >= 0 && answer >= 0);
  int epfd = epoll_create1(EPOLL_CLOEXEC);
  epoll_event event{};
  event.events = EPOLLIN | EPOLLET;
  event.data.u32 = 7;
  Check("Realm: epoll accepts the FIFO", epfd >= 0 && epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event) == 0);
  PROCESS_INFORMATION child{};
  Check("Realm: start the peer process", StartIndependent("--realm", path, &child));
  epoll_event ready{};
  Check("Realm: epoll_wait sees the other process's notification",
        epoll_wait(epfd, &ready, 1, 10000) == 1 && ready.data.u32 == 7 && (ready.events & EPOLLIN));
  char buffer[64];
  int drained = 0, n;
  while ((n = static_cast<int>(read(fd, buffer, sizeof(buffer)))) > 0) drained += n;
  Check("Realm: nonblocking drain ends with EAGAIN", drained == 1 && n == -1 && errno == EAGAIN);
  Check("Realm: answer", write(answer, "a", 1) == 1);
  if (child.hProcess) Check("Realm: the peer saw the answer through poll", Finish(&child) == 0);
  close(epfd);
  close(answer);
  close(fd);
  unlink(answer_path.c_str());
}

static void Inheritance(const char* path) {
  int reader = open(path, O_RDONLY | O_NONBLOCK);
  int writer = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  Check("Inheritance: open both ends", reader >= 0 && writer >= 0);
  auto snapshot = gesher::CaptureDescriptor(writer);
  Check("Inheritance: capture the writer", static_cast<bool>(snapshot));
  if (!snapshot) return;
  std::wstring native = Self();
  native = native.substr(0, native.find_last_of(L"/\\") + 1) + L"ofd_native_child.exe";
  gesher::exec::Launch launch;
  launch.application = native;
  gesher::exec::AppendQuoted(&launch.command_line, native);
  launch.command_line += L" --empty";
  launch.kind = "exe";
  std::vector<gesher::exec::ChildFd> files{{9, snapshot->handle, snapshot}};
  PROCESS_INFORMATION child{};
  DWORD error = gesher::exec::SpawnWithFds(launch, nullptr, L"", files, &child);
  Check("Inheritance: a program without the importer cannot receive a FIFO", error == ERROR_NOT_SUPPORTED);
  if (!error) Finish(&child);
  launch.application = Self();
  launch.command_line.clear();
  gesher::exec::AppendQuoted(&launch.command_line, launch.application);
  launch.command_line += L" --inherited";
  error = gesher::exec::SpawnWithFds(launch, nullptr, L"", files, &child);
  Check("Inheritance: start an importing child with the writer", error == 0);
  snapshot.reset();
  files.clear();
  close(writer);  // the child's inherited copy keeps the endpoint open
  pollfd p{reader, POLLIN, 0};
  if (!error) {
    Check("Inheritance: the child wrote through the inherited descriptor", Finish(&child) == 0);
    std::string data;
    Check("Inheritance: the parent reads the child's bytes", ReadAll(reader, &data) == 9 && data == "inherited");
    Check("Inheritance: the child's exit hangs up", poll(&p, 1, 0) == 1 && (p.revents & POLLHUP));
  }
  close(reader);
}

static void Crash(const char* path) {
  int reader = open(path, O_RDONLY | O_NONBLOCK);
  PROCESS_INFORMATION child{};
  Check("Crash: start a writer that dies", reader >= 0 && StartIndependent("--crash", path, &child));
  if (!child.hProcess) return;
  Finish(&child);
  // Blocking read: the byte, then end of file once the dead writer is swept.
  fcntl(reader, F_SETFL, 0);
  char byte = 0;
  Check("Crash: the dead writer's byte remains readable", read(reader, &byte, 1) == 1 && byte == 'z');
  const ULONGLONG start = GetTickCount64();
  Check("Crash: a dead writer is counted out (end of file)", read(reader, &byte, 1) == 0);
  Check("Crash: promptly", GetTickCount64() - start < 2000);
  close(reader);
}

// The only remaining endpoint is inherited by a child that has not touched
// it yet: the bytes already written and the child's endpoint must survive the
// parent closing every copy (the launcher passes the ring with the description).
// One byte within a deadline, so that a failing child never lingers.
static int ReadByteWithin(int fd, char* byte, int timeout_ms) {
  pollfd p{fd, POLLIN, 0};
  if (poll(&p, 1, timeout_ms) != 1 || (p.revents & POLLIN) == 0) return -1;
  return static_cast<int>(read(fd, byte, 1));
}

static int ChildInheritedOnly() {
  char byte = 0;
  if (ReadByteWithin(9, &byte, 10000) != 1 || byte != 'x') return 60;  // written before the handoff
  if (ReadByteWithin(9, &byte, 10000) != 1 || byte != 'z') return 61;  // from a later writer
  return write(9, "y", 1) == 1 ? 0 : 62;                  // O_RDWR: still a writer
}

// The exporter passes its only descriptor to a child and exits at once,
// without closing anything: the bytes it wrote must still reach the child.
static int ChildExporter(const char* path) {
  int fd = open(path, O_RDWR);
  if (fd < 0 || write(fd, "x", 1) != 1) return 70;
  auto snapshot = gesher::CaptureDescriptor(fd);
  if (!snapshot) return 71;
  gesher::exec::Launch launch;
  launch.application = Self();
  gesher::exec::AppendQuoted(&launch.command_line, launch.application);
  launch.command_line += L" --late-reader \"" + Widen(path) + L"\"";
  launch.kind = "exe";
  std::vector<gesher::exec::ChildFd> files{{9, snapshot->handle, snapshot}};
  PROCESS_INFORMATION child{};
  if (gesher::exec::SpawnWithFds(launch, nullptr, L"", files, &child) != 0) return 72;
  ExitProcess(0);  // no destructor, no close: the process and its handles just end
}
static int ChildLateReader(const char* path) {
  Sleep(1500);  // long after the exporter has exited
  char byte = 0;
  if (ReadByteWithin(9, &byte, 10000) != 1 || byte != 'x') return 80;
  if (ReadByteWithin(9, &byte, 10000) != 1 || byte != 'z') return 81;
  const std::string done = std::string(path) + "-done";
  int marker = open(done.c_str(), O_CREAT | O_WRONLY, 0600);
  return marker >= 0 && close(marker) == 0 ? 0 : 82;
}

static void ExporterExits(const char* path) {
  const std::string done = std::string(path) + "-done";
  unlink(done.c_str());
  Check("Exporter exit: mkfifo", mkfifo(path, 0600) == 0);
  PROCESS_INFORMATION exporter{};
  Check("Exporter exit: start the exporter", StartIndependent("--exporter", path, &exporter));
  if (!exporter.hProcess) return;
  Check("Exporter exit: the exporter spawned and exited", Finish(&exporter) == 0);
  int writer = open(path, O_WRONLY | O_NONBLOCK);
  Check("Exporter exit: the grandchild's reader counts", writer >= 0 && write(writer, "z", 1) == 1);
  struct stat st{};
  bool finished = false;
  for (int i = 0; i < 200 && !finished; ++i) {
    finished = stat(done.c_str(), &st) == 0;
    if (!finished) Sleep(50);
  }
  Check("Exporter exit: the grandchild read the exporter's byte and a later one", finished);
  close(writer);
  unlink(done.c_str());
  unlink(path);
}

static void InheritedOnly(const char* path) {
  Check("Handoff: mkfifo", mkfifo(path, 0600) == 0);
  int fd = open(path, O_RDWR);
  Check("Handoff: open O_RDWR and write before spawning", fd >= 0 && write(fd, "x", 1) == 1);
  auto snapshot = gesher::CaptureDescriptor(fd);
  if (!snapshot) { Check("Handoff: capture", false); return; }
  gesher::exec::Launch launch;
  launch.application = Self();
  gesher::exec::AppendQuoted(&launch.command_line, launch.application);
  launch.command_line += L" --inherited-only";
  launch.kind = "exe";
  std::vector<gesher::exec::ChildFd> files{{9, snapshot->handle, snapshot}};
  PROCESS_INFORMATION child{};
  DWORD error = gesher::exec::SpawnWithFds(launch, nullptr, L"", files, &child);
  Check("Handoff: start the child", error == 0);
  // Drop every parent copy at once: nothing else in any process maps the ring.
  files.clear();
  snapshot.reset();
  close(fd);
  if (error) return;
  Sleep(300);
  int writer = open(path, O_WRONLY | O_NONBLOCK);
  Check("Handoff: the child's inherited reader still counts (no ENXIO)", writer >= 0);
  Check("Handoff: a later writer reaches the child", writer >= 0 && write(writer, "z", 1) == 1);
  Check("Handoff: the child read the earlier and later bytes and wrote", Finish(&child) == 0);
  close(writer);
  unlink(path);
}

static void* RaceMkfifo(void* raw) {
  auto* path = static_cast<const char*>(raw);
  struct stat st{};
  const int result = mkfifo(path, 0600);
  if (result != 0 && errno != EEXIST) return reinterpret_cast<void*>(1);
  // A creator that lost the race finds a FIFO, never a regular file.
  if (stat(path, &st) != 0 || !S_ISFIFO(st.st_mode)) return reinterpret_cast<void*>(2);
  return reinterpret_cast<void*>(result == 0 ? 3 : 4);
}

static void Paths(const char* path) {
  const std::string race = std::string(path) + "-race";
  pthread_t threads[8];
  int created = 0, fine = 0;
  for (auto& thread : threads) pthread_create(&thread, nullptr, RaceMkfifo, const_cast<char*>(race.c_str()));
  for (auto& thread : threads) {
    void* result = nullptr;
    pthread_join(thread, &result);
    created += result == reinterpret_cast<void*>(3);
    fine += result == reinterpret_cast<void*>(3) || result == reinterpret_cast<void*>(4);
  }
  Check("racing mkfifo: one creator, every loser sees a FIFO", created == 1 && fine == 8);
  unlink(race.c_str());
  Check("mkfifo: gesher_is_fifo_path", mkfifo(path, 0644) == 0 && gesher_is_fifo_path(path) == 1);
  int fd = open(path, O_RDWR | O_NONBLOCK);
  struct stat st{};
  Check("fchmod changes the FIFO file's mode", fd >= 0 && fchmod(fd, 0444) == 0 && stat(path, &st) == 0 &&
                                                  (st.st_mode & 0222) == 0 && fchmod(fd, 0644) == 0);
  close(fd);
  const std::string link = std::string(path) + "-link";
  if (symlink(path, link.c_str()) == 0) {
    char resolved[4096];
    fd = open(link.c_str(), O_RDWR | O_NONBLOCK);
    Check("a symbolic link to a FIFO opens the FIFO", fd >= 0 && write(fd, "s", 1) == 1);
    Check("stat follows the link to the FIFO", stat(link.c_str(), &st) == 0 && S_ISFIFO(st.st_mode));
    Check("access follows the link", access(link.c_str(), F_OK) == 0);
    Check("realpath resolves the link to the FIFO", realpath(link.c_str(), resolved) != nullptr &&
                                                      std::strstr(resolved, path) != nullptr);
    Check("utimes follows the link", utimes(link.c_str(), nullptr) == 0);
    Check("truncate check follows the link", gesher_is_fifo_path(link.c_str()) == 1);
    close(fd);
    unlink(link.c_str());
  } else {
    std::printf("fifo: symbolic links unavailable here (errno=%d); link checks skipped\n", errno);
  }
  char resolved[4096];
  Check("realpath of a FIFO", realpath(path, resolved) != nullptr);
  Check("utimes on a FIFO", utimes(path, nullptr) == 0);
  unlink(path);
  int regular = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
  Check("a regular file is no FIFO", regular >= 0 && gesher_is_fifo_path(path) == 0);
  close(regular);
  unlink(path);
}

static void Unlink(const char* path) {
  int fd = open(path, O_RDWR | O_NONBLOCK);
  Check("Unlink: open", fd >= 0);
  Check("Unlink: remove the name", unlink(path) == 0);
  struct stat st{};
  Check("Unlink: the name is gone", stat(path, &st) == -1 && errno == ENOENT);
  char byte = 0;
  Check("Unlink: the open pipe still works", write(fd, "u", 1) == 1 && read(fd, &byte, 1) == 1 && byte == 'u');
  close(fd);
}

int main(int argc, char** argv) {
  if (argc == 3 && std::strcmp(argv[1], "--reader") == 0) return ChildReader(argv[2]);
  if (argc == 3 && std::strcmp(argv[1], "--writer") == 0) return ChildWriter(argv[2]);
  if (argc == 3 && std::strcmp(argv[1], "--realm") == 0) return ChildRealm(argv[2]);
  if (argc == 3 && std::strcmp(argv[1], "--crash") == 0) return ChildCrash(argv[2]);
  if (argc == 2 && std::strcmp(argv[1], "--inherited") == 0) return ChildInherited();
  if (argc == 2 && std::strcmp(argv[1], "--inherited-only") == 0) return ChildInheritedOnly();
  if (argc == 3 && std::strcmp(argv[1], "--exporter") == 0) return ChildExporter(argv[2]);
  if (argc == 3 && std::strcmp(argv[1], "--late-reader") == 0) return ChildLateReader(argv[2]);
  char path[64];
  std::snprintf(path, sizeof(path), "gesher-fifo-test-%lu", GetCurrentProcessId());
  unlink(path);
  FileSystem(path);
  SameProcess(path);
  AtomicWrites(path);
  Cancellation(path);
  CrossProcess(path);
  Realm(path);
  Inheritance(path);
  Crash(path);
  Unlink(path);
  InheritedOnly((std::string(path) + "-handoff").c_str());
  ExporterExits((std::string(path) + "-exporter").c_str());
  Paths(path);
  unlink(path);
  std::printf("fifo-windows: %d assertions, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
