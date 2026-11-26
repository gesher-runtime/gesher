package android.util;
public final class Log {
    public static final int DEBUG = 3;
    private Log() {}
    public static boolean isLoggable(String tag, int level) { return false; }
    public static int d(String tag, String msg) { return 0; }
    public static int w(String tag, String msg) { return 0; }
    public static int w(String tag, String msg, Throwable t) { return 0; }
}
