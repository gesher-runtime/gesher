package gesher;

import java.io.File;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.Arrays;

/** Actual boot-framework SQLite JNI with a new synthetic on-disk database. */
public final class FrameworkSQLiteRawStatementTest {
  interface Action { void run() throws Exception; }
  private static Object database;
  private static String selected;
  private static int checks;
  private static int cases;

  private static void check(boolean value, String message) {
    if (!value) throw new AssertionError(message);
    checks++;
  }

  private static Object call(Object target, String name, Object... args) throws Exception {
    Class<?>[] types = new Class<?>[args.length];
    for (int i = 0; i < args.length; i++) {
      Object arg = args[i];
      types[i] = arg instanceof Integer ? int.class : arg instanceof Long ? long.class
          : arg instanceof Double ? double.class : arg.getClass();
    }
    try {
      return target.getClass().getMethod(name, types).invoke(target, args);
    } catch (InvocationTargetException wrapped) {
      Throwable cause = wrapped.getCause();
      if (cause instanceof Exception) throw (Exception) cause;
      throw (Error) cause;
    }
  }

  private static void expect(String type, Action action) throws Exception {
    try { action.run(); }
    catch (Exception error) {
      if (Class.forName(type).isInstance(error)) { checks++; return; }
      throw error;
    }
    throw new AssertionError("expected " + type);
  }

  private static Object raw(String sql) throws Exception {
    Object statement = call(database, "createRawStatement", sql);
    check(statement.getClass().getClassLoader() == Object.class.getClassLoader(),
        "actual boot SQLiteRawStatement");
    return statement;
  }

  private static void close(Object object) throws Exception { ((AutoCloseable) object).close(); }

  private static void run(String name, Action action) throws Exception {
    if (selected != null && !selected.equals(name)) return;
    System.out.println("SQLiteRawStatement case " + name);
    call(database, "beginTransaction");
    try {
      action.run();
      call(database, "setTransactionSuccessful");
      cases++;
    } finally {
      call(database, "endTransaction");
    }
  }

