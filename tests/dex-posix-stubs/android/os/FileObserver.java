package android.os;

import java.io.File;

/**
 * The part of framework.jar's FileObserver tests/dex-posix compiles against
 * (javac only: the class comes from framework.jar at run time).
 */
public abstract class FileObserver {
  public static final int MODIFY = 0x00000002;
  public static final int CLOSE_WRITE = 0x00000008;
  public static final int MOVED_FROM = 0x00000040;
  public static final int MOVED_TO = 0x00000080;
  public static final int CREATE = 0x00000100;
  public static final int DELETE = 0x00000200;
  public static final int ALL_EVENTS = 0x00000fff;

  public FileObserver(File file, int mask) {}

  public void startWatching() {}

  public void stopWatching() {}

  public abstract void onEvent(int event, String path);
}
