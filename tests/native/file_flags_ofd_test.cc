// Native test for Track N Item N32 (Issue #56):
// File Status Flags & Open File Description Semantics in POSIX Layer.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>

static int g_assertions = 0;
static int g_failures = 0;

#define CHECK(cond)                                                                \
  do {                                                                             \
    ++g_assertions;                                                                \
    if (!(cond)) {                                                                 \
      ++g_failures;                                                                \
      std::printf("FAIL line %d: %s (errno=%d)\n", __LINE__, #cond, errno);        \
    }                                                                              \
  } while (0)

#define CHECK_EQ(a, b)                                                             \
  do {                                                                             \
    ++g_assertions;                                                                \
    auto val_a = (a);                                                              \
    auto val_b = (b);                                                              \
    if (val_a != val_b) {                                                          \
      ++g_failures;                                                                \
      std::printf("FAIL line %d: %s == %s (actual %lld vs %lld, errno=%d)\n",      \
                  __LINE__, #a, #b, static_cast<long long>(val_a),                 \
                  static_cast<long long>(val_b), errno);                           \
    }                                                                              \
  } while (0)

static std::string TempTestFile(const char* name) {
  const char* temp_dir = getenv("TEMP");
  if (!temp_dir) temp_dir = getenv("TMP");
  if (!temp_dir) temp_dir = ".";
  std::string path = temp_dir;
  if (!path.empty() && path.back() != '/' && path.back() != '\\') {
    path += '/';
  }
  path += name;
  return path;
}

static void check(const char* name, int ok) {
  CHECK(ok);
  if (!ok) std::printf("  contract: %s\n", name);
}

#include "file_description_contract_test.inc"

// 1. dup() shares O_NONBLOCK, O_APPEND across descriptors pointing to the same OFD.
// Modifying F_SETFL on fd1 modifies F_GETFL on fd2 immediately.
static void TestDupSharesFlags() {
  std::string path = TempTestFile("test_n32_dup_shares_flags.tmp");
  int fd1 = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_APPEND, 0666);
  CHECK(fd1 >= 0);

  int fl1 = fcntl(fd1, F_GETFL);
  CHECK_EQ(fl1 & O_ACCMODE, O_RDWR);
  CHECK((fl1 & O_APPEND) != 0);
  CHECK((fl1 & O_NONBLOCK) == 0);

  // Duplicate fd1
  int fd2 = dup(fd1);
  CHECK(fd2 >= 0 && fd2 != fd1);

  // fd2 must share the same OFD flags: O_RDWR and O_APPEND
  int fl2 = fcntl(fd2, F_GETFL);
  CHECK_EQ(fl2 & O_ACCMODE, O_RDWR);
  CHECK((fl2 & O_APPEND) != 0);
  CHECK((fl2 & O_NONBLOCK) == 0);

  // Modifying status flags on fd1 via F_SETFL MUST immediately affect fd2!
  CHECK_EQ(fcntl(fd1, F_SETFL, O_NONBLOCK), 0);

  fl1 = fcntl(fd1, F_GETFL);
  CHECK_EQ(fl1 & O_ACCMODE, O_RDWR);
  CHECK((fl1 & O_NONBLOCK) != 0);
  CHECK((fl1 & O_APPEND) == 0);

  fl2 = fcntl(fd2, F_GETFL);
  CHECK_EQ(fl2 & O_ACCMODE, O_RDWR);
  CHECK((fl2 & O_NONBLOCK) != 0);
  CHECK((fl2 & O_APPEND) == 0);

  // Modifying status flags on fd2 via F_SETFL MUST immediately affect fd1!
  CHECK_EQ(fcntl(fd2, F_SETFL, O_APPEND | O_NONBLOCK), 0);

  fl1 = fcntl(fd1, F_GETFL);
  CHECK((fl1 & O_APPEND) != 0);
  CHECK((fl1 & O_NONBLOCK) != 0);

  fl2 = fcntl(fd2, F_GETFL);
  CHECK((fl2 & O_APPEND) != 0);
  CHECK((fl2 & O_NONBLOCK) != 0);

  // Test across dup2
  int fd3 = 45;
  CHECK_EQ(dup2(fd1, fd3), fd3);
  int fl3 = fcntl(fd3, F_GETFL);
  CHECK((fl3 & O_APPEND) != 0);
  CHECK((fl3 & O_NONBLOCK) != 0);

  // Test across dup3
  int fd4 = 46;
  CHECK_EQ(dup3(fd1, fd4, 0), fd4);
  int fl4 = fcntl(fd4, F_GETFL);
  CHECK((fl4 & O_APPEND) != 0);
  CHECK((fl4 & O_NONBLOCK) != 0);

  // Test across fcntl(F_DUPFD)
  int fd5 = fcntl(fd1, F_DUPFD, 50);
  CHECK(fd5 >= 50);
  int fl5 = fcntl(fd5, F_GETFL);
  CHECK((fl5 & O_APPEND) != 0);
  CHECK((fl5 & O_NONBLOCK) != 0);

  // Test across fcntl(F_DUPFD_CLOEXEC)
  int fd6 = fcntl(fd1, F_DUPFD_CLOEXEC, 60);
  CHECK(fd6 >= 60);
  int fl6 = fcntl(fd6, F_GETFL);
  CHECK((fl6 & O_APPEND) != 0);
  CHECK((fl6 & O_NONBLOCK) != 0);

  // Changing status flags on fd6 must propagate to all 6 descriptors
  CHECK_EQ(fcntl(fd6, F_SETFL, 0), 0);
  CHECK((fcntl(fd1, F_GETFL) & (O_APPEND | O_NONBLOCK)) == 0);
  CHECK((fcntl(fd2, F_GETFL) & (O_APPEND | O_NONBLOCK)) == 0);
  CHECK((fcntl(fd3, F_GETFL) & (O_APPEND | O_NONBLOCK)) == 0);
  CHECK((fcntl(fd4, F_GETFL) & (O_APPEND | O_NONBLOCK)) == 0);
  CHECK((fcntl(fd5, F_GETFL) & (O_APPEND | O_NONBLOCK)) == 0);
  CHECK((fcntl(fd6, F_GETFL) & (O_APPEND | O_NONBLOCK)) == 0);

  // Close fd1, surviving descriptors must still retain and share the OFD
  CHECK_EQ(close(fd1), 0);
  CHECK_EQ(fcntl(fd2, F_SETFL, O_APPEND), 0);
  CHECK((fcntl(fd2, F_GETFL) & O_APPEND) != 0);
  CHECK((fcntl(fd3, F_GETFL) & O_APPEND) != 0);

  CHECK_EQ(close(fd2), 0);
  CHECK_EQ(close(fd3), 0);
  CHECK_EQ(close(fd4), 0);
  CHECK_EQ(close(fd5), 0);
  CHECK_EQ(close(fd6), 0);
  unlink(path.c_str());
}

