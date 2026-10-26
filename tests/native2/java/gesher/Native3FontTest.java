package gesher;

import android.graphics.Typeface;

/** Native font lookup shares the Minikin registry initialized by Typeface. */
public final class Native3FontTest {
    static native String fonts();

    public static void main(String[] args) throws Exception {
        Class<?> vmRuntime = Class.forName("dalvik.system.VMRuntime");
        Object runtime = vmRuntime.getMethod("getRuntime").invoke(null);
        vmRuntime.getMethod("setTargetSdkVersion", int.class).invoke(runtime, 36);
        String root = System.getenv("ANDROID_ROOT");
        Class.forName("com.gesher.app.Main")
                .getMethod("bootstrap", String.class, String.class, String.class)
                .invoke(null, "native3 fonts", root + "/build.prop",
                        root + "/framework/framework-res.apk");
        Object map = Class.forName("com.gesher.app.fonts.SystemFontMap")
                .getMethod("load").invoke(null);
        if (map == null) throw new IllegalStateException("Gesher system font map is missing");
        System.out.println("native3 system font map: " + map);
        System.load(args[0]);
        Typeface.create("sans-serif", Typeface.NORMAL);
        String report = fonts();
        System.out.println(report);
        if (!report.startsWith("native2 fonts: ok")) System.exit(1);
    }
}
