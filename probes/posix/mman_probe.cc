// gesher-base's mmap layer (compat/posix/src/mman.cc, ADR 0036): placeholder
// mappings, MAP_FIXED over existing mappings, executable file views, views
// split by MAP_FIXED, shared dual views of one file. Prints "mman probe: all
// passed" when every check holds.

#include <sys/mman.h>

#include <io.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void Check(const char* what, bool ok) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++failures;
}

constexpr size_t kPage = 4096;

// A temporary file of `size` bytes, byte i = (i / kPage) + 1 ... as an fd.
int TempFile(size_t size, wchar_t* path_out) {
  wchar_t dir[MAX_PATH];
  GetTempPathW(MAX_PATH, dir);
  GetTempFileNameW(dir, L"mmp", 0, path_out);
  HANDLE h = CreateFileW(path_out, GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                         nullptr);
  std::vector<uint8_t> bytes(size);
  for (size_t i = 0; i < size; ++i) bytes[i] = static_cast<uint8_t>(i / kPage + 1);
  DWORD written = 0;
  WriteFile(h, bytes.data(), static_cast<DWORD>(size), &written, nullptr);
  return _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_RDWR | _O_BINARY);
}

DWORD Type(const void* p) {
  MEMORY_BASIC_INFORMATION info;
  VirtualQuery(p, &info, sizeof(info));
  return info.Type;
}

DWORD Protection(const void* p) {
  MEMORY_BASIC_INFORMATION info;
  VirtualQuery(p, &info, sizeof(info));
  return info.Protect;
}

DWORD State(const void* p) {
  MEMORY_BASIC_INFORMATION info;
  VirtualQuery(p, &info, sizeof(info));
  return info.State;
}

// The process's commit charge.
size_t Commit() {
  PROCESS_MEMORY_COUNTERS_EX counters = {};
  counters.cb = sizeof(counters);
  GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                       sizeof(counters));
  return counters.PrivateUsage;
}

bool Readable(const void* p) {
  DWORD protection = Protection(p);
  return protection != PAGE_NOACCESS && protection != 0 && (protection & PAGE_GUARD) == 0;
}

