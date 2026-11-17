// Real SQLite operations. The Windows target uses the production Win32 VFS
// and DiskPath. The portable runner supplies only the Windows namespace and
// canonicalizer boundary; its file IO is SQLite's real Unix VFS.
#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include "sqlite_path_host.h"
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace gesher {
std::string Narrow(const wchar_t*);
std::wstring Widen(const char*);
}
extern "C" int gesher_sqlite_full_pathname(
    sqlite3_vfs*, const char*, int, char*, int (*)(sqlite3_vfs*, const char*, int, char*));

namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* label) {
  ++checks;
  if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
}

struct Event { std::string operation, path; int flags; std::string token; };
std::vector<Event> events;
sqlite3_vfs* native_vfs;
sqlite3_vfs observed_vfs;
constexpr const char* kVfs = "gesher-path-contract";
int callback_calls = 0;
int FailCanonicalize(sqlite3_vfs*, const char* name, int, char*) {
  ++callback_calls;
  Check(std::strcmp(name, "C:\\boundary.db") == 0, "canonicalizer sees mapped Windows path");
  return SQLITE_IOERR_FSTAT;
}

void BoundaryErrors() {
  char full[8] = "stale";
  Check(gesher_sqlite_full_pathname(&observed_vfs, "/mnt/c/boundary.db", sizeof(full), full, FailCanonicalize) == SQLITE_IOERR_FSTAT,
        "native canonicalizer's extended error is propagated unchanged");
  Check(full[0] == '\0' && full[1] == 't' && callback_calls == 1, "error output is initialized without overrunning its buffer");
  full[0] = 'x';
  Check(gesher_sqlite_full_pathname(&observed_vfs, "/mnt/c/boundary.db", 0, full, FailCanonicalize) == SQLITE_CANTOPEN && full[0] == 'x',
        "zero-capacity output is rejected without a write");
  Check(gesher_sqlite_full_pathname(&observed_vfs, "/mnt/c/boundary.db", sizeof(full), nullptr, FailCanonicalize) == SQLITE_CANTOPEN,
        "null output is rejected");
  const char* invalid[] = {nullptr, "", "/dev/null", "/mnt/unc/server"};
  for (const char* name : invalid) {
    full[0] = 'x';
    Check(gesher_sqlite_full_pathname(&observed_vfs, name, 1, full, FailCanonicalize) == SQLITE_CANTOPEN && full[0] == '\0' && full[1] == 't',
          "invalid disk path rejects before native callback and respects capacity");
  }
  Check(callback_calls == 1, "invalid inputs never invoke native canonicalization");
}

int ObserveOpen(sqlite3_vfs*, const char* path, sqlite3_file* file, int flags, int* out) {
  const char* token = (flags & SQLITE_OPEN_MAIN_DB) ? sqlite3_uri_parameter(path, "token") : nullptr;
  events.push_back({"open", path ? path : "", flags, token ? token : ""});
  // Keep SQLite's original pointer, including its filename and URI metadata.
  return native_vfs->xOpen(native_vfs, path, file, flags, out);
}
int ObserveAccess(sqlite3_vfs*, const char* path, int flags, int* out) {
  events.push_back({"access", path, flags, ""});
  return native_vfs->xAccess(native_vfs, path, flags, out);
}
int ObserveDelete(sqlite3_vfs*, const char* path, int sync) {
  events.push_back({"delete", path, sync, ""});
  return native_vfs->xDelete(native_vfs, path, sync);
}

struct Database {
  sqlite3* value = nullptr;
  Database(const std::string& path, int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI) {
    code = sqlite3_open_v2(path.c_str(), &value, flags, kVfs);
  }
  ~Database() { if (value) Check(sqlite3_close(value) == SQLITE_OK, "close database without leaked statements"); }
  int code;
  bool Sql(const char* sql) {
    const int rc = sqlite3_exec(value, sql, nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) std::fprintf(stderr, "SQL %s: %s\n", sql, sqlite3_errmsg(value));
    return rc == SQLITE_OK;
  }
  bool PathSql(const char* sql, const std::string& path) {
    sqlite3_stmt* statement = nullptr;
    int rc = sqlite3_prepare_v2(value, sql, -1, &statement, nullptr);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    const int finalize = sqlite3_finalize(statement);
    if (rc != SQLITE_DONE || finalize != SQLITE_OK)
      std::fprintf(stderr, "SQL %s path %s: %s\n", sql, path.c_str(), sqlite3_errmsg(value));
    return rc == SQLITE_DONE && finalize == SQLITE_OK;
  }
  std::string Scalar(const char* sql) {
    sqlite3_stmt* statement = nullptr;
    std::string result;
    if (sqlite3_prepare_v2(value, sql, -1, &statement, nullptr) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_ROW) {
      const auto* text = sqlite3_column_text(statement, 0);
      if (text) result = reinterpret_cast<const char*>(text);
    }
    sqlite3_finalize(statement);
    return result;
  }
};

