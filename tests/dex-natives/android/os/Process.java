package android.os;

/**
 * Stand-in for the framework class: the natives match Gesher's Process stub
 * (compat/framework/android_runtime.cc), the rest is what Looper and
 * HandlerThread use, over libcore's android.system.Os as in the real class.
 */
public class Process {
  public static final int THREAD_PRIORITY_DEFAULT = 0;
  public static final int THREAD_PRIORITY_BACKGROUND = 10;
  public static final int THREAD_GROUP_DEFAULT = -1;
  public static final int THREAD_GROUP_BACKGROUND = 0;
  public static final int THREAD_GROUP_FOREGROUND = 1;
  public static final int THREAD_GROUP_SYSTEM = 2;
  public static final int THREAD_GROUP_AUDIO_APP = 3;
  public static final int THREAD_GROUP_AUDIO_SYS = 4;
  public static final int THREAD_GROUP_TOP_APP = 5;
  public static final int THREAD_GROUP_RT_APP = 6;
  public static final int THREAD_GROUP_RESTRICTED = 7;

  public static native void setThreadPriority(int tid, int priority);

  public static native void setThreadPriority(int priority);

  public static native int getThreadPriority(int tid);

  public static native void setArgV0(String text);

  public static native long getElapsedCpuTime();

  // libcore's android.system.Os is on the boot class path at run time but
  // not in the JDK these stand-ins compile against.
  private static int os(String name) {
    try {
      return (Integer) Class.forName("android.system.Os").getMethod(name).invoke(null);
    } catch (ReflectiveOperationException e) {
      throw new RuntimeException(e);
    }
  }

  public static int myTid() {
    return os("gettid");
  }

  public static int myPid() {
    return os("getpid");
  }

  public static int myUid() {
    return os("getuid");
  }

  public static String myProcessName() {
    return "gesher";
  }

  public static boolean isIsolated() {
    return false;
  }

  public static int getProcessGroup(int tid) {
    return THREAD_GROUP_DEFAULT;
  }
}
