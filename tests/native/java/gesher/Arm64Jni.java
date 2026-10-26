// dalvikvm runs this with the path of jni_test_arm64.so
// (tests/native/arm64_jni_test.c): System.load goes through ART's host
// libnativeloader path to gesher-base's dlopen, the ELF loader (which sees the
// arm64-v8a machine and runs it through the translator) and the JNI bridge
// (compat/bionic/jni_bridge.cc, the guest proxy tables). Prints
// "Arm64Jni all passed" and exits 0 when every check passes (ADR 0047).
package gesher;

public class Arm64Jni {
    static int failures;

    static void check(String what, boolean ok) {
        System.out.println("  " + what + ": " + (ok ? "ok" : "FAIL"));
        if (!ok) failures++;
    }

    // Found by name (Java_gesher_Arm64Jni_*).
    static native int add(int a, int b);
    static native String greet(String who);
    static native void throwFromNative();
    static native int callbacks(Arm64Jni self);
    static native boolean onLoadRan();
    // Registered by JNI_OnLoad.
    static native int registeredTwice(int x);
    static native int vaForms();
    static native int nativeNest(int n);
    static native int attachedThread();

    // Java <-> guest nesting: nest(n) calls the guest, which calls nest(n-1).
    static int nest(int n) {
        return n <= 0 ? 0 : 1 + nativeNest(n - 1);
    }

    // Called from a guest pthread attached with AttachCurrentThread.
    static int fromThread(int x) {
        return x + 1 + (Thread.currentThread().getName().equals("arm64-guest-thread") ? 100 : 0);
    }

    long base = 1000;
    int counter;

    // Called back from native code.
    static int twice(int x) {
        return x * 2;
    }

    static String describe(int i, long j, double d, float f) {
        return i + " " + j + " " + d + " " + f;
    }

    long instanceMix(int a, long b) {
        return a + b + base;
    }

    static void thrower() {
        throw new IllegalStateException("from Java");
    }

    public static void main(String[] args) throws Exception {
        if (args.length < 1) {
            System.err.println("usage: Arm64Jni <jni_test_arm64.so>");
            System.exit(2);
        }
        System.load(args[0]);
        System.out.println("JNI_OnLoad ran: " + (onLoadRan() ? "ok" : "FAIL"));
        check("JNI_OnLoad ran", onLoadRan());

        check("add(40, 2) == 42", add(40, 2) == 42);
        check("greet", "Hello, arm64!".equals(greet("arm64")));
        check("registeredTwice(21) == 42", registeredTwice(21) == 42);

        boolean threw = false;
        try {
            throwFromNative();
        } catch (UnsupportedOperationException e) {
            threw = true;
        }
        check("native throw caught in Java", threw);

        int nativeFailures = callbacks(new Arm64Jni());
        check("native callbacks into ART", nativeFailures == 0);
        check("Call*MethodV from a guest va_list", vaForms() == 0);
        check("Java -> guest -> Java nesting, 8 deep", nest(8) == 8);
        int t = attachedThread();
        if (t != 121) System.out.println("  attachedThread returned " + t);
        check("a guest pthread attaches, calls Java, detaches", t == 121);

        if (failures == 0) {
            System.out.println("Arm64Jni all passed");
            System.exit(0);
        }
        System.out.println(failures + " check(s) failed");
        System.exit(1);
    }
}