std::string Full(const std::string& path) {
  std::vector<char> buffer(observed_vfs.mxPathname + 1, '\0');
  if (observed_vfs.xFullPathname(&observed_vfs, path.c_str(), static_cast<int>(buffer.size()), buffer.data()) != SQLITE_OK)
    return {};
  return buffer.data();
}
bool HasEvent(const char* operation, const std::string& path, int flag = 0) {
  return std::any_of(events.begin(), events.end(), [&](const Event& e) {
    return e.operation == operation && e.path == path && (!flag || (e.flags & flag) != 0);
  });
}
std::string UriPath(const std::string& path) {
  std::string result;
  constexpr char hex[] = "0123456789ABCDEF";
  for (unsigned char c : path) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '/' || c == ':' || c == '-' || c == '_' || c == '.') result += static_cast<char>(c);
    else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
  }
  return result;
}

void Exercise(const fs::path& directory, const std::string& native, const std::string& guest) {
  const auto main_native = native + "/same.db";
  const auto main_guest = guest + "/same.db";
  {
    Database db(main_native);
    Check(db.code == SQLITE_OK, "native path creates database");
    if (db.code == SQLITE_OK)
      Check(db.Sql("CREATE TABLE t(value TEXT); INSERT INTO t VALUES('original');"), "native database contains real data");
  }
  const std::string canonical = Full(main_native);
  Check(!canonical.empty(), "native full path is nonempty");
  Check(Full(main_guest) == canonical, "guest and native names have one canonical database identity");
  {
    char small[17];
    std::memset(small, 'x', sizeof(small));
    Check(observed_vfs.xFullPathname(&observed_vfs, main_guest.c_str(), 16, small) == SQLITE_CANTOPEN_FULLPATH &&
          small[0] == '\0' && small[16] == 'x', "small VFS output rejects truncation and preserves canary");
    Check(guest.size() + 16 < static_cast<size_t>(observed_vfs.mxPathname), "fixture directory leaves space for an ordinary database name");
    if (guest.size() + 16 < static_cast<size_t>(observed_vfs.mxPathname)) {
      const auto expanded = guest + "/" + std::string((observed_vfs.mxPathname - guest.size()) / 2, ':');
      Check(expanded.size() < static_cast<size_t>(observed_vfs.mxPathname), "reserved-name expansion starts below raw VFS pathname limit");
      std::vector<char> output(observed_vfs.mxPathname + 2, 'x');
      Check(observed_vfs.xFullPathname(&observed_vfs, expanded.c_str(), observed_vfs.mxPathname + 1, output.data()) == SQLITE_CANTOPEN_FULLPATH &&
            output[0] == '\0' && output.back() == 'x', "mapped UTF-8 expansion cannot publish a truncated canonical filename");
      Database too_wide(expanded);
      Check(too_wide.code == SQLITE_CANTOPEN, "mapped UTF-8 expansion retains SQLite open failure");
    }
  }
  {
    Database db(main_guest);
    Check(db.code == SQLITE_OK, "guest /mnt/<drive> opens existing directory");
    if (db.code == SQLITE_OK) {
      Check(db.Scalar("SELECT value FROM t") == "original", "guest open reads native database data");
      Check(db.Sql("PRAGMA journal_mode=DELETE; BEGIN IMMEDIATE; UPDATE t SET value='uncommitted';"), "real rollback transaction starts");
      Check(fs::exists(directory / "same.db-journal"), "rollback journal is next to mapped database");
      Check(db.Sql("ROLLBACK"), "rollback succeeds");
      Check(db.Scalar("SELECT value FROM t") == "original", "rollback preserves data");
      Check(!fs::exists(directory / "same.db-journal"), "rollback journal is removed");
    }
  }
  // Begin with a native name: this independently rejects a JNI-only or
  // first-open-only fix, since ATTACH and VACUUM enter through SQL.
  {
    Database db(main_native);
    const bool attached = db.PathSql("ATTACH DATABASE ? AS other", guest + "/attached.db");
    Check(attached, "SQL ATTACH maps its guest filename");
    if (attached) {
      Check(db.Sql("PRAGMA main.journal_mode=DELETE; PRAGMA other.journal_mode=DELETE;"
                   "PRAGMA main.synchronous=FULL; PRAGMA other.synchronous=FULL;"
                   "CREATE TABLE other.t(value TEXT); BEGIN IMMEDIATE;"
                   "UPDATE main.t SET value='committed'; INSERT INTO other.t VALUES('attached'); COMMIT;"),
            "multi-database transaction commits through super-journal");
      Check(db.Scalar("SELECT value FROM other.t") == "attached", "attached database persists its data");
      Check(db.Sql("DETACH DATABASE other"), "detach closes attached database");
      Check(fs::exists(directory / "attached.db"), "ATTACH file is in mapped directory");
      Check(std::any_of(events.begin(), events.end(), [&](const Event& e) {
        return e.operation == "open" && (e.flags & SQLITE_OPEN_SUPER_JOURNAL) && e.path.starts_with(canonical + "-mj");
      }), "SQLite generates a super-journal from the mapped main filename");
      Check(db.PathSql("ATTACH DATABASE ? AS readonly_peer", "file:" + UriPath(guest + "/attached.db") + "?mode=ro&token=attach%20metadata"),
            "ATTACH parses a guest URI through the same mapping");
      Check(sqlite3_db_readonly(db.value, "readonly_peer") == 1 &&
            sqlite3_exec(db.value, "INSERT INTO readonly_peer.t VALUES('forbidden')", nullptr, nullptr, nullptr) == SQLITE_READONLY,
            "attached read-only URI retains real write failure");
      Check(db.Sql("DETACH DATABASE readonly_peer"), "read-only URI attachment closes");
    }
    const bool vacuumed = db.PathSql("VACUUM INTO ?", guest + "/vacuum.db");
    Check(vacuumed, "SQL VACUUM INTO maps its guest filename");
    if (vacuumed) {
      Database copy(native + "/vacuum.db", SQLITE_OPEN_READONLY);
      Check(copy.code == SQLITE_OK && !copy.Scalar("SELECT value FROM t").empty(), "VACUUM copy contains real database data");
    }
  }
  {
    Database db(main_guest);
    if (db.code == SQLITE_OK) {
      Check(db.Scalar("PRAGMA journal_mode=WAL") == "wal", "WAL mode is real");
      Check(db.Sql("UPDATE t SET value='wal-data'"), "WAL commit succeeds");
      Check(fs::exists(directory / "same.db-wal"), "WAL is next to mapped database");
      Check(fs::exists(directory / "same.db-shm"), "SHM is next to mapped database");
      Database peer(main_native, SQLITE_OPEN_READONLY);
      Check(peer.code == SQLITE_OK && peer.Scalar("SELECT value FROM t") == "wal-data", "native alias reads guest WAL commit");
      Check(db.Sql("PRAGMA wal_checkpoint(TRUNCATE)"), "real WAL checkpoint succeeds");
    }
  }
  Check(!fs::exists(directory / "same.db-wal") && !fs::exists(directory / "same.db-shm"), "WAL and SHM are cleaned after last close");
  {
    const auto uri = "file:" + UriPath(main_guest) + "?mode=ro&token=kept%20metadata";
    Database db(uri);
    Check(db.code == SQLITE_OK, "URI maps decoded guest filename");
    if (db.code == SQLITE_OK) {
      Check(sqlite3_db_readonly(db.value, "main") == 1, "URI mode=ro remains read-only");
      Check(sqlite3_exec(db.value, "INSERT INTO t VALUES('forbidden')", nullptr, nullptr, nullptr) == SQLITE_READONLY,
            "read-only URI rejects writes with real SQLite error");
      const char* parameter = sqlite3_uri_parameter(sqlite3_db_filename(db.value, "main"), "token");
      Check(parameter && std::strcmp(parameter, "kept metadata") == 0, "core-owned URI metadata survives canonicalization");
      Check(std::any_of(events.begin(), events.end(), [&](const Event& e) {
        return e.operation == "open" && e.path == canonical && e.token == "kept metadata";
      }), "xOpen receives the original core-owned URI filename");
    }
    std::string slash_native = main_native;
    std::replace(slash_native.begin(), slash_native.end(), '\\', '/');
    Database native_uri("file:/" + UriPath(slash_native) + "?mode=ro");
    Check(native_uri.code == SQLITE_OK, "file:/C:/ native Windows URI spelling is retained");
  }
  for (const auto& leaf : {std::string("unicode-שלום-é.db"), std::string("reserved:name?.db")}) {
    Database db(guest + "/" + leaf);
    Check(db.code == SQLITE_OK, "Unicode and encoded reserved-name databases open");
    if (db.code == SQLITE_OK) {
      Check(db.Sql("CREATE TABLE t(value TEXT); INSERT INTO t VALUES('UTF-8');"), "Unicode database stores data");
      const auto resolved = Full(guest + "/" + leaf);
      Check(HasEvent("open", resolved, SQLITE_OPEN_MAIN_DB), "Unicode/reserved filename reaches xOpen unchanged");
      Database alias(native + "/" + leaf, SQLITE_OPEN_READONLY);
      Check(alias.code == SQLITE_OK && alias.Scalar("SELECT value FROM t") == "UTF-8", "native Unicode/reserved alias reads same data");
    }
  }
  Check(fs::exists(directory / fs::path(u8"unicode-שלום-é.db")), "UTF-8 name has expected physical spelling");
  Check(fs::exists(directory / fs::path(u8"reserved\uF03Aname\uF03F.db")), "reserved characters use existing POSIX PUA encoding");
  {
    Database encoded(native + "/reservedname.db", SQLITE_OPEN_READONLY);
    Check(encoded.code == SQLITE_OK && encoded.Scalar("SELECT value FROM t") == "UTF-8", "already mapped PUA names are not encoded twice");
    Database unicode_uri("file:" + UriPath(guest + "/unicode-שלום-é.db") + "?mode=ro");
    Check(unicode_uri.code == SQLITE_OK && unicode_uri.Scalar("SELECT value FROM t") == "UTF-8", "percent-encoded Unicode URI retains database identity");
  }
  for (const auto& path : {std::string("/dev/null"), std::string("/proc/self/status"),
                           std::string("/sys/missing"), std::string("/mnt/unc"), std::string("/mnt/unc/server")}) {
    const size_t before = events.size();
    Database db(path);
    Check(db.code == SQLITE_CANTOPEN, "virtual and malformed UNC paths retain failure");
    Check(events.size() == before, "virtual paths never reach disk VFS operations");
  }
  {
    Database missing(guest + "/absent.db", SQLITE_OPEN_READONLY);
    Check(missing.code == SQLITE_CANTOPEN, "read-only missing file is a real CANTOPEN");
    Database parent(guest + "/missing-parent/db.sqlite");
    Check(parent.code == SQLITE_CANTOPEN, "missing parent remains a native open failure");
    Database invalid("file:" + UriPath(main_guest) + "?mode=invalid");
    Check(invalid.code == SQLITE_ERROR, "invalid URI mode remains SQLite's parse error");
    Database long_name(guest + "/" + std::string(observed_vfs.mxPathname + 10, 'x'));
    Check(long_name.code == SQLITE_CANTOPEN, "overlong database pathname is not truncated into success");
  }
  const std::string unc = Full("/mnt/unc/server/share/db.sqlite");
  Check(!unc.empty() && unc == Full("\\\\server\\share\\db.sqlite"), "guest UNC canonicalization matches native UNC");
  Check(unc == Full("//server/share/db.sqlite"), "slash UNC alias retains identity");
  Check(Full("/mnt/unc/server/share/a:b") == Full("\\\\server\\share\\a:b"), "UNC leaf reserved-name encoding matches POSIX");

  const size_t before_memory = events.size();
  for (const auto& name : {std::string(":memory:"), std::string(""),
                          "file:" + UriPath(guest + "/memory.db") + "?mode=memory"}) {
    Database db(name);
    Check(db.code == SQLITE_OK, "memory and unnamed temporary databases open");
    Check(db.Sql("CREATE TABLE t(value TEXT); INSERT INTO t VALUES('memory');"), "memory/temp database performs real operations");
    Check(db.Scalar("SELECT value FROM t") == "memory", "memory/temp data can be read");
  }
  Check(events.size() == before_memory, "SQLITE_TEMP_STORE=3 and memory names do not become disk filenames");
  Check(!fs::exists(directory / "memory.db"), "memory URI creates no disk database");
  {
    const auto name = "file:" + UriPath(guest + "/shared-memory.db") + "?mode=memory&cache=shared";
    Database first(name), second(name);
    Check(first.code == SQLITE_OK && second.code == SQLITE_OK && first.Sql("CREATE TABLE shared(value); INSERT INTO shared VALUES(42)") &&
          second.Scalar("SELECT value FROM shared") == "42", "URI shared-memory identity is unchanged");
  }
  // Exercise the actual VFS's unnamed temporary-file transport as well as
  // SQLite's configured in-memory temp-store behavior above.
  auto* temporary = static_cast<sqlite3_file*>(sqlite3_malloc(observed_vfs.szOsFile));
  Check(temporary != nullptr, "temporary VFS handle allocation");
  if (temporary) {
    std::memset(temporary, 0, observed_vfs.szOsFile);
    const int rc = observed_vfs.xOpen(&observed_vfs, nullptr, temporary,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_DELETEONCLOSE | SQLITE_OPEN_TEMP_DB, nullptr);
    Check(rc == SQLITE_OK, "null VFS filename uses native temporary-file generation");
    if (rc == SQLITE_OK) {
      char data[4] = {};
      Check(temporary->pMethods->xWrite(temporary, "test", 4, 0) == SQLITE_OK &&
            temporary->pMethods->xRead(temporary, data, 4, 0) == SQLITE_OK && std::memcmp(data, "test", 4) == 0,
            "native temporary file performs real IO");
      Check(temporary->pMethods->xClose(temporary) == SQLITE_OK, "native temporary file closes and deletes");
    }
    sqlite3_free(temporary);
  }
  Check(HasEvent("open", canonical + "-journal", SQLITE_OPEN_MAIN_JOURNAL), "journal xOpen uses canonical sibling name");
  Check(HasEvent("access", canonical + "-journal"), "journal xAccess uses canonical sibling name");
  Check(HasEvent("delete", canonical + "-journal"), "journal xDelete uses canonical sibling name");
  Check(HasEvent("open", canonical + "-wal", SQLITE_OPEN_WAL), "WAL xOpen uses canonical sibling name");
  for (const auto& event : events) {
    Check(event.path.find("/mnt/") == std::string::npos && event.path.find("\\mnt\\") == std::string::npos,
          "guest drive prefix never leaks into downstream VFS IO");
  }
}
}  // namespace

