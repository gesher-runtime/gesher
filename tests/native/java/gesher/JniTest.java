// dalvikvm runs this with the path of jni_test.so (tests/native/jni_test.c):
// System.load goes through ART's host libnativeloader path to gesher-base's
// dlopen, the ELF loader and the JNI bridge (compat/bionic/jni_bridge.cc).
// Prints "JniTest all passed" and exits 0 when every check passes.
package gesher;

import java.lang.reflect.Method;
import java.util.Arrays;

public class JniTest {
    static int failures;

    static void check(String what, boolean ok) {
        System.out.println("  " + what + ": " + (ok ? "ok" : "FAIL"));
        if (!ok) failures++;
    }

    // Found by name (Java_gesher_JniTest_*).
    static native int add(int a, int b);
    static native String greet(String who);
    static native double mix(int a, double b, long c, float d, int e, int f, int g, int h,
            double i, double j, double k, double l, double m, double n, double o);
    static native void throwFromNative();
    static native int tlsBump();
    static native int callbacks(JniTest self);
    static native int callReflected(Method m);
    static native int threadAttach();
    static native int threadExitAttached();
    // Invoked only by FatalContext in a disposable VM, never by main().
    static native long fatalNullRead(boolean host);
    static native boolean onLoadRan();
    static native int hookedTable(String s);
    static native int sharedMemory();
    // Registered by JNI_OnLoad.
    static native int registeredTwice(int x);
    native long instanceSum(long a);

    long base = 1000;
    int counter;
    static String sfield = "static-field";

    JniTest() {}

    JniTest(int counter) {
        this.counter = counter;
    }

    // Called from native code.
    static int twice(int x) {
        return 2 * x;
    }

    // Native callbacks exceed inline storage and spill both register banks.
    static double wideCallback(int i1, double d1, int i2, double d2, int i3, double d3,
            int i4, double d4, int i5, double d5, int i6, double d6,
            int i7, double d7, int i8, double d8, int i9, double d9) {
        return 100.0 * (i1 + 2 * i2 + 3 * i3 + 4 * i4 + 5 * i5 + 6 * i6
                + 7 * i7 + 8 * i8 + 9 * i9)
                + d1 + 2 * d2 + 3 * d3 + 4 * d4 + 5 * d5 + 6 * d6
                + 7 * d7 + 8 * d8 + 9 * d9;
    }

    static int combine(int a, double b, long c) {
        return a * 100 + (int) (b * 10) + (int) c;
    }

    static String describe(int i, long j, double d, float f, boolean z, char c, short s, byte b,
            String str, int[] arr) {
        return i + " " + j + " " + d + " " + f + " " + z + " " + c + " " + s + " " + b + " "
                + str + " " + Arrays.toString(arr);
    }

    long instanceMix(int a, long b, double c, float d, String s) {
        return a + b + (long) c + (long) d + s.length() + base;
    }

    static void thrower() {
        throw new IllegalStateException("from Java");
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        System.out.println("JniTest: loaded " + args[0]);
        check("JNI_OnLoad ran", onLoadRan());
        check("Java_* by name: add(40, 2) == 42", add(40, 2) == 42);
        check("strings both ways: greet", "Hello, gesher!".equals(greet("gesher")));
        int hooked = hookedTable("gesher");
        check("a library that replaces its JNIEnv's table still reaches ART (failure "
                + hooked + ")", hooked == 0);
        int shm = sharedMemory();
        check("ASharedMemory: sealed GROW|SHRINK, setProt(PROT_READ) seals future writes (failure "
                + shm + ")", shm == 0);
        double expect = 1 + 0.5 + 3 + 0.25 + 50 + 600 + 7000 + 80000 + 1.5 + 2 * 2.5 + 4 * 3.5
                + 8 * 4.5 + 16 * 5.5 + 32 * 6.5 + 64 * 7.5;
        double got = mix(1, 0.5, 3, 0.25f, 5, 6, 7, 8, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5);
        check("15 arguments, some on the stack: mix == " + expect + " (got " + got + ")",
                got == expect);
        check("RegisterNatives: registeredTwice(21) == 42", registeredTwice(21) == 42);
        check("RegisterNatives, instance: instanceSum(5) == 1005",
                new JniTest().instanceSum(5) == 1005);
        try {
            throwFromNative();
            check("ThrowNew from native", false);
        } catch (UnsupportedOperationException e) {
            check("ThrowNew from native", "thrown by native code".equals(e.getMessage()));
        }
        int n = callbacks(new JniTest());
        check("calls back into ART through JNIEnv: " + n + " failure(s)", n == 0);
        Method m = JniTest.class.getDeclaredMethod("combine", int.class, double.class, long.class);
        check("FromReflectedMethod + variadic call", callReflected(m) == 512);
        check("a native thread attaches and calls Java", threadAttach() == 105);
        check("a native thread that ends attached only warns, as on Android",
                threadExitAttached() == 14);
        // Initial-exec TLS: each thread starts from the initial value 5.
        check("initial-exec TLS on the main thread", tlsBump() == 6 && tlsBump() == 7);
        final int[] other = new int[2];
        Thread t = new Thread(() -> {
            other[0] = tlsBump();
            other[1] = add(1, 2);
        });
        t.start();
        t.join();
        check("initial-exec TLS on another ART thread", other[0] == 6 && other[1] == 3);
        check("the main thread keeps its own TLS", tlsBump() == 8);
        if (failures == 0) {
            System.out.println("JniTest all passed");
        } else {
            System.out.println("JniTest: " + failures + " check(s) failed");
            System.exit(1);
        }
    }
}
