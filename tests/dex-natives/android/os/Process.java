package android.os;

/** Stand-in for the framework class; the natives match Gesher's Process stub. */
public class Process {
  public static native void setThreadPriority(int tid, int priority);

  public static native void setThreadPriority(int priority);

  public static native int getThreadPriority(int tid);

  public static native void setArgV0(String text);

  public static native long getElapsedCpuTime();
}
