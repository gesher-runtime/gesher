package android.os;

import dalvik.annotation.optimization.CriticalNative;

/** Stand-in for the framework class; the natives match android_os_SystemProperties.cpp. */
public class SystemProperties {
  public static native String native_get(String key, String def);

  public static native int native_get_int(String key, int def);

  public static native long native_get_long(String key, long def);

  public static native boolean native_get_boolean(String key, boolean def);

  public static native long native_find(String name);

  public static native String native_get(long handle);

  @CriticalNative public static native int native_get_int(long handle, int def);

  @CriticalNative public static native long native_get_long(long handle, long def);

  @CriticalNative public static native boolean native_get_boolean(long handle, boolean def);

  public static native void native_set(String key, String def);

  public static native void native_add_change_callback();

  public static native void native_report_sysprop_change();

  public static int getInt(String key, int def) {
    return native_get_int(key, def);
  }

  public static boolean getBoolean(String key, boolean def) {
    return native_get_boolean(key, def);
  }

  // native_add_change_callback caches this method; it must exist.
  private static void callChangeCallbacks() {}
}
