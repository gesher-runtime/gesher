package android.os;

/** Stand-in for the framework class: what Looper uses. */
public final class UserHandle {
  public static final int FIRST_APPLICATION_UID = 10000;

  private UserHandle() {}

  public static boolean isCore(int uid) {
    return uid >= 0 && uid % 100000 < FIRST_APPLICATION_UID;
  }
}
