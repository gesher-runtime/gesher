# SQLite and android.database's natives (docs/e2e.md): what SQLiteDatabase,
# SQLiteOpenHelper, Room and ContentProviders over them need.
#
# libsqlite mirrors external/sqlite/dist/Android.bp's host configuration
# (sqlite-default, which is sqlite-autoconf-3500400 in android-16.0.0_r4, with
# SQLITE_ENABLE_ICU and libsqlite3_android's functions and collators), built as
# Soong builds it for Windows hosts: SQLite's own Win32 VFS (os_win.c), so
# databases are files opened with CreateFileW, and Linux paths are paths on
# the current drive, as everywhere in Gesher (ADR 0010). No gesher-base here.
#
# frameworks/base's android_database_*.cpp go into libandroid_runtime
# (framework_runtime), their JNI tables made System V by
# patches/frameworks/base/0200-e2e-sqlite-sysv-jni-tables.patch
# (tools/jni-sysv-patch.py <checkout> sqlite).

set(_sqlite "${AOSP_DIR}/external/sqlite")
set(_sqlite_dist "${_sqlite}/dist/sqlite-autoconf-3500400")

add_library(sqlite STATIC
  ${_sqlite_dist}/sqlite3.c
  # libsqlite3_android
  ${_sqlite}/android/PhoneNumberUtils.cpp
  ${_sqlite}/android/OldPhoneNumberUtils.cpp
  ${_sqlite}/android/sqlite3_android.cpp)
target_include_directories(sqlite PUBLIC ${_sqlite_dist} ${_sqlite}/android)
target_link_libraries(sqlite PRIVATE log icu_headers icui18n icuuc)
# sqlite-minimal-defaults and sqlite-defaults (host), libsqlite's host ICU,
# libsqlite3_android_defaults.
target_compile_definitions(sqlite PRIVATE
  NDEBUG=1 HAVE_USLEEP=1 SQLITE_HAVE_ISNAN SQLITE_DEFAULT_JOURNAL_SIZE_LIMIT=1048576
  SQLITE_THREADSAFE=2 SQLITE_TEMP_STORE=3 SQLITE_POWERSAFE_OVERWRITE=1
  SQLITE_DEFAULT_FILE_FORMAT=4 SQLITE_DEFAULT_AUTOVACUUM=1 SQLITE_ENABLE_DBSTAT_VTAB
  SQLITE_ENABLE_MEMORY_MANAGEMENT=1 SQLITE_ENABLE_FTS3 SQLITE_ENABLE_FTS3_BACKWARDS
  SQLITE_ENABLE_FTS4 SQLITE_OMIT_BUILTIN_TEST SQLITE_OMIT_COMPILEOPTION_DIAGS
  SQLITE_OMIT_LOAD_EXTENSION SQLITE_DEFAULT_FILE_PERMISSIONS=0600 SQLITE_SECURE_DELETE
  SQLITE_ENABLE_BATCH_ATOMIC_WRITE SQLITE_DEFAULT_LEGACY_ALTER_TABLE
  SQLITE_ALLOW_ROWID_IN_VIEW SQLITE_ENABLE_BYTECODE_VTAB SQLITE_ENABLE_ICU)
target_compile_options(sqlite PRIVATE -Wno-unused-parameter -ftrivial-auto-var-init=pattern
  $<$<COMPILE_LANGUAGE:CXX>:-Wall -Werror -Wno-unused-function -Wno-unused-variable>)

target_sources(framework_runtime PRIVATE
  ${_fwjni}/android_database_CursorWindow.cpp
  ${_fwjni}/android_database_SQLiteCommon.cpp
  ${_fwjni}/android_database_SQLiteConnection.cpp
  ${_fwjni}/android_database_SQLiteDebug.cpp
  ${_fwjni}/android_database_SQLiteGlobal.cpp
  ${_fwjni}/android_database_SQLiteRawStatement.cpp)
target_link_libraries(framework_runtime PUBLIC sqlite)
# libandroid_runtime's flags are -Wall -Werror (no -Wextra, which framework_runtime adds).
set_source_files_properties(
  ${_fwjni}/android_database_CursorWindow.cpp ${_fwjni}/android_database_SQLiteCommon.cpp
  ${_fwjni}/android_database_SQLiteConnection.cpp ${_fwjni}/android_database_SQLiteDebug.cpp
  ${_fwjni}/android_database_SQLiteGlobal.cpp ${_fwjni}/android_database_SQLiteRawStatement.cpp
  PROPERTIES COMPILE_OPTIONS "-Wno-unused-function;-Wno-sign-compare;-Wno-missing-field-initializers;-Wno-implicit-fallthrough;-Wno-unused-but-set-variable")
