package gesher;

/** Runs the same public NDK contracts inside ART and the translated ARM64 guest. */
public final class MediaCodecNdkTest {
    private static native int run();

    public static void main(String[] args) {
        if (args.length != 1) throw new IllegalArgumentException("native library path required");
        System.load(args[0]);
        int failures = run();
        if (failures != 0) throw new AssertionError("NDK codec failures: " + failures);
        System.out.println("MediaCodecNdkTest: all passed");
    }
}
