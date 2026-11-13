// Android's ftw/nftw contracts over the Windows-backed POSIX filesystem.
// Only ancestors enter the identity set: completed aliases are visited again.
#include <ftw.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <utility>
#include <vector>
namespace {
using NftwFn = int (*)(const char*, const struct stat*, int, struct FTW*);
using FtwFn = int (*)(const char*, const struct stat*, int);
struct WalkState {
  NftwFn nftw_fn;
  FtwFn ftw_fn;
  int flags;
  bool change_directory;
  std::string original_directory;
  dev_t root_device = 0;
  std::vector<std::pair<dev_t, ino_t>> ancestors;
  int original_directory_fd = -1;
  bool RestoreInitialDirectory() {
    // Gesher's getcwd omits the current drive. A physical walk can follow a
    // command-line link onto another volume, where that same text would name
    // a different directory. Resolve every access path from the owned initial
    // directory handle before changing cwd for the callback.
    return !change_directory || fchdir(original_directory_fd) == 0;
  }
  int Call(const std::string& path, const std::string& parent, const struct stat& sb,
           int type, int level) {
    // FTS_LOGICAL implies FTS_NOCHDIR in Android. Physical postorder callbacks
    // run in the parent directory, just like preorder callbacks.
    if (!RestoreInitialDirectory()) return -1;
    if (change_directory && level != 0 && chdir(parent.c_str()) != 0) return -1;
    size_t slash = path.find_last_of('/');
    int base = path == "/" || slash == std::string::npos ? 0 : static_cast<int>(slash + 1);
    struct FTW info = {base, level};
    if (nftw_fn) return nftw_fn(path.c_str(), &sb, type, &info);
    return ftw_fn(path.c_str(), &sb, type == FTW_SLN ? FTW_NS : type);
  }
  int Walk(const std::string& path, const std::string& access_path,
           const std::string& parent, int level) {
    if (!RestoreInitialDirectory()) return -1;
    struct stat sb{};
    // Android uses FTS_COMFOLLOW even for FTW_PHYS.
    bool follow = (flags & FTW_PHYS) == 0 || level == 0;
    int type;
    if ((follow ? stat(access_path.c_str(), &sb) : lstat(access_path.c_str(), &sb)) != 0) {
      int saved = errno;
      // FTS_SLNONE includes ELOOP and other failed follows, not only ENOENT.
      type = follow && lstat(access_path.c_str(), &sb) == 0 ? FTW_SLN : FTW_NS;
      if (type == FTW_NS) memset(&sb, 0, sizeof(sb));
      errno = type == FTW_SLN ? 0 : saved;
      if (level == 0 && type == FTW_NS) return -1;
    } else if (S_ISLNK(sb.st_mode)) type = FTW_SL;
    else if (S_ISDIR(sb.st_mode)) type = FTW_D;
    else type = FTW_F;
    if (type != FTW_D) return Call(path, parent, sb, type, level);
    const auto identity = std::make_pair(sb.st_dev, sb.st_ino);
    for (const auto& ancestor : ancestors) if (ancestor == identity) return 0;
    if (level == 0) root_device = sb.st_dev;
    bool depth = (flags & FTW_DEPTH) != 0;
    if ((flags & FTW_MOUNT) && sb.st_dev != root_device)
      return Call(path, parent, sb, depth ? FTW_DP : FTW_D, level);
    DIR* dir = opendir(access_path.c_str());
    if (!dir) return Call(path, parent, sb, FTW_DNR, level);
    if (!depth) {
      int result = Call(path, parent, sb, FTW_D, level);
      if (result != 0) { int saved = errno; closedir(dir); errno = saved; return result; }
    }
    // Close each enumeration before descending. Android validates positive nfds
    // but otherwise ignores that value; here only one enumeration stays open.
    std::vector<std::string> names;
    int read_error = 0;
    for (;;) {
      errno = 0;
      struct dirent* entry = readdir(dir);
      if (!entry) { read_error = errno; break; }
      if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
        names.emplace_back(entry->d_name);
    }
    int close_result = closedir(dir);
    if (read_error != 0) return errno = read_error, -1;
    if (close_result != 0) return -1;
    ancestors.push_back(identity);
    for (const std::string& name : names) {
      std::string child = path + (path.back() == '/' ? "" : "/") + name;
      std::string access_child = access_path + (access_path.back() == '/' ? "" : "/") + name;
      int result = Walk(child, access_child, access_path, level + 1);
      if (result != 0) { ancestors.pop_back(); return result; }
    }
    ancestors.pop_back();
    return depth ? Call(path, parent, sb, FTW_DP, level) : 0;
  }
};
int WalkTree(const char* directory, NftwFn nftw_fn, FtwFn ftw_fn, int nfds, int flags) {
  if (!directory || (!nftw_fn && !ftw_fn) || nfds < 1) return errno = EINVAL, -1;
  if (*directory == '\0') return errno = ENOENT, -1;
  WalkState walk{nftw_fn, ftw_fn, flags,
                 (flags & (FTW_PHYS | FTW_CHDIR)) == (FTW_PHYS | FTW_CHDIR), {}, 0, {}};
  std::string path(directory);
  std::string access_path = path;
  int saved_cwd = -1;
  if (walk.change_directory) {
    char* cwd = getcwd(nullptr, 0);
    if (!cwd) return -1;
    walk.original_directory = cwd;
    free(cwd);
    saved_cwd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (saved_cwd < 0) return -1;
    walk.original_directory_fd = saved_cwd;
    bool absolute = path.front() == '/' || path.front() == '\\' ||
                    (path.size() > 1 && path[1] == ':');
    if (!absolute) access_path = walk.original_directory + "/" + path;
  }
  int result = walk.Walk(path, access_path, walk.original_directory, 0);
  int saved = errno;
  if (saved_cwd >= 0) {
    int restored = fchdir(saved_cwd);
    int restore_error = errno;
    int closed = close(saved_cwd);
    if (result == 0 && (restored != 0 || closed != 0)) {
      if (restored != 0) errno = restore_error;
      return -1;
    }
  }
  errno = saved;
  return result;
}
}  // namespace
extern "C" int nftw(const char* dir, NftwFn fn, int nfds, int flags) {
  // Android translates only the four POSIX flags; other bits are ignored.
  return WalkTree(dir, fn, nullptr, nfds, flags);
}
extern "C" int ftw(const char* dir, FtwFn fn, int nfds) {
  return WalkTree(dir, nullptr, fn, nfds, 0);
}
