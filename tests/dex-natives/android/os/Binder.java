package android.os;

/**
 * Stand-in for the framework class. Without framework.jar (the stand-in tests)
 * it is one process with no calls: the statics Looper uses. With framework.jar
 * on the boot class path the real class is used, and this is only the API
 * gesher.FrameworkNativesTest compiles against.
 */
public class Binder implements IBinder {
  public Binder() {}

  public static long clearCallingIdentity() {
    return 0;
  }

  public static void restoreCallingIdentity(long token) {}

  public static int getCallingUid() {
    return Process.myUid();
  }

  public static int getCallingPid() {
    return Process.myPid();
  }

  public static void flushPendingCommands() {}

  public final boolean transact(int code, Parcel data, Parcel reply, int flags)
      throws RemoteException {
    return onTransact(code, data, reply, flags);
  }

  protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
      throws RemoteException {
    return false;
  }
}
