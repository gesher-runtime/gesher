// Ordinary public traversal fixtures, shared by the host and both Android ABIs.
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef TRAVERSAL_NFTW
#define TRAVERSAL_NFTW nftw
#define TRAVERSAL_FTW ftw
#endif
static int traversal_failures, traversal_checks;
static void traversal_check(const char* message, int ok) {
  ++traversal_checks;
  if (!ok) { ++traversal_failures; printf("FAIL traversal: %s (errno=%d)\n", message, errno); }
}
struct traversal_row { char path[1024]; int type, base, level; struct stat sb, cwd; };
static struct traversal_row traversal_rows[96];
static int traversal_count, traversal_stop, traversal_nested, traversal_inner_calls;
static const char* traversal_inner_path;
static int traversal_inner(const char* p, const struct stat* s, int t) {
  (void)p; (void)s; (void)t; ++traversal_inner_calls; return 29;
}
static int traversal_record(const char* path, const struct stat* sb, int type, struct FTW* info) {
  if (traversal_count >= 96) { traversal_check("bounded fixture traversal terminates", 0); return 98; }
  struct traversal_row* row = &traversal_rows[traversal_count++];
  snprintf(row->path, sizeof(row->path), "%s", path);
  row->type = type; row->base = info->base; row->level = info->level; row->sb = *sb;
  traversal_check("callback cwd can be inspected", stat(".", &row->cwd) == 0);
  if (traversal_nested) {
    traversal_nested = 0;
    traversal_check("nested ftw preserves outer callback", TRAVERSAL_FTW(traversal_inner_path, traversal_inner, 1) == 29);
  }
  return traversal_stop;
}
static int traversal_record_ftw(const char* p, const struct stat* s, int t) {
  struct FTW info = {0, 0}; return traversal_record(p, s, t, &info);
}
static int traversal_find(const char* path) {
  for (int i = 0; i < traversal_count; ++i) if (!strcmp(path, traversal_rows[i].path)) return i;
  return -1;
}
static int traversal_same(const struct stat* a, const struct stat* b) {
  return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}
