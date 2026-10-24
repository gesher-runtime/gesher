package gesher;

import java.nio.file.FileSystems;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardWatchEventKinds;
import java.nio.file.WatchEvent;
import java.nio.file.WatchKey;
import java.nio.file.WatchService;
import java.util.concurrent.TimeUnit;

/**
 * java.nio.file.WatchService (libcore's LinuxWatchService, libopenjdk's
 * LinuxWatchService.c) over gesher-base's inotify (docs/posix.md).
 *
 *   WatchServiceTest <directory>
 */
public final class WatchServiceTest {
  public static void main(String[] args) throws Exception {
    Path dir = Paths.get(args[0]);
    Files.createDirectories(dir);
    WatchService watcher = FileSystems.getDefault().newWatchService();
    check(watcher.getClass().getName().equals("sun.nio.fs.LinuxWatchService"), watcher.getClass().getName());
    dir.register(watcher, StandardWatchEventKinds.ENTRY_CREATE, StandardWatchEventKinds.ENTRY_MODIFY,
        StandardWatchEventKinds.ENTRY_DELETE);
    Path file = dir.resolve("watched.txt");
    Files.write(file, new byte[] {1, 2, 3});
    expect(watcher, StandardWatchEventKinds.ENTRY_CREATE, "watched.txt");
    Files.delete(file);
    expect(watcher, StandardWatchEventKinds.ENTRY_DELETE, "watched.txt");
    watcher.close();
    System.out.println("WatchServiceTest all passed");
  }

  private static void expect(WatchService watcher, WatchEvent.Kind<?> kind, String name) throws Exception {
    long deadline = System.currentTimeMillis() + 5000;
    StringBuilder seen = new StringBuilder();
    while (System.currentTimeMillis() < deadline) {
      WatchKey key = watcher.poll(100, TimeUnit.MILLISECONDS);
      if (key == null) continue;
      boolean found = false;
      for (WatchEvent<?> e : key.pollEvents()) {
        seen.append(" [").append(e.kind()).append(' ').append(e.context()).append(']');
        found |= e.kind() == kind && name.equals(String.valueOf(e.context()));
      }
      key.reset();
      if (found) return;
    }
    throw new AssertionError("no " + kind + " " + name + "; saw" + seen);
  }

  private static void check(boolean ok, String what) {
    if (!ok) throw new AssertionError(what);
  }
}
