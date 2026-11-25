// Real public scandir families in the Windows x64 and translated ARM64 ELF
// suites, also replayed through the complete host directory implementation.
static int scandir_sort_limit;
static int scandir_sort_order;
static int scandir_sort_seen;
static int scandir_sort_last_errno;
static int scandir_sort_errno_ok;

static int scandir_sort_select(const struct dirent* entry) {
  char* end = NULL;
  if (strncmp(entry->d_name, "item-", 5) != 0) return 0;
  long value = strtol(entry->d_name + 5, &end, 10);
  errno = ENOTTY;
  return *end == '\0' && value >= 0 && value < scandir_sort_limit;
}
static int scandir_sort_value(const struct dirent* left, const struct dirent* right) {
  if (scandir_sort_order == 2) return strcmp(left->d_name, right->d_name);
  unsigned long long a = (unsigned long long)left->d_off;
  unsigned long long b = (unsigned long long)right->d_off;
  if (scandir_sort_order == 1) { a %= 7; b %= 7; }
  return scandir_sort_order == 0 ? (a < b) - (a > b) : (a > b) - (a < b);
}
static int scandir_sort_compare(const struct dirent** left, const struct dirent** right) {
  if (scandir_sort_seen && errno != scandir_sort_last_errno) scandir_sort_errno_ok = 0;
  int result = scandir_sort_value(*left, *right);
  scandir_sort_last_errno = (++scandir_sort_seen & 1) ? E2BIG : EACCES;
  errno = scandir_sort_last_errno;
  return result;
}

static void test_scandir_sort_contracts(void) {
  directory_scan_fn scan = (directory_scan_fn)dlsym(RTLD_DEFAULT, "scandir");
  directory_scan_fn scan64 = (directory_scan_fn)dlsym(RTLD_DEFAULT, "scandir64");
  directory_scanat_fn scanat = (directory_scanat_fn)dlsym(RTLD_DEFAULT, "scandirat");
  directory_scanat_fn scanat64 = (directory_scanat_fn)dlsym(RTLD_DEFAULT, "scandirat64");
  int exports = scan && scan64 && scanat && scanat64;
  check("scandir sort: all four public exports", exports);
  if (!exports) return;
  char path[] = "gesher-scandir-public-XXXXXX";
  if (mkdtemp(path) == NULL) { check("scandir sort: create real directory", 0); return; }
  int created = 0;
  for (int i = 0; i < 257; ++i) {
    char name[128];
    snprintf(name, sizeof(name), "%s/item-%05d", path, i);
    int fd = open(name, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) break;
    ++created;
    if (close(fd) != 0) break;
  }
  check("scandir sort: create 257 owned entries", created == 257);
  int parent = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  check("scandir sort: caller-owned parent descriptor", parent >= 0);
  const int sizes[] = {127, 128, 129, 257};
  if (created == 257 && parent >= 0) for (size_t size = 0; size < sizeof(sizes) / sizeof(sizes[0]); ++size) {
    scandir_sort_limit = sizes[size];
    for (scandir_sort_order = 0; scandir_sort_order < 3; ++scandir_sort_order) {
      struct dirent** expected = NULL;
      int expected_count = scan(path, &expected, scandir_sort_select, NULL);
      check("scandir sort: real unsorted reference enumeration", expected_count == scandir_sort_limit);
      if (expected_count != scandir_sort_limit) { directory_free_list(expected, expected_count); continue; }
      // Independent stable reference over the exact original enumeration.
      for (int i = 1; i < expected_count; ++i) {
        struct dirent* value = expected[i];
        int j = i;
        while (j > 0 && scandir_sort_value(expected[j - 1], value) > 0) {
          expected[j] = expected[j - 1]; --j;
        }
        expected[j] = value;
      }
      struct dirent** actual = NULL;
      scandir_sort_seen = 0; scandir_sort_errno_ok = 1; errno = EDOM;
      int family = (int)((size * 3 + (size_t)scandir_sort_order) % 4);
      int count = family == 0 ? scan(path, &actual, scandir_sort_select, scandir_sort_compare)
          : family == 1 ? scan64(path, &actual, scandir_sort_select, scandir_sort_compare)
          : family == 2 ? scanat(parent, path, &actual, scandir_sort_select, scandir_sort_compare)
          : scanat64(AT_FDCWD, path, &actual, scandir_sort_select, scandir_sort_compare);
      int result_errno = errno;
      int same = count == expected_count;
      if (same) for (int i = 0; i < count; ++i) {
        const struct dirent* a = actual[i]; const struct dirent* b = expected[i];
        if (a->d_ino != b->d_ino || a->d_off != b->d_off || a->d_type != b->d_type
                || a->d_reclen != b->d_reclen || strcmp(a->d_name, b->d_name) != 0) same = 0;
      }
      check("scandir sort: complete fields and stable equal-key order", same);
      check("scandir sort: callback and caller errno preserved",
          scandir_sort_seen > 0 && scandir_sort_errno_ok && result_errno == EDOM);
      directory_free_list(actual, count); directory_free_list(expected, expected_count);
      check("scandir sort: caller parent survives each scan", fcntl(parent, F_GETFD, 0) >= 0);
    }
  }
  if (parent >= 0) check("scandir sort: retire caller parent", close(parent) == 0);
  int cleaned = 1;
  for (int i = 0; i < created; ++i) {
    char name[128]; snprintf(name, sizeof(name), "%s/item-%05d", path, i);
    if (unlink(name) != 0) cleaned = 0;
  }
  if (rmdir(path) != 0) cleaned = 0;
  check("scandir sort: all owned entries and directory retired", cleaned);
}
