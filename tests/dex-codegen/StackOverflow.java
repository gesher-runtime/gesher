package gesher;

import java.util.ArrayList;
import java.util.List;

/**
 * Deep recursion throws a StackOverflowError that the program catches and
 * recovers from, over and over, on the main thread and on new threads
 * (docs/compiled-code.md, ADR 0036):
 *   app:     app code recursing (the switch interpreter, nterp or the JIT);
 *   boot:    the boot library recursing (ArrayList.hashCode of a list that
 *            contains itself): AOT code from the boot image, outside -Xint;
 *   native:  app recursion with a native call (and a math entrypoint) in
 *            every frame, so the overflow may come near a JNI transition.
 * Each kind runs several rounds in a row; after them the thread must still
 * be able to recurse normally. Prints "StackOverflow all passed".
 */
public final class StackOverflow {
  static int failures;

  static void check(String name, boolean ok) {
    if (!ok) {
      failures++;
      System.out.println("FAIL " + name);
    } else {
      System.out.println("ok   " + name);
    }
  }

  // Per thread: the threads below overflow at the same time.
  static final class Recurser {
    int depth;
    double sink;

    void recurse(int x) {
      depth++;
      recurse(x + 1);
    }

    void recurseNative(Object o) {
      depth++;
      sink += System.identityHashCode(o) + Math.sin(depth);
      recurseNative(o);
    }
  }

  static int finite(int n) {
    return n == 0 ? 0 : finite(n - 1) + 1;
  }

  // One kind, `rounds` times in a row; the depth reached each time.
  static boolean rounds(String kind, int rounds, StringBuilder depths) {
    for (int round = 0; round < rounds; round++) {
      Recurser r = new Recurser();
      int depth = 0;
      try {
        switch (kind) {
          case "app":
            r.recurse(0);
            break;
          case "native":
            r.recurseNative(new Object());
            break;
          case "boot": {
            List<Object> self = new ArrayList<>();
            self.add(self);
            self.hashCode();
            break;
          }
          default:
            return false;
        }
        return false;  // never returns normally
      } catch (StackOverflowError e) {
        // The boot library's frames are counted in the error's stack trace.
        depth = kind.equals("boot") ? e.getStackTrace().length : r.depth;
        if (depth < 10) return false;  // must get at least a little way down
        depths.append(round == 0 ? "" : " ").append(depth);
      }
    }
    return finite(200) == 200;
  }

  static void all(String where) {
    for (String kind : new String[] {"app", "boot", "native"}) {
      StringBuilder depths = new StringBuilder();
      boolean ok = rounds(kind, 4, depths);
      check("stack overflow, " + kind + " code, on " + where + " (depths " + depths + ")", ok);
    }
  }

  static void onThread(String name, long stackSize) throws InterruptedException {
    Thread t = new Thread(null, () -> all(name), name, stackSize);
    t.start();
    t.join();
  }

  public static void main(String[] args) throws Exception {
    all("the main thread");
    onThread("a new thread", 0);
    onThread("a new thread with a 512 KiB stack", 512 * 1024);
    // Two at once.
    List<Thread> threads = new ArrayList<>();
    for (int i = 0; i < 2; i++) {
      String name = "concurrent thread " + i;
      threads.add(new Thread(() -> all(name), name));
    }
    for (Thread t : threads) t.start();
    for (Thread t : threads) t.join();
    // And the main thread once more, after the others.
    all("the main thread (again)");
    System.out.println(failures == 0 ? "StackOverflow all passed" : "StackOverflow " + failures + " failed");
    if (failures != 0) System.exit(1);
  }
}
