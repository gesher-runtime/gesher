// Host contract test for external storage path mapping and scoped-storage /
// MANAGE_EXTERNAL_STORAGE policy enforcement in gesher_posix.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <windows.h>

#include <string>

namespace {

int g_failures = 0;

void Check(bool cond, const char* msg) {
  if (!cond) {
    fprintf(stderr, "FAIL: %s (errno=%d: %s)\n", msg, errno, strerror(errno));
    ++g_failures;
  }
}

void WriteTextFile(const std::string& path, const std::string& content) {
  FILE* f = fopen(path.c_str(), "wb");
  Check(f != nullptr, ("open for write: " + path).c_str());
  if (f) {
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
  }
}

void WritePolicy(const std::string& data_root, const std::string& pkg, const std::string& mode,
                 int media_mask) {
  std::string path = data_root + "/system/storage-policy/" + pkg;
  std::string body = "v=1\npkg=" + pkg + "\nuid=10001\nmode=" + mode +
                     "\nmedia_mask=" + std::to_string(media_mask) + "\n";
  WriteTextFile(path, body);
}

std::string MakeTempDir() {
  char tmp[MAX_PATH];
  DWORD n = GetTempPathA(MAX_PATH, tmp);
  Check(n > 0 && n < MAX_PATH, "GetTempPathA");
  std::string dir = std::string(tmp) + "gesher_storage_policy_test_" +
                    std::to_string(GetCurrentProcessId());
  CreateDirectoryA(dir.c_str(), nullptr);
  return dir;
}

void RemoveTree(const std::string& dir) {
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
      std::string child = dir + "\\" + fd.cFileName;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        RemoveTree(child);
      } else {
        DeleteFileA(child.c_str());
      }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
  }
  RemoveDirectoryA(dir.c_str());
}

}  // namespace

