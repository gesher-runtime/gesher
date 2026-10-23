package gesher;

import dalvik.annotation.optimization.CriticalNative;

/**
 * @CriticalNative ABI canaries (compat/framework/android_runtime.cc): enough
 * integer and floating-point arguments that a System V / Microsoft
 * calling-convention mismatch at the managed-code boundary changes the result.
 */
public final class NativeCanary {
  private NativeCanary() {}

  @CriticalNative
  public static native long ints(int a, int b, int c, int d, int e, int f, int g, int h, long i,
      long j);

  @CriticalNative
  public static native double floats(float a, double b, float c, double d, float e, double f,
      float g, double h, float i);

  @CriticalNative
  public static native double mixed(int a, float b, long c, double d, int e, float f, long g,
      double h);

  /** The natives' combination, in Java; exact in double arithmetic for small values. */
  public static long intsExpected(int a, int b, int c, int d, int e, int f, int g, int h, long i,
      long j) {
    return a + 3L * b + 5L * c + 7L * d + 11L * e + 13L * f + 17L * g + 19L * h + 23L * i
        + 29L * j;
  }

  public static double floatsExpected(float a, double b, float c, double d, float e, double f,
      float g, double h, float i) {
    return a + 3.0 * b + 5.0 * c + 7.0 * d + 11.0 * e + 13.0 * f + 17.0 * g + 19.0 * h + 23.0 * i;
  }

  public static double mixedExpected(int a, float b, long c, double d, int e, float f, long g,
      double h) {
    return a + 3.0 * b + 5.0 * c + 7.0 * d + 11.0 * e + 13.0 * f + 17.0 * g + 19.0 * h;
  }
}
