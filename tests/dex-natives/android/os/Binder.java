package android.os;

/** Stand-in for the framework class until libbinder (step 5): one process, no calls. */
public class Binder {
  public static long clearCallingIdentity() {
    return 0;
  }

  public static void restoreCallingIdentity(long token) {}

  public static int getCallingUid() {
    return Process.myUid();
  }

  public static void flushPendingCommands() {}
}