int main() {
  std::string root = MakeTempDir();
  std::string data = root + "\\data";
  CreateDirectoryA(data.c_str(), nullptr);
  CreateDirectoryA((data + "\\system").c_str(), nullptr);
  CreateDirectoryA((data + "\\system\\storage-policy").c_str(), nullptr);
  CreateDirectoryA((data + "\\media").c_str(), nullptr);
  CreateDirectoryA((data + "\\media\\0").c_str(), nullptr);
  CreateDirectoryA((data + "\\media\\0\\Android").c_str(), nullptr);
  CreateDirectoryA((data + "\\media\\0\\Android\\data").c_str(), nullptr);
  CreateDirectoryA((data + "\\media\\0\\Android\\data\\com.test.app").c_str(), nullptr);
  CreateDirectoryA((data + "\\media\\0\\Android\\data\\com.other.app").c_str(), nullptr);
  CreateDirectoryA((data + "\\media\\0\\Android\\obb").c_str(), nullptr);
  CreateDirectoryA((data + "\\media\\0\\Android\\obb\\com.other.app").c_str(), nullptr);
  CreateDirectoryA((data + "\\media\\0\\Android\\media").c_str(), nullptr);
  CreateDirectoryA((data + "\\media\\0\\Android\\media\\com.other.app").c_str(), nullptr);
  CreateDirectoryA((data + "\\media\\0\\Pictures").c_str(), nullptr);
  CreateDirectoryA((data + "\\media\\0\\Download").c_str(), nullptr);

  setenv("ANDROID_DATA", data.c_str(), 1);
  unsetenv("GESHER_APP_PACKAGE");
  unsetenv("GESHER_STORAGE_ENFORCE");

  // 1. Unset GESHER_APP_PACKAGE -> unrestricted, and path mapping works for all 3 prefixes.
  {
    int fd = open("/sdcard/Download/doc.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    Check(fd >= 0, "unrestricted create /sdcard/Download/doc.txt");
    if (fd >= 0) {
      write(fd, "hello", 5);
      close(fd);
    }
    struct stat st {};
    Check(stat("/storage/emulated/0/Download/doc.txt", &st) == 0,
          "stat /storage/emulated/0/Download/doc.txt");
    Check(stat("/storage/self/primary/Download/doc.txt", &st) == 0,
          "stat /storage/self/primary/Download/doc.txt");

    WriteTextFile(data + "\\media\\0\\Pictures\\photo.jpg", "jpg");
    WriteTextFile(data + "\\media\\0\\Pictures\\clip.mp4", "mp4");
    WriteTextFile(data + "\\media\\0\\Android\\data\\com.other.app\\secret.txt", "secret");
    WriteTextFile(data + "\\media\\0\\Android\\media\\com.other.app\\song.mp3", "mp3");
  }

  // 2. Missing policy file with GESHER_APP_PACKAGE set -> fail-closed SCOPED.
  setenv("GESHER_APP_PACKAGE", "com.test.app", 1);
  {
    struct stat st {};
    Check(stat("/sdcard", &st) == 0, "SCOPED: stat /sdcard root allowed");
    Check(stat("/sdcard/Android", &st) == 0, "SCOPED: stat /sdcard/Android allowed");
    Check(access("/sdcard", F_OK) == 0, "SCOPED: access(F_OK) /sdcard allowed");

    errno = 0;
    Check(opendir("/sdcard") == nullptr && errno == EACCES, "SCOPED: opendir /sdcard denied");
    errno = 0;
    Check(open("/sdcard/Download/doc.txt", O_RDONLY) < 0 && errno == EACCES,
          "SCOPED: read shared file denied");
    errno = 0;
    Check(open("/sdcard/Download/new.txt", O_WRONLY | O_CREAT, 0644) < 0 && errno == EACCES,
          "SCOPED: write shared file denied");

    // Own Android/data is allowed.
    int fd = open("/sdcard/Android/data/com.test.app/own.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    Check(fd >= 0, "SCOPED: write own Android/data allowed");
    if (fd >= 0) close(fd);

    // Other Android/data is denied.
    errno = 0;
    Check(open("/sdcard/Android/data/com.other.app/secret.txt", O_RDONLY) < 0 && errno == EACCES,
          "SCOPED: read other Android/data denied");
  }

  // 3. MEDIA_RO mode with images+audio (mask=5), video denied.
  WritePolicy(data, "com.test.app", "MEDIA_RO", 5);
  {
    DIR* d = opendir("/sdcard/Pictures");
    Check(d != nullptr, "MEDIA_RO: opendir /sdcard/Pictures allowed");
    if (d) closedir(d);

    int fd = open("/sdcard/Pictures/photo.jpg", O_RDONLY);
    Check(fd >= 0, "MEDIA_RO: read .jpg allowed with mask=5");
    if (fd >= 0) close(fd);

    fd = open("/sdcard/Android/media/com.other.app/song.mp3", O_RDONLY);
    Check(fd >= 0, "MEDIA_RO: read Android/media/<other> .mp3 allowed with mask=5");
    if (fd >= 0) close(fd);

    errno = 0;
    Check(open("/sdcard/Pictures/clip.mp4", O_RDONLY) < 0 && errno == EACCES,
          "MEDIA_RO: read .mp4 denied with mask=5");
    errno = 0;
    Check(open("/sdcard/Download/doc.txt", O_RDONLY) < 0 && errno == EACCES,
          "MEDIA_RO: read .txt denied");
    errno = 0;
    Check(open("/sdcard/Pictures/new.jpg", O_WRONLY | O_CREAT, 0644) < 0 && errno == EACCES,
          "MEDIA_RO: write .jpg denied");
  }

  // 4. LEGACY_RO mode.
  WritePolicy(data, "com.test.app", "LEGACY_RO", 0);
  {
    int fd = open("/sdcard/Download/doc.txt", O_RDONLY);
    Check(fd >= 0, "LEGACY_RO: read shared .txt allowed");
    if (fd >= 0) close(fd);

    errno = 0;
    Check(open("/sdcard/Download/doc.txt", O_WRONLY | O_APPEND) < 0 && errno == EACCES,
          "LEGACY_RO: write shared .txt denied");
  }

  // 5. MANAGER mode: shared RW allowed, Android/data/<other> & Android/obb/<other> & listing denied.
  WritePolicy(data, "com.test.app", "MANAGER", 0);
  {
    int fd = open("/sdcard/Download/mgr.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    Check(fd >= 0, "MANAGER: write shared file allowed");
    if (fd >= 0) close(fd);

    Check(mkdir("/sdcard/Download/sub", 0755) == 0, "MANAGER: mkdir shared allowed");
    Check(rmdir("/sdcard/Download/sub") == 0, "MANAGER: rmdir shared allowed");
    Check(rename("/sdcard/Download/mgr.txt", "/sdcard/Download/mgr2.txt") == 0,
          "MANAGER: rename shared allowed");
    Check(unlink("/sdcard/Download/mgr2.txt") == 0, "MANAGER: unlink shared allowed");

    errno = 0;
    Check(opendir("/sdcard/Android/data") == nullptr && errno == EACCES,
          "MANAGER: opendir Android/data denied");
    errno = 0;
    Check(opendir("/sdcard/Android/obb") == nullptr && errno == EACCES,
          "MANAGER: opendir Android/obb denied");
    errno = 0;
    Check(open("/sdcard/Android/data/com.other.app/secret.txt", O_RDONLY) < 0 && errno == EACCES,
          "MANAGER: read other Android/data denied");
    errno = 0;
    Check(open("/sdcard/Android/obb/com.other.app/main.obb", O_WRONLY | O_CREAT, 0644) < 0 &&
              errno == EACCES,
          "MANAGER: write other Android/obb denied");
    // Traversal with '..' cannot bypass Android/data/<other>.
    errno = 0;
    Check(open("/sdcard/Download/../Android/data/com.other.app/secret.txt", O_RDONLY) < 0 &&
              errno == EACCES,
          "MANAGER: .. traversal into other Android/data denied");
  }

  // 6. GESHER_STORAGE_ENFORCE=0 disables enforcement.
  setenv("GESHER_STORAGE_ENFORCE", "0", 1);
  {
    int fd = open("/sdcard/Android/data/com.other.app/secret.txt", O_RDONLY);
    Check(fd >= 0, "GESHER_STORAGE_ENFORCE=0: allows other Android/data");
    if (fd >= 0) close(fd);
  }

  unsetenv("GESHER_STORAGE_ENFORCE");
  unsetenv("GESHER_APP_PACKAGE");
  RemoveTree(root);

  if (g_failures != 0) {
    fprintf(stderr, "Total failures: %d\n", g_failures);
    return 1;
  }
  printf("storage_policy_host_test: ALL PASSED\n");
  return 0;
}
