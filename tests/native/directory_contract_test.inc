// Public directory API contracts, shared by the host adapter check and the
// Windows x86-64 / translated ARM64 ELF libc suites.
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef int (*directory_filter_fn)(const struct dirent*);
typedef int (*directory_compare_fn)(const struct dirent**, const struct dirent**);
typedef int (*directory_scan_fn)(const char*, struct dirent***,
                                 directory_filter_fn, directory_compare_fn);
typedef int (*directory_scanat_fn)(int, const char*, struct dirent***,
                                   directory_filter_fn, directory_compare_fn);

static void test_directory_cookies(const char* path) {
  DIR* (*open_dir)(const char*) = (DIR* (*)(const char*))dlsym(RTLD_DEFAULT, "opendir");
  struct dirent* (*read_dir)(DIR*) = (struct dirent* (*)(DIR*))dlsym(RTLD_DEFAULT, "readdir");
  int (*close_dir)(DIR*) = (int (*)(DIR*))dlsym(RTLD_DEFAULT, "closedir");
  long (*tell_dir)(DIR*) = (long (*)(DIR*))dlsym(RTLD_DEFAULT, "telldir");
  void (*seek_dir)(DIR*, long) = (void (*)(DIR*, long))dlsym(RTLD_DEFAULT, "seekdir");
  void (*rewind_dir)(DIR*) = (void (*)(DIR*))dlsym(RTLD_DEFAULT, "rewinddir");
  int ready = open_dir && read_dir && close_dir && tell_dir && seek_dir && rewind_dir;
  check("directory: seekdir/telldir and stream exports", ready);
  if (!ready) return;
  DIR* dir = open_dir(path);
  check("directory: open cookie stream", dir != NULL);
  if (dir == NULL) return;
  long start = tell_dir(dir);
  struct dirent* first = read_dir(dir);
  char first_name[256] = {0}, second_name[256] = {0};
  if (first != NULL) strcpy(first_name, first->d_name);
  long next = tell_dir(dir);
  struct dirent* second = read_dir(dir);
  if (second != NULL) strcpy(second_name, second->d_name);
  check("directory: tell records stream progress", first != NULL && second != NULL && next != start);
  seek_dir(dir, next);
  struct dirent* again = read_dir(dir);
  check("directory: seek restores next entry", again != NULL && strcmp(again->d_name, second_name) == 0);
  seek_dir(dir, start);
  again = read_dir(dir);
  check("directory: seek restores initial entry", again != NULL && strcmp(again->d_name, first_name) == 0);
  while (read_dir(dir) != NULL) {}
  long end = tell_dir(dir);
  seek_dir(dir, start);
  seek_dir(dir, end);
  check("directory: end cookie restores EOF", tell_dir(dir) == end && read_dir(dir) == NULL);
  rewind_dir(dir);
  check("directory: rewind resets cookie", tell_dir(dir) == start);
  close_dir(dir);
}

static int directory_filter(const struct dirent* entry) {
  errno = EACCES;  // Callback errno must not be mistaken for a read failure.
  return strncmp(entry->d_name, "item-", 5) == 0;
}
static int directory_reject(const struct dirent* entry) {
  (void)entry;
  errno = EACCES;
  return 0;
}
static int directory_compare(const struct dirent** left, const struct dirent** right) {
  return strcmp((*left)->d_name, (*right)->d_name);
}
static void directory_free_list(struct dirent** entries, int count) {
  if (count < 0) return;
  for (int i = 0; i < count; ++i) free(entries[i]);
  free(entries);
}

static void test_directory_contracts(void) {
  directory_scan_fn scan = (directory_scan_fn)dlsym(RTLD_DEFAULT, "scandir");
  directory_scan_fn scan64 = (directory_scan_fn)dlsym(RTLD_DEFAULT, "scandir64");
  directory_scanat_fn scanat = (directory_scanat_fn)dlsym(RTLD_DEFAULT, "scandirat");
  directory_scanat_fn scanat64 = (directory_scanat_fn)dlsym(RTLD_DEFAULT, "scandirat64");
  check("directory: scandir/scandir64 exports", scan != NULL && scan64 != NULL);
  check("directory: scandirat/scandirat64 exports", scanat != NULL && scanat64 != NULL);
  if (scan == NULL || scan64 == NULL) return;

  char path[] = "gesher-directory-XXXXXX";
  if (mkdtemp(path) == NULL) {
    check("directory: create fixture", 0);
    return;
  }
  const char* names[] = {"item-z", "item-a", "item-m", "skipped"};
  int created = 1;
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    char name[128];
    snprintf(name, sizeof(name), "%s/%s", path, names[i]);
    int fd = open(name, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) created = 0;
    else close(fd);
  }
  check("directory: create entries", created);
  test_directory_cookies(path);

  struct dirent** entries = NULL;
  int count = scan(path, &entries, directory_filter, directory_compare);
  check("directory: filter errno does not hide successful scan", count == 3);
  check("directory: comparator and result ownership",
        count == 3 && strcmp(entries[0]->d_name, "item-a") == 0 &&
        strcmp(entries[1]->d_name, "item-m") == 0 && strcmp(entries[2]->d_name, "item-z") == 0);
  directory_free_list(entries, count);
  entries = NULL;
  count = scan64(path, &entries, directory_reject, NULL);
  check("directory: empty selection succeeds", count == 0);
  directory_free_list(entries, count);

  struct dirent* sentinel_entry = NULL;
  struct dirent** sentinel = &sentinel_entry;
  entries = sentinel;
  errno = 0;
  count = scan("gesher-no-such-directory/child", &entries, NULL, NULL);
  check("directory: failed open preserves output", count == -1 && errno == ENOENT && entries == sentinel);

  if (scanat != NULL && scanat64 != NULL) {
    int parent = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    check("directory: parent descriptor", parent >= 0);
    if (parent >= 0) {
      entries = NULL;
      count = scanat(parent, path, &entries, directory_filter, directory_compare);
      check("directory: descriptor-relative scan", count == 3);
      directory_free_list(entries, count);
      check("directory: caller parent remains open", fcntl(parent, F_GETFD, 0) >= 0);
      entries = NULL;
      count = scanat64(AT_FDCWD, path, &entries, directory_filter, NULL);
      check("directory: scandirat64 AT_FDCWD", count == 3);
      directory_free_list(entries, count);
      entries = sentinel;
      errno = 0;
      count = scanat(-1, path, &entries, NULL, NULL);
      check("directory: invalid parent preserves output", count == -1 && errno == EBADF && entries == sentinel);
      close(parent);
    }
  }
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    char name[128];
    snprintf(name, sizeof(name), "%s/%s", path, names[i]);
    unlink(name);
  }
  rmdir(path);
}
