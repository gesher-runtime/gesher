package gesher;

/** Core-only DEX: no framework.jar, Typeface, FontFamily or app bootstrap. */
public final class Native3StandaloneFontTest {
    private static native int run(String[] arguments);

    private static void absent(String name) {
        try {
            Class.forName(name, false, ClassLoader.getSystemClassLoader());
            throw new IllegalStateException("Framework class present in core-only test: " + name);
        } catch (ClassNotFoundException expected) {
            // The class must be absent, not merely uninitialized.
        }
    }

    public static void main(String[] args) {
        if (args.length != 5) throw new IllegalArgumentException("library, case, root, fonts, xml");
        absent("android.graphics.Typeface");
        absent("android.graphics.fonts.FontFamily");
        absent("com.gesher.app.fonts.SystemFontMap");
        String boot = System.getProperty("java.boot.class.path");
        if (boot == null || boot.contains("framework.jar") || boot.contains("gesher-app.jar")) {
            throw new IllegalStateException("Unexpected core-only boot class path: " + boot);
        }
        System.out.println("FONT_CORE_BCP " + boot);
        System.out.println("FONT_CORE_ARGV " + java.util.Arrays.toString(args));
        System.out.println("FONT_CORE_CLASSES_ABSENT Typeface FontFamily SystemFontMap");
        System.load(args[0]);
        String[] arguments = {"fonts_standalone", args[1], args[2], args[3], args[4], "dex"};
        int result = run(arguments);
        if (result != 0) throw new IllegalStateException("Native standalone font exit " + result);
        System.out.println("FONT_CORE_DEX_PASS case=" + args[1]);
    }
}
