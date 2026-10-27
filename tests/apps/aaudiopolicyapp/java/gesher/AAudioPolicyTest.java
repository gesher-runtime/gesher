package gesher;

/** Typed NDK calls through the core ART boot path, without a recording grant. */
public final class AAudioPolicyTest {
    private static native String run(boolean allowed);

    public static void main(String[] args) {
        System.load(args[0]);
        String report = run(false);
        System.out.print(report);
        System.out.flush();
        if (!report.contains("media4 aaudio: all passed\n")) System.exit(1);
    }
}
