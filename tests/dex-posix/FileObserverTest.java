package gesher;

import android.os.FileObserver;
import java.io.File;
import java.io.FileOutputStream;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * android.os.FileObserver (framework.jar) over libandroid_runtime's natives and
 * gesher-base's inotify (docs/posix.md): a watched directory reports a file's
 * creation, write, close, move and deletion.
 *
 *   FileObserverTest <directory>
 */
public final class FileObserverTest {
  private static final LinkedBlockingQueue<String> events = new LinkedBlockingQueue<>();

  public static void main(String[] args) throws Exception {
    File dir = new File(args[0]);
    check(dir.isDirectory() || dir.mkdirs(), "directory " + dir);
    int mask = FileObserver.CREATE | FileObserver.MODIFY | FileObserver.CLOSE_WRITE
        | FileObserver.MOVED_FROM | FileObserver.MOVED_TO | FileObserver.DELETE;
    FileObserver observer = new FileObserver(dir, mask) {
      @Override
      public void onEvent(int event, String path) {
        events.add((event & FileObserver.ALL_EVENTS) + " " + path);
      }
    };
    observer.startWatching();
    File file = new File(dir, "observed.txt");
    try (FileOutputStream out = new FileOutputStream(file)) {
      out.write(new byte[] {1, 2, 3});
    }
    expect(FileObserver.CREATE, "observed.txt");
    expect(FileObserver.MODIFY, "observed.txt");
    expect(FileObserver.CLOSE_WRITE, "observed.txt");
    File moved = new File(dir, "moved.txt");
    check(file.renameTo(moved), "rename");
    expect(FileObserver.MOVED_FROM, "observed.txt");
    expect(FileObserver.MOVED_TO, "moved.txt");
    check(moved.delete(), "delete");
    expect(FileObserver.DELETE, "moved.txt");
    observer.stopWatching();
    System.out.println("FileObserverTest all passed");
  }

  // Waits up to 5 s for the event, skipping others.
  private static void expect(int event, String path) throws InterruptedException {
    String want = event + " " + path;
    long deadline = System.currentTimeMillis() + 5000;
    StringBuilder seen = new StringBuilder();
    while (System.currentTimeMillis() < deadline) {
      String e = events.poll(100, TimeUnit.MILLISECONDS);
      if (e == null) continue;
      if (e.equals(want)) return;
      seen.append(" [").append(e).append(']');
    }
    throw new AssertionError("no event " + want + "; saw" + seen);
  }

  private static void check(boolean ok, String what) {
    if (!ok) throw new AssertionError(what);
  }
}
