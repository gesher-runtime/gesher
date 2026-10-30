package gesher;
/** Diagnostic only; BMP path strings, no framework bootstrap or font fallback claim. */
public final class Native3FontEnvironmentTest {
    private static native int run(String[] values);
    private static void absent(String name) throws Exception {
        try { Class.forName(name, false, ClassLoader.getSystemClassLoader()); throw new IllegalStateException("Framework class present: " + name); }
        catch (ClassNotFoundException expected) { }
    }
    public static void main(String[] args) throws Exception {
        System.out.println("FONT_ENV_JAVA_ENTER");
        if (args.length != 9 || !args[1].equals("dex")) throw new IllegalArgumentException("library plus eight explicit probe values");
        System.out.println("FONT_ENV_CORE_BCP " + System.getProperty("java.boot.class.path"));
        absent("android.graphics.Typeface"); absent("android.graphics.fonts.FontFamily"); absent("com.gesher.app.fonts.SystemFontMap");
        System.out.println("FONT_ENV_CORE_CLASSES_ABSENT Typeface FontFamily SystemFontMap");
        System.load(args[0]); String[] values = new String[9]; values[0] = "fonts_env";
        System.arraycopy(args, 1, values, 1, 8);
        int result = run(values); if (result != 0) throw new IllegalStateException("Environment diagnostic exit " + result);
        System.out.println("FONT_ENV_JAVA_OBSERVED observation_only=1 unicode_environment_conformance=false");
    }
}
