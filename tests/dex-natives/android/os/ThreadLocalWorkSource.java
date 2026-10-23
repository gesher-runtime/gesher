package android.os;

/** Stand-in for the framework class: the work source uid of the current thread. */
public final class ThreadLocalWorkSource {
  public static final int UID_NONE = -1;
  private static final ThreadLocal<Integer> sWorkSourceUid = ThreadLocal.withInitial(() -> UID_NONE);

  private ThreadLocalWorkSource() {}

  public static int getUid() {
    return sWorkSourceUid.get();
  }

  public static long setUid(int uid) {
    long token = getUid();
    sWorkSourceUid.set(uid);
    return token;
  }

  public static void restore(long token) {
    sWorkSourceUid.set((int) token);
  }
}