// 2. FD_CLOEXEC is per-descriptor and NOT shared across dup().
static void TestFdCloexecNotSharedAcrossDup() {
  std::string path = TempTestFile("test_n32_cloexec.tmp");
  int fd1 = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
  CHECK(fd1 >= 0);

  // fd1 has FD_CLOEXEC
  CHECK_EQ(fcntl(fd1, F_GETFD), FD_CLOEXEC);

  // dup() MUST create a new fd with FD_CLOEXEC cleared
  int fd2 = dup(fd1);
  CHECK(fd2 >= 0);
  CHECK_EQ(fcntl(fd2, F_GETFD), 0);
  // fd1 still has FD_CLOEXEC
  CHECK_EQ(fcntl(fd1, F_GETFD), FD_CLOEXEC);

  // Modifying FD_CLOEXEC on fd2 must NOT affect fd1
  CHECK_EQ(fcntl(fd2, F_SETFD, FD_CLOEXEC), 0);
  CHECK_EQ(fcntl(fd2, F_GETFD), FD_CLOEXEC);
  CHECK_EQ(fcntl(fd1, F_GETFD), FD_CLOEXEC);

  CHECK_EQ(fcntl(fd1, F_SETFD, 0), 0);
  CHECK_EQ(fcntl(fd1, F_GETFD), 0);
  CHECK_EQ(fcntl(fd2, F_GETFD), FD_CLOEXEC);

  // dup2() clears FD_CLOEXEC on target fd
  int fd3 = 70;
  CHECK_EQ(dup2(fd2, fd3), fd3);
  CHECK_EQ(fcntl(fd3, F_GETFD), 0);

  // F_DUPFD clears FD_CLOEXEC
  int fd4 = fcntl(fd2, F_DUPFD, 75);
  CHECK(fd4 >= 75);
  CHECK_EQ(fcntl(fd4, F_GETFD), 0);

  // F_DUPFD_CLOEXEC sets FD_CLOEXEC
  int fd5 = fcntl(fd1, F_DUPFD_CLOEXEC, 80);
  CHECK(fd5 >= 80);
  CHECK_EQ(fcntl(fd5, F_GETFD), FD_CLOEXEC);

  // dup3() with O_CLOEXEC sets FD_CLOEXEC
  int fd6 = 85;
  CHECK_EQ(dup3(fd1, fd6, O_CLOEXEC), fd6);
  CHECK_EQ(fcntl(fd6, F_GETFD), FD_CLOEXEC);

  // dup3() without O_CLOEXEC clears FD_CLOEXEC
  int fd7 = 86;
  CHECK_EQ(dup3(fd2, fd7, 0), fd7);
  CHECK_EQ(fcntl(fd7, F_GETFD), 0);

  CHECK_EQ(close(fd1), 0);
  CHECK_EQ(close(fd2), 0);
  CHECK_EQ(close(fd3), 0);
  CHECK_EQ(close(fd4), 0);
  CHECK_EQ(close(fd5), 0);
  CHECK_EQ(close(fd6), 0);
  CHECK_EQ(close(fd7), 0);
  unlink(path.c_str());
}

