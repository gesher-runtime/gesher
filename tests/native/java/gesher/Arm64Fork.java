package gesher;

// Guest fork inside ART (ADR 0092): an arm64-v8a JNI library
// (tests/native/arm64_fork_exec.c built with -DARM64_FORK_JNI) runs every
// fork check while ART's threads, and this busy Java thread, keep running.
// The children compute and exit in their own processes; the VM goes on.
public class Arm64Fork {
  static native int run();

  public static void main(String[] args) throws Exception {
    System.load(args[0]);
    final long[] sum = new long[1];
    Thread busy = new Thread(() -> {
      long s = 0;
      for (int i = 0; i < 100_000_000; ++i) s += i % 7;
      sum[0] = s;
    });
    busy.start();
    int failures = run();
    busy.join();
    System.out.println("Java thread computed while forking: " + (sum[0] > 0));
    System.out.println(failures == 0 && sum[0] > 0 ? "Arm64Fork all passed" : "Arm64Fork failures=" + failures);
    System.exit(failures == 0 ? 0 : 1);
  }
}
