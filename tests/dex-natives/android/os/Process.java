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

  // The natives, by the real class's names (compat/framework/android_runtime.cc).
  private static native void setThreadPriorityNative(int tid, int priority);

  public static native int getThreadPriority(int tid);

  private static native void setArgV0Native(String text);

  public static native long getElapsedCpuTime();

  public static native void setThreadGroup(int tid, int group);

  public static native int getProcessGroup(int pid);

  public static native int getUidForName(String name);

  public static native void sendSignalQuiet(int pid, int signal);

  public static native void sendSignal(int pid, int signal);

  public static native void setThreadGroupAndCpuset(int tid, int group);

  public static native void setProcessGroup(int pid, int group);

  public static native int getGidForName(String name);

  public static native void readProcLines(String file, String[] fields, long[] values);

  public static native boolean readProcFile(String file, int[] format,
      String[] strings, long[] longs, float[] floats);

  public static native boolean parseProcLine(byte[] buffer, int start, int end,
      int[] format, String[] strings, long[] longs, float[] floats);

  public static void setThreadPriority(int tid, int priority) {
    setThreadPriorityNative(tid, priority);
  }

  public static void setThreadPriority(int priority) {
    setThreadPriorityNative(myTid(), priority);
  }

  public static void setArgV0(String text) {
    setArgV0Native(text);
  }

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

}