// 3. File offset (lseek, read, write) is shared across all fds pointing to that OFD.
static void TestFileOffsetSharedAcrossDup() {
  std::string path = TempTestFile("test_n32_offset.tmp");
  int fd1 = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
  CHECK(fd1 >= 0);

  const char initial_data[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  const size_t data_len = sizeof(initial_data) - 1;
  CHECK_EQ(write(fd1, initial_data, data_len), static_cast<ssize_t>(data_len));

  int fd2 = dup(fd1);
  CHECK(fd2 >= 0);

  // Both should start at offset data_len
  CHECK_EQ(lseek(fd1, 0, SEEK_CUR), static_cast<off_t>(data_len));
  CHECK_EQ(lseek(fd2, 0, SEEK_CUR), static_cast<off_t>(data_len));

  // Seeking on fd1 moves offset observed by fd2
  CHECK_EQ(lseek(fd1, 10, SEEK_SET), 10);
  CHECK_EQ(lseek(fd2, 0, SEEK_CUR), 10);

  // Reading from fd1 advances offset observed by fd2
  char buf[16] = {};
  CHECK_EQ(read(fd1, buf, 5), 5);
  buf[5] = '\0';
  CHECK_EQ(strcmp(buf, "ABCDE"), 0);

  CHECK_EQ(lseek(fd1, 0, SEEK_CUR), 15);
  CHECK_EQ(lseek(fd2, 0, SEEK_CUR), 15);

  // Reading next 5 bytes from fd2 continues from offset 15
  memset(buf, 0, sizeof(buf));
  CHECK_EQ(read(fd2, buf, 5), 5);
  buf[5] = '\0';
  CHECK_EQ(strcmp(buf, "FGHIJ"), 0);

  CHECK_EQ(lseek(fd1, 0, SEEK_CUR), 20);
  CHECK_EQ(lseek(fd2, 0, SEEK_CUR), 20);

  // Seeking on fd2 updates fd1
  CHECK_EQ(lseek(fd2, 5, SEEK_SET), 5);
  CHECK_EQ(lseek(fd1, 0, SEEK_CUR), 5);

  // Writing on fd1 at offset 5 overwrites and advances offset for both
  CHECK_EQ(write(fd1, "#####", 5), 5);
  CHECK_EQ(lseek(fd1, 0, SEEK_CUR), 10);
  CHECK_EQ(lseek(fd2, 0, SEEK_CUR), 10);

  // Verify overwritten bytes by reading from fd2
  CHECK_EQ(lseek(fd2, 5, SEEK_SET), 5);
  memset(buf, 0, sizeof(buf));
  CHECK_EQ(read(fd2, buf, 5), 5);
  buf[5] = '\0';
  CHECK_EQ(strcmp(buf, "#####"), 0);

  CHECK_EQ(close(fd1), 0);
  CHECK_EQ(close(fd2), 0);
  unlink(path.c_str());
}

// 4. O_APPEND write semantics and dynamic toggle via F_SETFL across dups.
static void TestAppendModeSemantics() {
  std::string path = TempTestFile("test_n32_append.tmp");
  int fd1 = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_APPEND, 0666);
  CHECK(fd1 >= 0);

  int fd2 = dup(fd1);
  CHECK(fd2 >= 0);

  CHECK_EQ(write(fd1, "AAAA", 4), 4);
  CHECK_EQ(lseek(fd2, 0, SEEK_CUR), 4);

  // With O_APPEND, seeking to 0 and writing must still append at EOF
  CHECK_EQ(lseek(fd1, 0, SEEK_SET), 0);
  // A zero-length write must not reposition the shared open description.
  CHECK_EQ(write(fd1, "", 0), 0);
  CHECK_EQ(lseek(fd2, 0, SEEK_CUR), 0);
  CHECK_EQ(write(fd1, "BBBB", 4), 4);

  // File should now be 8 bytes: "AAAABBBB"
  CHECK_EQ(lseek(fd2, 0, SEEK_END), 8);

  // Writing via fd2 with O_APPEND also appends
  CHECK_EQ(lseek(fd2, 0, SEEK_SET), 0);
  CHECK_EQ(write(fd2, "CCCC", 4), 4);
  CHECK_EQ(lseek(fd1, 0, SEEK_END), 12);

  // Now clear O_APPEND via fcntl(fd1, F_SETFL, 0)
  CHECK_EQ(fcntl(fd1, F_SETFL, 0), 0);
  CHECK((fcntl(fd1, F_GETFL) & O_APPEND) == 0);
  CHECK((fcntl(fd2, F_GETFL) & O_APPEND) == 0);

  // Without O_APPEND, seek to 0 and write overwrites the beginning
  CHECK_EQ(lseek(fd2, 0, SEEK_SET), 0);
  CHECK_EQ(write(fd2, "XXXX", 4), 4);
  CHECK_EQ(lseek(fd1, 0, SEEK_CUR), 4);

  // Verify the file content: "XXXXBBBBCCCC"
  CHECK_EQ(lseek(fd1, 0, SEEK_SET), 0);
  char content[16] = {};
  CHECK_EQ(read(fd1, content, 12), 12);
  content[12] = '\0';
  CHECK_EQ(strcmp(content, "XXXXBBBBCCCC"), 0);

  // Now re-enable O_APPEND via fd2
  CHECK_EQ(fcntl(fd2, F_SETFL, O_APPEND), 0);
  CHECK((fcntl(fd1, F_GETFL) & O_APPEND) != 0);
  CHECK((fcntl(fd2, F_GETFL) & O_APPEND) != 0);

  // Seeking to 0 on fd1 and writing appends to the end
  CHECK_EQ(lseek(fd1, 0, SEEK_SET), 0);
  CHECK_EQ(write(fd1, "YYYY", 4), 4);

  CHECK_EQ(lseek(fd1, 0, SEEK_SET), 0);
  char content2[20] = {};
  CHECK_EQ(read(fd2, content2, 16), 16);
  content2[16] = '\0';
  CHECK_EQ(strcmp(content2, "XXXXBBBBCCCCYYYY"), 0);

  CHECK_EQ(close(fd1), 0);
  CHECK_EQ(close(fd2), 0);
  unlink(path.c_str());
}

