package android.os;
public final class SystemProperties {
    public static String get(String key, String fallback) { return fallback; }
    public static boolean getBoolean(String key, boolean fallback) { return fallback; }
    public static long getLong(String key, long fallback) { return 0; }
}
