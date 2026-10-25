// gesher-base: ftw and nftw (<ftw.h>), over opendir, readdir and stat/lstat.
// FTW_CHDIR and FTW_MOUNT are accepted; the walk does not change directory and
// does not stop at volume boundaries.
#include <ftw.h>

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include <string>

namespace {

using NftwFn = int (*)(const char*, const struct stat*, int, struct FTW*);

int Walk(std::string& path, int level, NftwFn fn, int flags) {
  struct stat st;
  bool phys = (flags & FTW_PHYS) != 0;
  int type;
  if ((phys ? lstat(path.c_str(), &st) : stat(path.c_str(), &st)) != 0) {
    if (errno == ENOENT && !phys && lstat(path.c_str(), &st) == 0) {
      type = FTW_SLN;  // a link to nothing
    } else {
      if (level == 0) return -1;
      memset(&st, 0, sizeof(st));
      type = FTW_NS;
    }
  } else if (S_ISLNK(st.st_mode)) {
    type = FTW_SL;
  } else if (S_ISDIR(st.st_mode)) {
    type = FTW_D;
  } else {
    type = FTW_F;
  }
  size_t slash = path.find_last_of('/', path.size() > 1 ? path.size() - 2 : 0);
  struct FTW ftw = {slash == std::string::npos ? 0 : static_cast<int>(slash + 1), level};
  bool action = (flags & FTW_ACTIONRETVAL) != 0;
  if (type != FTW_D) return fn(path.c_str(), &st, type, &ftw);

  DIR* dir = opendir(path.c_str());
  if (dir == nullptr) return fn(path.c_str(), &st, FTW_DNR, &ftw);
  int r = 0;
  if ((flags & FTW_DEPTH) == 0) {
    r = fn(path.c_str(), &st, FTW_D, &ftw);
    if (action && r == FTW_SKIP_SUBTREE) {
      closedir(dir);
      return 0;
    }
    if (r != 0) {
      closedir(dir);
      return r;
    }
  }
  size_t length = path.size();
  while (struct dirent* e = readdir(dir)) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    if (path.empty() || path.back() != '/') path += '/';
    path += e->d_name;
    r = Walk(path, level + 1, fn, flags);
    path.resize(length);
    if (action && r == FTW_SKIP_SIBLINGS) {
      r = 0;
      break;
    }
    if (r != 0) {
      closedir(dir);
      return r;
    }
  }
  closedir(dir);
  if ((flags & FTW_DEPTH) != 0) r = fn(path.c_str(), &st, FTW_DP, &ftw);
  return r;
}

// ftw's callback, reached through nftw's.
thread_local int (*g_ftw_fn)(const char*, const struct stat*, int) = nullptr;

int FtwAdapter(const char* path, const struct stat* sb, int flag, struct FTW*) {
  return g_ftw_fn(path, sb, flag == FTW_SL || flag == FTW_SLN ? FTW_F : flag);
}

}  // namespace

extern "C" {

int nftw(const char* dir, NftwFn fn, int, int flags) {
  if (dir == nullptr || fn == nullptr) return errno = EINVAL, -1;
  std::string path = dir;
  int r = Walk(path, 0, fn, flags);
  return (flags & FTW_ACTIONRETVAL) != 0 && r == FTW_SKIP_SUBTREE ? 0 : r;
}

int ftw(const char* dir, int (*fn)(const char*, const struct stat*, int), int nopenfd) {
  auto saved = g_ftw_fn;
  g_ftw_fn = fn;
  int r = nftw(dir, FtwAdapter, nopenfd, 0);
  g_ftw_fn = saved;
  return r;
}

}  // extern "C"
