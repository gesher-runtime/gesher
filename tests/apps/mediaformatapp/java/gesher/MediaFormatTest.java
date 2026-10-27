package gesher;

/** The same real NDK format checks through System.load on the core boot path. */
public final class MediaFormatTest {
    private static native String run();

    public static void main(String[] args) {
        System.load(args[0]);
        String report = run();
        System.out.print(report);
        System.out.flush();
        if (!report.contains("media4 format: all passed\n")) System.exit(1);
    }
}
