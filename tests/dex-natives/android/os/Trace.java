package android.os;

import dalvik.annotation.optimization.CriticalNative;

/** Stand-in for the framework class; the natives match android_os_Trace.cpp. */
public final class Trace {
  public static final long TRACE_TAG_ALWAYS = 1L << 0;
  public static final long TRACE_TAG_APP = 1L << 12;

  public static final long TRACE_TAG_VIEW = 1L << 3;

  private Trace() {}

  public static boolean isTagEnabled(long traceTag) {
    return nativeIsTagEnabled(traceTag);
  }

  public static void traceBegin(long traceTag, String methodName) {
    if (isTagEnabled(traceTag)) nativeTraceBegin(traceTag, methodName);
  }

  public static void traceEnd(long traceTag) {
    if (isTagEnabled(traceTag)) nativeTraceEnd(traceTag);
  }

  public static native void nativeSetAppTracingAllowed(boolean allowed);

  public static native void nativeSetTracingEnabled(boolean allowed);

  public static native void nativeTraceCounter(long tag, String name, long value);

  public static native void nativeTraceBegin(long tag, String name);

  public static native void nativeAsyncTraceBegin(long tag, String name, int cookie);

  public static native void nativeAsyncTraceEnd(long tag, String name, int cookie);

  public static native void nativeAsyncTraceForTrackBegin(long tag, String trackName, String name,
      int cookie);

  public static native void nativeAsyncTraceForTrackEnd(long tag, String trackName, int cookie);

  public static native void nativeInstant(long tag, String name);

  public static native void nativeInstantForTrack(long tag, String trackName, String name);

  @CriticalNative public static native boolean nativeIsTagEnabled(long tag);

  @CriticalNative public static native void nativeTraceEnd(long tag);
}
