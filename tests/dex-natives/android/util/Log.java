package android.util;

import java.io.PrintWriter;
import java.io.StringWriter;

/**
 * Stand-in for the framework class; the natives match android_util_Log.cpp,
 * and the helpers write through println_native as the real class does.
 */
public final class Log {
  // register_android_util_Log reads these.
  public static final int VERBOSE = 2;
  public static final int DEBUG = 3;
  public static final int INFO = 4;
  public static final int WARN = 5;
  public static final int ERROR = 6;
  public static final int ASSERT = 7;

  private static final int LOG_ID_MAIN = 0;

  private Log() {}

  public static int v(String tag, String msg) {
    return println_native(LOG_ID_MAIN, VERBOSE, tag, msg);
  }

  public static int d(String tag, String msg) {
    return println_native(LOG_ID_MAIN, DEBUG, tag, msg);
  }

  public static int i(String tag, String msg) {
    return println_native(LOG_ID_MAIN, INFO, tag, msg);
  }

  public static int w(String tag, String msg) {
    return println_native(LOG_ID_MAIN, WARN, tag, msg);
  }

  public static int w(String tag, String msg, Throwable tr) {
    return w(tag, msg + '\n' + getStackTraceString(tr));
  }

  public static int e(String tag, String msg) {
    return println_native(LOG_ID_MAIN, ERROR, tag, msg);
  }

  public static int e(String tag, String msg, Throwable tr) {
    return e(tag, msg + '\n' + getStackTraceString(tr));
  }

  public static int wtf(String tag, String msg) {
    return println_native(LOG_ID_MAIN, ASSERT, tag, msg);
  }

  public static int wtf(String tag, String msg, Throwable tr) {
    return wtf(tag, msg + '\n' + getStackTraceString(tr));
  }

  public static String getStackTraceString(Throwable tr) {
    if (tr == null) return "";
    StringWriter sw = new StringWriter();
    tr.printStackTrace(new PrintWriter(sw));
    return sw.toString();
  }

  public static native boolean isLoggable(String tag, int level);

  public static native int println_native(int bufID, int priority, String tag, String msg);

  static native int logger_entry_max_payload_native();

  public static int maxPayload() {
    return logger_entry_max_payload_native();
  }
}
