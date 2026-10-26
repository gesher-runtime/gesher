package gesher;

/**
 * The native2 track's test (tests/native2/native2_test.cpp) in dalvikvm:
 * {@code Native2Test <libnative2.so> <work dir> [<symbols file>]}. System.load
 * of the library (x86_64 or arm64-v8a), then its checks; prints their lines.
 */
public final class Native2Test {
    static native String run(String dir, String symbols);

    public static void main(String[] args) {
        System.load(args[0]);
        String report = run(args[1], args.length > 2 ? args[2] : "");
        System.out.print(report);
        System.out.flush();
        if (!report.contains("native2: all passed")) System.exit(1);
    }
}