// 5. Access mode bits (O_RDONLY, O_WRONLY, O_RDWR) cannot be modified by F_SETFL.
static void TestAccessModeImmutable() {
  std::string path = TempTestFile("test_n32_acc.tmp");
  int fd_w = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
  CHECK(fd_w >= 0);
  CHECK_EQ(fcntl(fd_w, F_GETFL) & O_ACCMODE, O_WRONLY);

  // F_SETFL with O_RDWR must NOT change access mode from O_WRONLY
  CHECK_EQ(fcntl(fd_w, F_SETFL, O_RDWR | O_NONBLOCK), 0);
  int fl_w = fcntl(fd_w, F_GETFL);
  CHECK_EQ(fl_w & O_ACCMODE, O_WRONLY);
  CHECK((fl_w & O_NONBLOCK) != 0);

  int fd_w2 = dup(fd_w);
  CHECK(fd_w2 >= 0);
  CHECK_EQ(fcntl(fd_w2, F_GETFL) & O_ACCMODE, O_WRONLY);
  CHECK((fcntl(fd_w2, F_GETFL) & O_NONBLOCK) != 0);
  CHECK_EQ(close(fd_w), 0);
  CHECK_EQ(close(fd_w2), 0);

  // Test read-only
  int fd_r = open(path.c_str(), O_RDONLY);
  CHECK(fd_r >= 0);
  CHECK_EQ(fcntl(fd_r, F_GETFL) & O_ACCMODE, O_RDONLY);

  CHECK_EQ(fcntl(fd_r, F_SETFL, O_RDWR | O_APPEND), 0);
  int fl_r = fcntl(fd_r, F_GETFL);
  CHECK_EQ(fl_r & O_ACCMODE, O_RDONLY);
  CHECK((fl_r & O_APPEND) != 0);

  int fd_r2 = dup(fd_r);
  CHECK(fd_r2 >= 0);
  CHECK_EQ(fcntl(fd_r2, F_GETFL) & O_ACCMODE, O_RDONLY);
  CHECK((fcntl(fd_r2, F_GETFL) & O_APPEND) != 0);
  CHECK_EQ(close(fd_r), 0);
  CHECK_EQ(close(fd_r2), 0);
  unlink(path.c_str());
}

