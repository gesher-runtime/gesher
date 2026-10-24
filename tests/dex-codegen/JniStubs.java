package gesher;

/**
 * Native methods called often enough for the JIT to compile their JNI stubs
 * (docs/compiled-code.md, ADR 0036). Run with jni-test.dex on the class path
 * too and the path of jni_test.so (tests/native): the natives are
 * gesher.JniTest's, found by name or registered by its JNI_OnLoad. Covers
 * int, double/float and stack arguments, a String argument and result
 * (the reference result's decoding), an instance native, an exception thrown
 * by native code, calls back into Java, on two threads at once, with GCs in
 * between. Prints "JniStubs all passed".
 */
public final class JniStubs {
  static int failures;

  static void check(String name, boolean ok) {
    if (!ok) {
      failures++;
      System.out.println("FAIL " + name);
    } else {
      System.out.println("ok   " + name);
    }
  }

  static final double MIX = 1 + 0.5 + 3 + 0.25 + 50 + 600 + 7000 + 80000 + 1.5 + 2 * 2.5 + 4 * 3.5
      + 8 * 4.5 + 16 * 5.5 + 32 * 6.5 + 64 * 7.5;

  // One thread's share; returns the number of wrong results.
  static int calls(int rounds) {
    int wrong = 0;
    JniTest self = new JniTest();
    for (int i = 0; i < rounds; i++) {
      if (JniTest.add(i, 1) != i + 1) wrong++;
      if (JniTest.registeredTwice(i) != 2 * i) wrong++;
      if (self.instanceSum(i) != 1000 + i) wrong++;
      if (JniTest.mix(1, 0.5, 3, 0.25f, 5, 6, 7, 8, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5) != MIX) wrong++;
      if (i % 16 == 0 && !"Hello, gesher!".equals(JniTest.greet("gesher"))) wrong++;
      if (i % 64 == 0) {
        try {
          JniTest.throwFromNative();
          wrong++;
        } catch (UnsupportedOperationException e) {
          if (!"thrown by native code".equals(e.getMessage())) wrong++;
        }
      }
      if (i == rounds / 2 && JniTest.callbacks(new JniTest()) != 0) wrong++;
    }
    return wrong;
  }

  public static void main(String[] args) throws Exception {
    System.load(args[0]);
    for (int round = 0; round < 4; round++) {
      final int[] other = new int[1];
      Thread t = new Thread(() -> other[0] = calls(20000));
      t.start();
      int mine = calls(20000);
      t.join();
      System.gc();
      check("JNI calls, round " + round + " (two threads)", mine == 0 && other[0] == 0);
    }
    System.out.println(failures == 0 ? "JniStubs all passed" : "JniStubs " + failures + " failed");
    if (failures != 0) System.exit(1);
  }
}
