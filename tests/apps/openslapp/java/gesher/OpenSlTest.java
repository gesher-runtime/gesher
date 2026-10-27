package gesher;

/** Typed NDK calls through the core ART boot path, with no manifest gate. */
public final class OpenSlTest {
    private static native String run(boolean allowed);

    public static void main(String[] args) {
        System.load(args[0]);
        String report = run(true);
        System.out.print(report);
        System.out.flush();
        if (!report.contains("media4 opensl: all passed\n")) System.exit(1);
    }
}
