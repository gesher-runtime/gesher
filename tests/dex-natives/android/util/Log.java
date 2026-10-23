package android.util;

/** Stand-in for the framework class; the natives match android_util_Log.cpp. */
public final class Log {
  // register_android_util_Log reads these.
  public static final int VERBOSE = 2;
  public static final int DEBUG = 3;
  public static final int INFO = 4;
  public static final int WARN = 5;
  public static final int ERROR = 6;
  public static final int ASSERT = 7;

  private Log() {}

  public static native boolean isLoggable(String tag, int level);

  public static native int println_native(int bufID, int priority, String tag, String msg);

  static native int logger_entry_max_payload_native();

  public static int maxPayload() {
    return logger_entry_max_payload_native();
  }
}