static int traversal_run(void) {
  char root[] = "gesher-traversal-XXXXXX";
  traversal_check("create isolated directory", mkdtemp(root) != NULL);
  if (traversal_failures) return traversal_failures;
  char sub[1024], leaf[1024], file[1024], alias[1024], up[1024], bad[1024], loop[1024], command[1024];
  snprintf(sub, sizeof(sub), "%s/sub", root); snprintf(leaf, sizeof(leaf), "%s/sub/leaf", root);
  snprintf(file, sizeof(file), "%s/file", root); snprintf(alias, sizeof(alias), "%s/alias", root);
  snprintf(up, sizeof(up), "%s/sub/up", root); snprintf(bad, sizeof(bad), "%s/dangling", root);
  snprintf(loop, sizeof(loop), "%s/loop", root); snprintf(command, sizeof(command), "%s-command", root);
  traversal_check("create child directory", mkdir(sub, 0700) == 0);
  int fd = open(file, O_CREAT | O_WRONLY | O_TRUNC, 0600);
  traversal_check("create file", fd >= 0);
  if (fd >= 0) { traversal_check("write metadata fixture", write(fd, "hello", 5) == 5); close(fd); }
  fd = open(leaf, O_CREAT | O_WRONLY | O_TRUNC, 0600);
  traversal_check("create nested file", fd >= 0); if (fd >= 0) close(fd);
  traversal_check("create directory alias", symlink("sub", alias) == 0);
  traversal_check("create ancestor link", symlink("..", up) == 0);
  traversal_check("create dangling link", symlink("absent", bad) == 0);
  traversal_check("create unresolved self link", symlink("loop", loop) == 0);
  traversal_check("create command-line link", symlink(root, command) == 0);
  struct stat original, root_stat, sub_stat, file_stat;
  stat(".", &original); stat(root, &root_stat); stat(sub, &sub_stat); stat(file, &file_stat);
  for (int flags = 0; flags < 16; ++flags) {
    traversal_count = 0; traversal_stop = 0;
    traversal_check("all POSIX flag combinations finish", TRAVERSAL_NFTW(root, traversal_record, 1, flags) == 0);
    traversal_check("alias/cycle filtering produces bounded complete visit set", traversal_count == 8);
    int ri = traversal_find(root), si = traversal_find(sub), fi = traversal_find(file), li = traversal_find(leaf);
    traversal_check("all regular entries visited", ri >= 0 && si >= 0 && fi >= 0 && li >= 0);
    if (ri >= 0 && si >= 0 && fi >= 0 && li >= 0) {
      traversal_check("preorder/postorder root ordering", flags & FTW_DEPTH ? ri > fi && ri > li : ri < fi && ri < li);
      traversal_check("directory order and type", flags & FTW_DEPTH ? si > li && traversal_rows[si].type == FTW_DP : si < li && traversal_rows[si].type == FTW_D);
      traversal_check("correct levels and basename", traversal_rows[ri].level == 0 && traversal_rows[li].level == 2 && !strcmp(traversal_rows[li].path + traversal_rows[li].base, "leaf"));
      traversal_check("callback stat preserves identity and size", traversal_same(&traversal_rows[fi].sb, &file_stat) && traversal_rows[fi].sb.st_size == 5 && traversal_rows[fi].sb.st_mode == file_stat.st_mode);
      int change = (flags & (FTW_PHYS | FTW_CHDIR)) == (FTW_PHYS | FTW_CHDIR);
      traversal_check("root callback cwd matches Android", traversal_same(&traversal_rows[ri].cwd, &original));
      traversal_check("file callback cwd matches Android", traversal_same(&traversal_rows[li].cwd, change ? &sub_stat : &original));
      traversal_check("directory callback cwd is parent even postorder", traversal_same(&traversal_rows[si].cwd, change ? &root_stat : &original));
    }
    int bi = traversal_find(bad), ui = traversal_find(up), ai = traversal_find(alias), oi = traversal_find(loop);
    traversal_check("unresolved links retained", bi >= 0 && oi >= 0);
    if (bi >= 0 && oi >= 0) traversal_check("unresolved links classified", traversal_rows[bi].type == ((flags & FTW_PHYS) ? FTW_SL : FTW_SLN) && traversal_rows[oi].type == traversal_rows[bi].type);
    traversal_check("ancestor links skipped only for logical traversal", (flags & FTW_PHYS) ? ui >= 0 : ui < 0);
    traversal_check("completed directory aliases are revisited", ai >= 0);
    struct stat after; stat(".", &after); traversal_check("cwd restored after walk", traversal_same(&after, &original));
  }
  traversal_count = 0; traversal_stop = 19;
  traversal_check("callback early return preserved", TRAVERSAL_NFTW(root, traversal_record, 1, FTW_PHYS | FTW_CHDIR) == 19 && traversal_count == 1);
  traversal_count = 0; traversal_stop = 0;
  traversal_check("command-line directory link follows even PHYS", TRAVERSAL_NFTW(command, traversal_record, 1, FTW_PHYS) == 0 && traversal_count == 8 && traversal_rows[0].type == FTW_D);
  traversal_count = 0;
  traversal_check("ftw reports failed logical follows as FTW_NS", TRAVERSAL_FTW(loop, traversal_record_ftw, 1) == 0 && traversal_count == 1 && traversal_rows[0].type == FTW_NS);
  traversal_count = 0; traversal_nested = 1; traversal_inner_path = file; traversal_inner_calls = 0;
  traversal_check("nested ftw callback state restored", TRAVERSAL_FTW(root, traversal_record_ftw, 1) == 0 && traversal_count == 8 && traversal_inner_calls == 1);
  traversal_count = 0; errno = 0;
  traversal_check("nfds must be positive", TRAVERSAL_NFTW(root, traversal_record, 0, 0) == -1 && errno == EINVAL && traversal_count == 0);
  traversal_count = 0; errno = 0;
  traversal_check("absent root fails without callback", TRAVERSAL_NFTW("gesher-no-such-traversal-root", traversal_record, 1, 0) == -1 && errno == ENOENT && traversal_count == 0);
  traversal_count = 0; traversal_stop = 23;
  traversal_check("slash root basename is zero", TRAVERSAL_NFTW("/", traversal_record, 1, FTW_PHYS) == 23 && traversal_count == 1 && traversal_rows[0].base == 0);
  char trailing[1024]; snprintf(trailing,sizeof(trailing),"%s/",root);
  traversal_count = 0;
  traversal_check("trailing slash path and empty basename preserved", TRAVERSAL_NFTW(trailing,traversal_record,1,FTW_PHYS) == 23 && traversal_count == 1 && !strcmp(traversal_rows[0].path,trailing) && traversal_rows[0].base == (int)strlen(trailing));
  traversal_stop = 0;
  const char* second = getenv("GESHER_TRAVERSAL_SECOND_ROOT");
  traversal_check("second-volume fixture is configured", second != NULL);
  if (second) {
    char external[1024], external_file[1100], cross[1024], cross_file[1024];
    snprintf(external, sizeof(external), "%s/gesher-traversal-volume-XXXXXX", second);
    traversal_check("create owned second-volume directory", mkdtemp(external) != NULL);
    struct stat external_stat;
    traversal_check("fixture actually spans devices", stat(external, &external_stat) == 0 && external_stat.st_dev != root_stat.st_dev);
    snprintf(external_file, sizeof(external_file), "%s/leaf", external);
    snprintf(cross, sizeof(cross), "%s/cross", root); snprintf(cross_file, sizeof(cross_file), "%s/cross/leaf", root);
    fd = open(external_file, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    traversal_check("create second-volume leaf", fd >= 0); if (fd >= 0) close(fd);
    traversal_check("create volume boundary link", symlink(external, cross) == 0);
    for (int depth = 0; depth < 2; ++depth) {
      traversal_count = 0;
      traversal_check("FTW_MOUNT traversal succeeds", TRAVERSAL_NFTW(root, traversal_record, 1, FTW_MOUNT | (depth ? FTW_DEPTH : 0)) == 0);
      int ci = traversal_find(cross);
      traversal_check("mount boundary is reported", ci >= 0 && traversal_rows[ci].type == (depth ? FTW_DP : FTW_D));
      traversal_check("mount boundary is not descended", traversal_find(cross_file) < 0);
    }
    traversal_count = 0;
    traversal_check("without mount flag, linked volume is traversed", TRAVERSAL_NFTW(root, traversal_record, 1, 0) == 0 && traversal_find(cross_file) >= 0);
    unlink(cross); unlink(external_file); rmdir(external);
  }
  unlink(command); unlink(up); unlink(alias); unlink(bad); unlink(loop); unlink(leaf); unlink(file); rmdir(sub); rmdir(root);
  printf("traversal_contract: %d checks, %d failures\n", traversal_checks, traversal_failures);
  if (!traversal_failures) puts("traversal_contract: all passed");
  return traversal_failures;
}
