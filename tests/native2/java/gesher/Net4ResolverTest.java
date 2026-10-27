package gesher;
public final class Net4ResolverTest {
    private static native String run(boolean allowed);
    public static void main(String[] args) {
        System.load(args[0]);
        String report = run(args[1].equals("allow"));
        System.out.print(report);
        System.out.flush();
        if (!report.contains("net4 resolver: all passed;")) System.exit(1);
    }
}
