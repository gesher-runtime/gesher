package gesher;

import java.util.Objects;

public final class BinderSgActor {
    private static native int run(String role, String scenario, String prefix, String endpoint);
    private static native String currentEndpoint();
    public static void main(String[] args) {
        if (args.length != 5) throw new IllegalArgumentException("library role scenario prefix endpoint required");
        System.load(args[0]);
        String before = currentEndpoint();
        int status = run(args[1], args[2], args[3], args[4]);
        boolean restored = Objects.equals(before, currentEndpoint());
        System.out.println("SG ART result: role=" + args[1] + "; scenario=" + args[2]
                + "; status=" + status + "; endpoint restored=" + restored);
        System.out.flush();
        if (status != 0 || !restored) System.exit(1);
    }
}