void ConcurrentZeroing() {
  uint8_t* p = static_cast<uint8_t*>(mmap(nullptr, kPage, PROT_READ | PROT_WRITE,
                                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  Check("concurrent zeroing mapping", p != MAP_FAILED);
  if (p == MAP_FAILED) return;

  // A reserved page during madvise is still logically RW. mprotect must not
  // mistake that transient state for a completed PROT_NONE reservation and
  // let the recommit silently restore RW permissions after it returns.
  std::atomic<bool> go{false};
  std::atomic<int> advice_failures{0};
  std::thread zeroer([&] {
    while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
    for (int i = 0; i < 4096; ++i) {
      if (madvise(p, kPage, MADV_DONTNEED) != 0) ++advice_failures;
    }
  });
  go.store(true, std::memory_order_release);
  int protection_failures = 0;
  for (int i = 0; i < 4096; ++i) {
    if (mprotect(p, kPage, PROT_NONE) != 0) ++protection_failures;
    MEMORY_BASIC_INFORMATION info;
    VirtualQuery(p, &info, sizeof(info));
    if (info.State == MEM_COMMIT && info.Protect != PAGE_NOACCESS) ++protection_failures;
    if (mprotect(p, kPage, PROT_READ | PROT_WRITE) != 0) ++protection_failures;
  }
  zeroer.join();
  Check("mprotect PROT_NONE survives concurrent madvise", protection_failures == 0);
  Check("concurrent mapping changes preserve successful advice", advice_failures.load() == 0);

  // Exercise the real vectored-handler retry as a different thread performs
  // atomic accesses while the same page is decommitted/recommitted. No page
  // protection changes run concurrently in this phase.
  std::atomic<bool> entered{false}, stop{false};
  std::atomic<unsigned> accesses{0};
  std::thread accessor([&] {
    entered.store(true, std::memory_order_release);
    do {
      InterlockedExchangeAdd(reinterpret_cast<volatile LONG*>(p), 1);
      ++accesses;
    } while (!stop.load(std::memory_order_acquire));
  });
  while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
  for (int i = 0; i < 1024; ++i) {
    if (madvise(p, kPage, MADV_DONTNEED) != 0) ++advice_failures;
  }
  stop.store(true, std::memory_order_release);
  accessor.join();
  Check("concurrent atomic accesses survive madvise", accesses.load() > 0 && advice_failures.load() == 0);
  Check("concurrent zeroing mapping is released", munmap(p, kPage) == 0);
}

}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  wchar_t path[MAX_PATH];
  const int fd = TempFile(10 * kPage + 100, path);  // 10 whole pages and a partial one
  Check("a test file", fd >= 0);

  // 1. Anonymous memory: unmap a middle page, map it again with MAP_FIXED.
  uint8_t* a = static_cast<uint8_t*>(mmap(nullptr, 8 * kPage, PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  Check("anonymous mmap", a != MAP_FAILED);
  memset(a, 7, 8 * kPage);
  Check("munmap of a middle page", munmap(a + 3 * kPage, kPage) == 0);
  Check("the page is gone, its neighbours stay", !Readable(a + 3 * kPage) && a[2 * kPage] == 7 &&
                                                     a[4 * kPage] == 7);
  void* again = mmap(a + 3 * kPage, kPage, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  Check("MAP_FIXED into the hole: zero page", again == a + 3 * kPage && a[3 * kPage] == 0);
  Check("mprotect across former pieces", mprotect(a, 8 * kPage, PROT_READ) == 0 &&
                                             Protection(a + 3 * kPage) == PAGE_READONLY &&
                                             Protection(a + 6 * kPage) == PAGE_READONLY);
  Check("munmap of it all", munmap(a, 8 * kPage) == 0 && !Readable(a));

  // 2. An oat file's way: a PROT_NONE reservation, file segments MAP_FIXED
  // into it, executable and writable, as views of the file.
  uint8_t* r = static_cast<uint8_t*>(mmap(nullptr, 16 * kPage, PROT_NONE,
                                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  Check("a PROT_NONE reservation", r != MAP_FAILED);
  void* text = mmap(r + 2 * kPage, 3 * kPage, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_FIXED, fd,
                    2 * kPage);
  Check("an executable segment: a view of the file", text == r + 2 * kPage &&
                                                         Type(text) == MEM_MAPPED &&
                                                         Protection(text) == PAGE_EXECUTE_READ &&
                                                         r[2 * kPage] == 3 && r[4 * kPage] == 5);
  void* data = mmap(r + 5 * kPage, 2 * kPage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd,
                    5 * kPage);
  Check("a writable segment: a copy-on-write view", data == r + 5 * kPage &&
                                                        Type(data) == MEM_MAPPED &&
                                                        r[5 * kPage] == 6);
  r[5 * kPage] = 99;
  Check("mprotect it read-only after writing", mprotect(r + 5 * kPage, 2 * kPage, PROT_READ) == 0 &&
                                                   Protection(r + 5 * kPage) == PAGE_READONLY &&
                                                   r[5 * kPage] == 99);
  // The last segment, past the file's whole pages: anonymous with its bytes.
  void* last = mmap(r + 8 * kPage, 3 * kPage, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 9 * kPage);
  Check("a segment that ends past the file", last == r + 8 * kPage && r[8 * kPage] == 10 &&
                                                 r[9 * kPage] == 11 && r[9 * kPage + 100] == 0 &&
                                                 r[10 * kPage] == 0);
  Check("the reservation around them stays", !Readable(r) && !Readable(r + 7 * kPage) &&
                                                 !Readable(r + 15 * kPage));
  // The file itself is unchanged by the private write.
  uint8_t* check = static_cast<uint8_t*>(mmap(nullptr, kPage, PROT_READ, MAP_SHARED, fd, 5 * kPage));
  Check("a private write does not reach the file", check != MAP_FAILED && check[0] == 6);
  munmap(check, kPage);

  // 2b. Segments mapped without PROT_EXEC can get it later, as on Linux
  // (packers and protectors mprotect a whole library RWX).
  uint8_t* ro_seg = static_cast<uint8_t*>(mmap(r + 12 * kPage, 2 * kPage, PROT_READ,
                                               MAP_PRIVATE | MAP_FIXED, fd, 0));
  Check("a read-only segment: a view", ro_seg == r + 12 * kPage && Type(ro_seg) == MEM_MAPPED &&
                                           Protection(ro_seg) == PAGE_READONLY && ro_seg[0] == 1);
  Check("... mprotect R|X", mprotect(ro_seg, 2 * kPage, PROT_READ | PROT_EXEC) == 0 &&
                                Protection(ro_seg) == PAGE_EXECUTE_READ);
  bool ro_rwx = mprotect(ro_seg, kPage, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
  if (ro_rwx) ro_seg[0] = 77;
  Check("... mprotect RWX, written privately",
        ro_rwx && Protection(ro_seg) == PAGE_EXECUTE_READWRITE && ro_seg[kPage] == 2);
  bool rw_rwx = mprotect(r + 5 * kPage, 2 * kPage, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
  if (rw_rwx) r[6 * kPage] = 55;
  Check("the writable segment: mprotect RWX", rw_rwx && r[5 * kPage] == 99 && r[6 * kPage] == 55);
  Check("mprotect RWX across segments and back",
        mprotect(r + 2 * kPage, 5 * kPage, PROT_READ | PROT_WRITE | PROT_EXEC) == 0 &&
            mprotect(r + 2 * kPage, 3 * kPage, PROT_READ | PROT_EXEC) == 0 &&
            mprotect(ro_seg, 2 * kPage, PROT_READ) == 0 && ro_seg[0] == 77 &&
            Protection(ro_seg) == PAGE_READONLY && Protection(r + 2 * kPage) == PAGE_EXECUTE_READ);
  munmap(ro_seg, 2 * kPage);
  mmap(r + 12 * kPage, 2 * kPage, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  {
    // A read-only private view charges no commit, and can still become RWX.
    wchar_t big_path[MAX_PATH];
    const size_t big = size_t{64} << 20;
    const int big_fd = TempFile(big, big_path);
    const size_t before_view = Commit();
    uint8_t* bv = static_cast<uint8_t*>(mmap(nullptr, big, PROT_READ, MAP_PRIVATE, big_fd, 0));
    Check("a 64 MiB read-only private view", bv != MAP_FAILED && bv[kPage] == 2);
    Check("... charges no commit", Commit() < before_view + (size_t{8} << 20));
    bool bv_rwx = bv != MAP_FAILED && mprotect(bv, kPage, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
    if (bv_rwx) bv[0] = 9;
    Check("... mprotect RWX of a page of it", bv_rwx && bv[0] == 9);
    if (bv != MAP_FAILED) munmap(bv, big);
    close(big_fd);
  }

  // 3. MAP_FIXED over the middle of a view that was written: what stays of
  // the view keeps its contents and protections.
  uint8_t* v = static_cast<uint8_t*>(mmap(nullptr, 6 * kPage, PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE, fd, 0));
  Check("a private view", v != MAP_FAILED && Type(v) == MEM_MAPPED && v[kPage] == 2);
  v[0] = 42;
  v[5 * kPage] = 43;
  mprotect(v + 4 * kPage, kPage, PROT_READ);
  void* middle = mmap(v + 2 * kPage, 2 * kPage, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  Check("MAP_FIXED anonymous pages over the middle of a view", middle == v + 2 * kPage &&
                                                                  v[2 * kPage] == 0 &&
                                                                  Type(middle) == MEM_PRIVATE);
  Check("the view's written pages stay written", v[0] == 42 && v[5 * kPage] == 43 &&
                                                     v[kPage] == 2 && v[4 * kPage] == 5);
  Check("... and its protections", Protection(v + 4 * kPage) == PAGE_READONLY);
  Check("munmap of the view and what replaced it", munmap(v, 6 * kPage) == 0 && !Readable(v) &&
                                                       !Readable(v + 2 * kPage));

  // 4. The JIT's dual view: one file shared twice, written through one view
  // and executed through the other; the end of a view replaced by MAP_FIXED.
  uint8_t* rw = static_cast<uint8_t*>(mmap(nullptr, 4 * kPage, PROT_READ | PROT_WRITE,
                                           MAP_SHARED, fd, 0));
  uint8_t* ro = static_cast<uint8_t*>(mmap(nullptr, 4 * kPage, PROT_READ, MAP_SHARED, fd, 0));
  Check("two shared views", rw != MAP_FAILED && ro != MAP_FAILED);
  Check("munmap of a view's end", munmap(ro + 2 * kPage, 2 * kPage) == 0);
  uint8_t* rx = static_cast<uint8_t*>(mmap(ro + 2 * kPage, 2 * kPage, PROT_READ | PROT_EXEC,
                                           MAP_SHARED | MAP_FIXED, fd, 2 * kPage));
  Check("an executable shared view in its place", rx == ro + 2 * kPage &&
                                                      Protection(rx) == PAGE_EXECUTE_READ);
  rw[2 * kPage] = 0xc3;  // ret
  rw[1] = 77;
  Check("written through one view, seen through the others", rx[0] == 0xc3 && ro[1] == 77);
  using Fn = void (*)();
  reinterpret_cast<Fn>(rx)();
  Check("code written through the RW view runs from the RX view", true);
  munmap(rw, 4 * kPage);
  munmap(ro, 4 * kPage);

  // 5. mremap moves anonymous memory.
  uint8_t* m = static_cast<uint8_t*>(mmap(nullptr, 4 * kPage, PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  m[kPage] = 5;
  uint8_t* moved = static_cast<uint8_t*>(mremap(m, 4 * kPage, 4 * kPage, MREMAP_MAYMOVE | MREMAP_FIXED,
                                                r + 12 * kPage));
  Check("mremap into the reservation", moved == r + 12 * kPage && moved[kPage] == 5 && !Readable(m));
  Check("munmap of the reservation", munmap(r, 16 * kPage) == 0 && !Readable(r + 2 * kPage));

  // 6. PROT_NONE anonymous memory is reserved, not committed: ART reserves
  // gigabytes that way, which cost commit charge when they were committed.
  const size_t before = Commit();
  uint8_t* big = static_cast<uint8_t*>(mmap(nullptr, size_t{1} << 30, PROT_NONE,
                                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  Check("a 1 GiB PROT_NONE mapping", big != MAP_FAILED && State(big) == MEM_RESERVE);
  Check("... charges no commit", Commit() < before + (size_t{16} << 20));
  Check("mprotect of two pages commits them, zero-filled",
        mprotect(big + 8 * kPage, 2 * kPage, PROT_READ | PROT_WRITE) == 0 &&
        State(big + 8 * kPage) == MEM_COMMIT && big[8 * kPage] == 0 && big[9 * kPage] == 0 &&
        State(big + 10 * kPage) == MEM_RESERVE && State(big + 7 * kPage) == MEM_RESERVE);
  big[8 * kPage] = 11;
  Check("PROT_NONE over reserved and committed pages",
        mprotect(big, 16 * kPage, PROT_NONE) == 0 && State(big) == MEM_RESERVE &&
        !Readable(big + 8 * kPage));
  Check("... keeps what was written", mprotect(big + 8 * kPage, kPage, PROT_READ) == 0 &&
                                         big[8 * kPage] == 11);
  // A mixed source moves like ART's from-space: written pages copied, PROT_NONE
  // pages still reserved at the destination, and the source reads as zeros.
  uint8_t* to = static_cast<uint8_t*>(mremap(big, 16 * kPage, 16 * kPage,
                                             MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP,
                                             big + 64 * kPage));
  Check("mremap of reserved and committed pages", to == big + 64 * kPage &&
        to[8 * kPage] == 11 && State(to) == MEM_RESERVE && State(to + 15 * kPage) == MEM_RESERVE &&
        Protection(to + 8 * kPage) == PAGE_READONLY);
  Check("... the source stays, zeroed", big[8 * kPage] == 0 && State(big) == MEM_RESERVE);
  uint8_t* written = static_cast<uint8_t*>(mmap(big + 128 * kPage, 256 * kPage, PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0));
  memset(written, 1, 256 * kPage);
  uint8_t* none = static_cast<uint8_t*>(mmap(written, 256 * kPage, PROT_NONE,
                                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0));
  Check("MAP_FIXED PROT_NONE over written pages gives their commit back",
        none == written && State(written) == MEM_RESERVE && State(written + 255 * kPage) == MEM_RESERVE);
  Check("madvise(MADV_DONTNEED) of reserved pages", madvise(big, 64 * kPage, MADV_DONTNEED) == 0);
  Check("munmap of the 1 GiB mapping", munmap(big, size_t{1} << 30) == 0 &&
        State(big) == MEM_FREE && Commit() < before + (size_t{16} << 20));

  // 7. Protection changes and ordinary accesses during physical page release.
  ConcurrentZeroing();

  _close(fd);
  printf(failures == 0 ? "mman probe: all passed\n" : "mman probe: %d failed\n", failures);
  return failures == 0 ? 0 : 1;
}
