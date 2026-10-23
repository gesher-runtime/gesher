package android.util;

/** Stand-in for the framework class: the system log buffer, here the main one. */
public final class Slog {
  private Slog() {}

  public static int w(String tag, String msg) {
    return Log.w(tag, msg);
  }
}
