package gesher;

import java.lang.ref.WeakReference;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Exercises what compiled code and nterp depend on (docs/compiled-code.md):
 * implicit null checks, stack overflow, exceptions thrown through many
 * frames, GC with frames on the stack, monitors and threads, JNI (libcore
 * natives), reflection and proxies, array bounds and arithmetic exceptions.
 *
 * Prints "Stress all passed" when every check holds; otherwise the failures.
 */
public final class Stress {
  static int failures;

  static void check(String name, boolean ok) {
    if (!ok) {
      failures++;
      System.out.println("FAIL " + name);
    } else {
      System.out.println("ok   " + name);
    }
  }

  static final class Node {
    int value;
    Node next;
  }

  static int readValue(Node n) {
    return n.value;  // implicit null check
  }

  static int nullChecks() {
    int caught = 0;
    Node n = null;
    for (int i = 0; i < 1000; i++) {
      try {
        readValue(n);
      } catch (NullPointerException e) {
        caught++;
      }
      try {
        Object o = null;
        o.hashCode();
      } catch (NullPointerException e) {
        caught++;
      }
      try {
        int[] a = null;
        a[0] = 1;
      } catch (NullPointerException e) {
        caught++;
      }
    }
    return caught;
  }

  static int depth;

  static void recurse(int x) {
    depth++;
    recurse(x + 1);
  }

  static boolean stackOverflow() {
    for (int round = 0; round < 3; round++) {
      depth = 0;
      try {
        recurse(0);
        return false;
      } catch (StackOverflowError e) {
        if (depth < 1000) return false;
      }
    }
    return true;
  }

  static int thrower(int n) {
    if (n == 0) throw new IllegalStateException("bottom");
    return thrower(n - 1) + 1;
  }

  static boolean deepExceptions() {
    int caught = 0;
    for (int i = 0; i < 200; i++) {
      try {
        thrower(100);
      } catch (IllegalStateException e) {
        if ("bottom".equals(e.getMessage()) && e.getStackTrace().length > 100) caught++;
      }
    }
    return caught == 200;
  }

  static boolean finallyAndRethrow() {
    int finallies = 0;
    try {
      try {
        try {
          throw new RuntimeException("x");
        } finally {
          finallies++;
        }
      } catch (RuntimeException e) {
        finallies++;
        throw new Error("y", e);
      }
    } catch (Error e) {
      return finallies == 2 && e.getCause().getMessage().equals("x");
    }
  }

  static long gcWithFrames(int level, List<Object> keep) {
    Object local = new byte[1024];
    if (level > 0) {
      long r = gcWithFrames(level - 1, keep);
      return r + ((byte[]) local).length;
    }
    WeakReference<Object> weak = new WeakReference<>(new Object());
    for (int i = 0; i < 2000; i++) keep.add(new int[256]);
    keep.clear();
    System.gc();
    Runtime.getRuntime().gc();
    return weak.get() == null ? 1 : 0;
  }

  static boolean gc() {
    List<Object> keep = new ArrayList<>();
    long r = gcWithFrames(50, keep);
    // 50 frames of 1024 bytes each survived, and the weak referent is gone.
    return r == 50 * 1024 + 1;
  }

  static final Object lock = new Object();
  static long shared;

  static boolean threads() throws InterruptedException {
    final int n = 4;
    final AtomicLong atomic = new AtomicLong();
    Thread[] ts = new Thread[n];
    for (int t = 0; t < n; t++) {
      ts[t] = new Thread(() -> {
        for (int i = 0; i < 20_000; i++) {
          synchronized (lock) {
            shared++;
          }
          atomic.incrementAndGet();
          if ((i & 1023) == 0) {
            try {
              Object o = null;
              o.toString();
            } catch (NullPointerException e) {
              // expected
            }
          }
        }
      });
      ts[t].start();
    }
    // wait/notify between this thread and one more.
    final boolean[] ready = new boolean[1];
    Thread waiter = new Thread(() -> {
      synchronized (lock) {
        while (!ready[0]) {
          try {
            lock.wait();
          } catch (InterruptedException e) {
            return;
          }
        }
      }
    });
    waiter.start();
    synchronized (lock) {
      ready[0] = true;
      lock.notifyAll();
    }
    waiter.join();
    for (Thread t : ts) t.join();
    return shared == n * 20_000L && atomic.get() == n * 20_000L;
  }

  static boolean jni() {
    // libcore natives: String.intern, System.arraycopy, Math (StrictMath),
    // Object.clone, identityHashCode, Thread.currentThread.
    String s = new StringBuilder("gesher").append("-jni").toString();
    boolean ok = s.intern() == "gesher-jni";
    int[] a = {1, 2, 3, 4, 5};
    int[] b = new int[5];
    System.arraycopy(a, 0, b, 0, 5);
    ok &= b[4] == 5;
    ok &= Math.abs(StrictMath.cbrt(27.0) - 3.0) < 1e-12;
    int[] c = a.clone();
    ok &= c != a && c[2] == 3;
    ok &= System.identityHashCode(a) == System.identityHashCode(a);
    ok &= Thread.currentThread() != null;
    long sum = 0;
    for (int i = 0; i < 10_000; i++) sum += Long.parseLong(Integer.toString(i));
    ok &= sum == 49_995_000L;
    return ok;
  }

  interface Greeter {
    String greet(String name);
  }

  static boolean proxy() {
    InvocationHandler h = (p, m, args) -> m.getName() + ":" + args[0];
    Greeter g = (Greeter) Proxy.newProxyInstance(
        Stress.class.getClassLoader(), new Class<?>[] {Greeter.class}, h);
    boolean ok = true;
    for (int i = 0; i < 100; i++) ok &= g.greet("x" + i).equals("greet:x" + i);
    return ok;
  }

  static boolean reflection() throws Exception {
    java.lang.reflect.Method m = Stress.class.getDeclaredMethod("thrower", int.class);
    try {
      m.invoke(null, 3);
      return false;
    } catch (java.lang.reflect.InvocationTargetException e) {
      return e.getCause() instanceof IllegalStateException;
    }
  }

  static boolean arrayAndArith() {
    int caught = 0;
    int[] a = new int[3];
    for (int i = 0; i < 100; i++) {
      try {
        a[i + 3] = 1;
      } catch (ArrayIndexOutOfBoundsException e) {
        caught++;
      }
      try {
        int zero = i - i;
        caught += 10 / zero;
      } catch (ArithmeticException e) {
        caught++;
      }
      try {
        Object o = "s";
        Integer x = (Integer) o;
        caught += x;
      } catch (ClassCastException e) {
        caught++;
      }
    }
    return caught == 300;
  }

  public static void main(String[] args) throws Exception {
    check("implicit null checks", nullChecks() == 3000);
    check("stack overflow", stackOverflow());
    check("exceptions through 100 frames", deepExceptions());
    check("finally and rethrow", finallyAndRethrow());
    check("gc with frames on the stack", gc());
    check("threads, monitors, wait/notify", threads());
    check("jni (libcore natives)", jni());
    check("proxy", proxy());
    check("reflection", reflection());
    check("array bounds, div by zero, class cast", arrayAndArith());
    // Once more, after the code has warmed up (JIT).
    check("implicit null checks (again)", nullChecks() == 3000);
    check("stack overflow (again)", stackOverflow());
    check("exceptions through 100 frames (again)", deepExceptions());
    System.out.println(failures == 0 ? "Stress all passed" : "Stress " + failures + " failed");
    System.exit(failures == 0 ? 0 : 1);
  }
}