// 6. Pipe OFD sharing and non-blocking read EAGAIN semantics.
static void TestPipeOfdSharing() {
  int p[2] = {-1, -1};
  CHECK_EQ(pipe2(p, O_NONBLOCK), 0);
  CHECK(p[0] >= 0 && p[1] >= 0);

  CHECK_EQ(fcntl(p[0], F_GETFL) & O_ACCMODE, O_RDONLY);
  CHECK((fcntl(p[0], F_GETFL) & O_NONBLOCK) != 0);

  CHECK_EQ(fcntl(p[1], F_GETFL) & O_ACCMODE, O_WRONLY);
  CHECK((fcntl(p[1], F_GETFL) & O_NONBLOCK) != 0);

  int p0_dup = dup(p[0]);
  CHECK(p0_dup >= 0);
  CHECK((fcntl(p0_dup, F_GETFL) & O_NONBLOCK) != 0);

  // Nonblocking read on empty pipe returns EAGAIN
  char buf[16] = {};
  CHECK_EQ(read(p0_dup, buf, 0), 0);
  CHECK_EQ(read(p0_dup, buf, sizeof(buf)), -1);
  CHECK_EQ(errno, EAGAIN);

  // Toggle O_NONBLOCK off via p0_dup
  CHECK_EQ(fcntl(p0_dup, F_SETFL, 0), 0);
  CHECK((fcntl(p[0], F_GETFL) & O_NONBLOCK) == 0);
  CHECK((fcntl(p0_dup, F_GETFL) & O_NONBLOCK) == 0);

  // Write and read
  CHECK_EQ(write(p[1], "pipe", 4), 4);
  CHECK_EQ(read(p0_dup, buf, 4), 4);
  buf[4] = '\0';
  CHECK_EQ(strcmp(buf, "pipe"), 0);

  CHECK_EQ(close(p[0]), 0);
  CHECK_EQ(close(p[1]), 0);
  CHECK_EQ(close(p0_dup), 0);
}

// 7. Invalid descriptors and closed descriptors report EBADF.
static void TestInvalidFdHandling() {
  CHECK_EQ(fcntl(-1, F_GETFL), -1);
  CHECK_EQ(errno, EBADF);

  CHECK_EQ(fcntl(-1, F_SETFL, 0), -1);
  CHECK_EQ(errno, EBADF);

  CHECK_EQ(fcntl(9999, F_GETFL), -1);
  CHECK_EQ(errno, EBADF);

  CHECK_EQ(fcntl(9999, F_SETFL, 0), -1);
  CHECK_EQ(errno, EBADF);
}

int main() {
  std::printf("Running Track N Item N32 File Status Flags & OFD Semantics test...\n");
  TestDupSharesFlags();
  TestFdCloexecNotSharedAcrossDup();
  TestFileOffsetSharedAcrossDup();
  TestAppendModeSemantics();
  TestAccessModeImmutable();
  TestPipeOfdSharing();
  TestInvalidFdHandling();
  test_file_description_contracts();

  std::printf("file_flags_ofd_test: %d assertions, %d failures\n", g_assertions, g_failures);
  return g_failures == 0 ? 0 : 1;
}