  public static void main(String[] args) throws Exception {
    selected = args.length == 0 ? null : args[0];
    System.loadLibrary("android_runtime");
    Class.forName("android.content.res.AssetManager").getMethod(
        "createSystemAssetsInZygoteLocked", boolean.class, String.class).invoke(
            null, false, System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
    File directory = new File(System.getenv("ANDROID_DATA"));
    check(directory.isDirectory(), "isolated data directory exists");
    File file = File.createTempFile("raw-statement-", ".db", directory);
    check(file.length() == 0, "fresh empty synthetic database");
    Class<?> db = Class.forName("android.database.sqlite.SQLiteDatabase");
    database = db.getMethod("openOrCreateDatabase", File.class,
        Class.forName("android.database.sqlite.SQLiteDatabase$CursorFactory")).invoke(null, file, null);
    try {
      call(database, "execSQL", "CREATE TABLE synthetic (value TEXT)");
      run("legacy-text-control", () -> {
        Object insert = call(database, "compileStatement", "INSERT INTO synthetic VALUES (?)");
        try {
          call(insert, "bindString", 1, "legacy synthetic");
          check((Long) call(insert, "executeInsert") > 0, "legacy insert");
        } finally { close(insert); }
        Object query = call(database, "compileStatement", "SELECT value FROM synthetic LIMIT 1");
        try { check("legacy synthetic".equals(call(query, "simpleQueryForString")), "legacy text roundtrip"); }
        finally { close(query); }
      });
      run("metadata-pointer-width", () -> {
        Object query = raw("SELECT :value");
        try {
          Field pointer = query.getClass().getDeclaredField("mStatement");
          pointer.setAccessible(true);
          check((pointer.getLong(query) >>> 32) != 0, "native statement exercises pointer bits above32");
          check((Integer) call(query, "getParameterCount") == 1, "parameter count preserves pointer");
          check((Integer) call(query, "getParameterIndex", ":value") == 1, "named parameter index");
          check(":value".equals(call(query, "getParameterName", 1)), "named parameter name");
        } finally { close(query); }
      });
      run("constant-query", () -> {
        Object query = raw("SELECT 7, 'synthetic'");
        try {
          check((Boolean) call(query, "step"), "constant result row");
          check((Integer) call(query, "getColumnInt", 0) == 7, "constant integer");
          check("synthetic".equals(call(query, "getColumnText", 1)), "constant text");
          check(!(Boolean) call(query, "step"), "constant query complete");
        } finally { close(query); }
      });
      run("insert-text-roundtrip", () -> {
        String text = "raw synthetic \u05d0\u05d1\u05d2 \uD83D\uDE00";
        Object insert = raw("INSERT INTO synthetic VALUES (?)");
        try {
          call(insert, "bindText", 1, text);
          check(!(Boolean) call(insert, "step"), "raw insert completes");
        } finally { close(insert); }
        Object query = raw("SELECT value FROM synthetic ORDER BY rowid DESC LIMIT 1");
        try {
          check((Boolean) call(query, "step"), "inserted row available");
          check(text.equals(call(query, "getColumnText", 0)), "UTF16 text roundtrip");
          check(!(Boolean) call(query, "step"), "one inserted row");
        } finally { close(query); }
      });
      run("bind-types-reset-and-clear", () -> {
        Object query = raw("SELECT ?, ?, ?, ?, ?, ?");
        try {
          byte[] blob = new byte[] {0, 1, -1, 65};
          call(query, "bindNull", 1);
          call(query, "bindInt", 2, -73);
          call(query, "bindLong", 3, 0x12345678fedcba98L);
          call(query, "bindDouble", 4, 1.25);
          call(query, "bindBlob", 5, blob);
          call(query, "bindText", 6, "");
          check((Boolean) call(query, "step"), "bound result row");
          check((Integer) call(query, "getColumnType", 0) == 5, "bound null");
          check((Integer) call(query, "getColumnInt", 1) == -73, "bound int");
          check((Long) call(query, "getColumnLong", 2) == 0x12345678fedcba98L, "bound long keeps high bits");
          check((Double) call(query, "getColumnDouble", 3) == 1.25, "bound double");
          check(Arrays.equals(blob, (byte[]) call(query, "getColumnBlob", 4)), "bound blob");
          check("".equals(call(query, "getColumnText", 5)), "empty text");
          check(!(Boolean) call(query, "step"), "bound query complete");
          call(query, "reset");
          call(query, "clearBindings");
          check((Boolean) call(query, "step"), "query reusable after reset");
          for (int i = 0; i < 6; i++) check((Integer) call(query, "getColumnType", i) == 5, "bindings cleared");
        } finally { close(query); }
      });
      run("parameter-and-column-bounds", () -> {
        Object query = raw("SELECT ?");
        try {
          String range = "android.database.sqlite.SQLiteBindOrColumnIndexOutOfRangeException";
          expect(range, () -> call(query, "bindText", 0, "synthetic"));
          expect(range, () -> call(query, "bindText", 2, "synthetic"));
          call(query, "bindText", 1, "valid");
          check((Boolean) call(query, "step"), "valid bind after bounds error");
          expect(range, () -> call(query, "getColumnText", -1));
          expect(range, () -> call(query, "getColumnText", 1));
          check("valid".equals(call(query, "getColumnText", 0)), "error handling retains valid pointer");
        } finally { close(query); }
      });
      run("closed-statement", () -> {
        Object query = raw("SELECT ?");
        close(query);
        close(query);
        expect("java.lang.IllegalStateException", () -> call(query, "bindText", 1, "synthetic"));
      });
    } finally { close(database); }
    check(cases == (selected == null ? 7 : 1), "all selected cases executed");
    System.out.println("FrameworkSQLiteRawStatementTest all passed: " + checks);
  }
}
