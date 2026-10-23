package android.util;

/** Stand-in for the framework class: only what MessageQueue's dump uses. */
public final class TimeUtils {
  private TimeUtils() {}

  public static void formatDuration(long duration, StringBuilder builder) {
    builder.append(duration).append("ms");
  }
}
