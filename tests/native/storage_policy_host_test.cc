// Host contract test for external storage path mapping and scoped-storage /
// MANAGE_EXTERNAL_STORAGE policy enforcement in gesher_posix.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>
#include <windows.h>

#include <string>
#include <gesher/storage_policy.h>

namespace {

int g_failures = 0;

void Check(bool cond, const char* msg) {
  if (!cond) {
    fprintf(stderr, "FAIL: %s (errno=%d: %s)\n", msg, errno, strerror(errno));
    ++g_failures;
  }
}

void WriteTextFile(const std::string& path, const std::string& content) {
  // The trusted host fixture publishes files independently of the application
  // whose ordinary POSIX operations are under test.
  HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  Check(file != INVALID_HANDLE_VALUE, ("host write: " + path).c_str());
  if (file != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    Check(WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr)
              && written == content.size(), "host file contents written");
    CloseHandle(file);
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

bool CheckSqlite(int result, int expected, const char* message, sqlite3* db = nullptr) {
  Check(result == expected, message);
  if (result != expected) {
    fprintf(stderr, "SQLite result=%d expected=%d: %s\n", result, expected,
            db != nullptr ? sqlite3_errmsg(db) : sqlite3_errstr(result));
  }
  return result == expected;
}

void CheckDatabaseValue(sqlite3* db) {
  sqlite3_stmt* statement = nullptr;
  if (CheckSqlite(sqlite3_prepare_v2(db, "SELECT value FROM storage_test", -1,
                                   &statement, nullptr), SQLITE_OK,
                  "SQLite: prepare SELECT from real database", db)) {
    if (CheckSqlite(sqlite3_step(statement), SQLITE_ROW,
                    "SQLite: SELECT reads committed row", db)) {
      Check(sqlite3_column_int(statement, 0) == 42, "SQLite: committed value is unchanged");
      CheckSqlite(sqlite3_step(statement), SQLITE_DONE,
                  "SQLite: denied write added no second row", db);
    }
  }
  CheckSqlite(sqlite3_finalize(statement), SQLITE_OK, "SQLite: finalize SELECT", db);
}

void SQLitePolicyLifecycle(const std::string& data) {
  constexpr const char* guest = "/sdcard/Download/storage-policy.db";
  const std::string native = data + "\\media\\0\\Download\\storage-policy.db";
  sqlite3_vfs* vfs = sqlite3_vfs_find(nullptr);
  Check(vfs != nullptr && strcmp(vfs->zName, "win32") == 0,
        "SQLite: exercise production Win32 VFS");
  if (vfs == nullptr) return;

  WritePolicy(data, "com.test.app", "MANAGER", 0);
  sqlite3* db = nullptr;
  std::string canonical;
  int result = sqlite3_open_v2(guest, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
  if (CheckSqlite(result, SQLITE_OK, "SQLite MANAGER: create shared database", db)) {
    Check(sqlite3_db_readonly(db, "main") == 0, "SQLite MANAGER: database is writable");
    CheckSqlite(sqlite3_exec(db, "PRAGMA journal_mode=DELETE;"
        "CREATE TABLE storage_test(value INTEGER); INSERT INTO storage_test VALUES(42);",
        nullptr, nullptr, nullptr), SQLITE_OK, "SQLite MANAGER: create table and commit real row", db);
    const char* filename = sqlite3_db_filename(db, "main");
    Check(filename != nullptr && *filename != '\0', "SQLite: canonical database filename available");
    if (filename != nullptr) canonical = filename;
  }
  CheckSqlite(sqlite3_close(db), SQLITE_OK, "SQLite MANAGER: close writable database");
  Check(GetFileAttributesA(native.c_str()) != INVALID_FILE_ATTRIBUTES,
        "SQLite MANAGER: database exists in mapped Download directory");

  WritePolicy(data, "com.test.app", "LEGACY_RO", 0);
  db = nullptr;
  // Request ordinary writable/create access to the existing database. The VFS
  // must preserve SQLite's readonly fallback rather than granting a write or
  // rejecting the legitimate read of the existing file.
  result = sqlite3_open_v2(guest, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
  if (CheckSqlite(result, SQLITE_OK, "SQLite LEGACY_RO: writable request falls back to read-only", db)) {
    Check(sqlite3_db_readonly(db, "main") == 1, "SQLite LEGACY_RO: connection reports read-only");
    CheckDatabaseValue(db);
    CheckSqlite(sqlite3_exec(db, "INSERT INTO storage_test VALUES(99)", nullptr, nullptr, nullptr),
                SQLITE_READONLY, "SQLite LEGACY_RO: real INSERT is denied", db);
    CheckDatabaseValue(db);
  }
  CheckSqlite(sqlite3_close(db), SQLITE_OK, "SQLite LEGACY_RO: close read-only database");

  if (!canonical.empty()) {
    // Close every database handle first: failure must come from storage policy,
    // not Windows refusing deletion because a connection still holds the file.
    int readable = 0, writable = 1;
    CheckSqlite(vfs->xAccess(vfs, canonical.c_str(), SQLITE_ACCESS_READ, &readable), SQLITE_OK,
                "SQLite LEGACY_RO: VFS read access query succeeds");
    CheckSqlite(vfs->xAccess(vfs, canonical.c_str(), SQLITE_ACCESS_READWRITE, &writable), SQLITE_OK,
                "SQLite LEGACY_RO: VFS write access query succeeds");
    Check(readable == 1 && writable == 0, "SQLite LEGACY_RO: VFS reports read-only access");
    CheckSqlite(vfs->xDelete(vfs, canonical.c_str(), 0), SQLITE_IOERR_DELETE,
                "SQLite LEGACY_RO: direct VFS delete is denied");
    Check(GetFileAttributesA(native.c_str()) != INVALID_FILE_ATTRIBUTES,
          "SQLite LEGACY_RO: denied VFS delete preserves real database");
  }

  WritePolicy(data, "com.test.app", "SCOPED", 0);
  db = nullptr;
  result = sqlite3_open_v2(guest, &db, SQLITE_OPEN_READONLY, nullptr);
  CheckSqlite(result & 0xff, SQLITE_CANTOPEN, "SQLite SCOPED: fresh shared database open denied", db);
  CheckSqlite(sqlite3_close(db), SQLITE_OK, "SQLite SCOPED: release failed-open handle");
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

  WriteTextFile(data + "\\media\\0\\Download\\doc.txt", "hello");
  WriteTextFile(data + "\\media\\0\\Pictures\\photo.jpg", "jpg");
  WriteTextFile(data + "\\media\\0\\Pictures\\clip.mp4", "mp4");
  WriteTextFile(data + "\\media\\0\\Android\\data\\com.other.app\\secret.txt", "secret");
  WriteTextFile(data + "\\media\\0\\Android\\media\\com.other.app\\song.mp3", "mp3");
  setenv("ANDROID_DATA", data.c_str(), 1);
  setenv("GESHER_APP_PACKAGE", "com.test.app", 1);
  setenv("GESHER_UID", "10001", 1);
  gesher::InitializeExternalStoragePolicy();

  // 2. Missing policy file with GESHER_APP_PACKAGE set -> fail-closed SCOPED.
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
    Check(stat("/storage/emulated/0/Android/data/com.test.app/own.txt", &st) == 0,
          "own file maps through /storage/emulated/0");
    Check(stat("/storage/self/primary/Android/data/com.test.app/own.txt", &st) == 0,
          "own file maps through /storage/self/primary");

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

  SQLitePolicyLifecycle(data);

  // 6. A running process observes the host's later permission revocation.
  WritePolicy(data, "com.test.app", "SCOPED", 0);
  errno = 0;
  Check(open("/sdcard/Download/doc.txt", O_RDONLY) < 0 && errno == EACCES,
        "revoked manager cannot read shared storage");
  int own = open("/sdcard/Android/data/com.test.app/own.txt", O_RDONLY);
  Check(own >= 0, "revocation retains own application directory");
  if (own >= 0) close(own);

  WriteTextFile(data + "\\system\\storage-policy\\com.test.app",
      "v=1\npkg=com.test.app\nuid=10001\nmode=MANAGER\n");
  errno = 0;
  Check(open("/sdcard/Download/doc.txt", O_RDONLY) < 0 && errno == EACCES,
        "incomplete snapshot retains scoped access");

  RemoveTree(root);

  if (g_failures != 0) {
    fprintf(stderr, "Total failures: %d\n", g_failures);
    return 1;
  }
  printf("storage_policy_host_test: ALL PASSED\n");
  return 0;
}
