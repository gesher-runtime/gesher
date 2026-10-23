package android.os;

import dalvik.annotation.optimization.CriticalNative;

/** Stand-in for the framework class; the natives match android_os_SystemClock.cpp. */
public final class SystemClock {
  private SystemClock() {}

  @CriticalNative public static native long uptimeMillis();

  @CriticalNative public static native long uptimeNanos();

  @CriticalNative public static native long elapsedRealtime();

  @CriticalNative public static native long elapsedRealtimeNanos();

  @CriticalNative public static native long currentThreadTimeMillis();

  @CriticalNative public static native long currentThreadTimeMicro();

  @CriticalNative public static native long currentTimeMicro();
}