int main() {
  fs::path root, directory;
  std::string native, guest;
#ifdef _WIN32
  wchar_t buffer[MAX_PATH + 1] = {};
  const DWORD length = GetTempPathW(MAX_PATH, buffer);
  if (length == 0 || length >= MAX_PATH) return 2;
  root = fs::path(buffer) / ("gesher-sqlite-path-" + std::to_string(GetCurrentProcessId()));
  directory = root / "Users" / "fixture";
  native = gesher::Narrow(directory.c_str());
  std::string slash = native;
  std::replace(slash.begin(), slash.end(), '\\', '/');
  if (slash.size() < 3 || slash[1] != ':') return 2;
  guest = "/mnt/" + std::string(1, static_cast<char>(slash[0] | 32)) + slash.substr(2);
#else
  root = fs::temp_directory_path() / ("gesher-sqlite-path-" + std::to_string(getpid()));
  directory = root / "c" / "Users" / "fixture";
  native = "C:\\Users\\fixture";
  guest = "/mnt/c/Users/fixture";
  host_root = root.string();
#endif
  fs::remove_all(root);
  fs::create_directories(directory);
  const auto original_directory = fs::current_path();
#ifndef _WIN32
  fs::create_directories(root / "d" / "work");
  fs::current_path(root / "d" / "work");
#endif
  Check(sqlite3_initialize() == SQLITE_OK, "SQLite initializes");
  native_vfs = sqlite3_vfs_find(nullptr);
  if (!native_vfs) return 2;
  observed_vfs = *native_vfs;
  observed_vfs.zName = kVfs;
  observed_vfs.pNext = nullptr;
  observed_vfs.xOpen = ObserveOpen;
  observed_vfs.xAccess = ObserveAccess;
  observed_vfs.xDelete = ObserveDelete;
#ifndef _WIN32
  observed_vfs.xFullPathname = winFullPathname;
  // SQLITE_WIN32_MAX_PATH_BYTES is four UTF-8 bytes per Windows path character.
  observed_vfs.mxPathname = 260 * 4;  // Pinned Win32 default, not Unix's 512.
#endif
  Check(sqlite3_vfs_register(&observed_vfs, 0) == SQLITE_OK, "observer VFS registers before use");
  BoundaryErrors();
  Exercise(directory, native, guest);
  Check(sqlite3_vfs_unregister(&observed_vfs) == SQLITE_OK, "observer VFS unregisters after all closes");
  Check(sqlite3_shutdown() == SQLITE_OK, "SQLite shuts down without live databases");
  fs::current_path(original_directory);
  fs::remove_all(root);
  std::printf("sqlite-paths: %d checks, %d failures (%s)\n", checks, failures,
#ifdef _WIN32
              "production Win32 VFS and POSIX mapper"
#else
              "production mapping/callback, modeled Windows canonicalizer, real Unix VFS IO"
#endif
  );
  return failures ? 1 : 0;
}
