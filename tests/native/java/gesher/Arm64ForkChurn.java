package gesher;

// Guest forks while Java threads keep starting and leaving (ADR 0092): each
// leaving thread runs its exit callbacks under ntdll's loader lock, which the
// clone waits for, so none of them may wait for a lock the fork holds. The
// forks are libarm64fork.so's forkLoop (tests/native/arm64_fork_exec.c).
public class Arm64ForkChurn {
  static native int forkLoop(int n);
  static volatile boolean stop;

  public static void main(String[] args) throws Exception {
    System.load(args[0]);
    int churners = Integer.parseInt(args[1]);
    int forks = Integer.parseInt(args[2]);
    final long[] started = new long[1];
    Thread[] threads = new Thread[churners];
    for (int c = 0; c < churners; ++c) {
      threads[c] = new Thread(() -> {
        while (!stop) {
          Thread t = new Thread(() -> {
            long s = 0;
            for (int i = 0; i < 1000; ++i) s += i;
            if (s == 42) System.out.println();
          });
          t.start();
          try {
            t.join();
          } catch (InterruptedException e) {
            return;
          }
          synchronized (started) {
            started[0]++;
          }
        }
      });
      threads[c].start();
    }
    int failures = forkLoop(forks);
    stop = true;
    for (Thread t : threads) t.join();
    System.out.println("Arm64ForkChurn threads started and left: " + (started[0] > 0) + " failures=" + failures);
    System.out.println(failures == 0 && started[0] > 0 ? "Arm64ForkChurn all passed" : "Arm64ForkChurn FAILED");
    System.exit(failures == 0 ? 0 : 1);
  }
}
