package gesher;

import android.graphics.Typeface;

/** Native font lookup shares the Minikin registry initialized by Typeface. */
public final class Native3FontTest {
    static native String fonts();

    public static void main(String[] args) {
        System.loadLibrary("android_runtime");
        System.load(args[0]);
        Typeface.create("sans-serif", Typeface.NORMAL);
        String report = fonts();
        System.out.println(report);
        if (!report.startsWith("native2 fonts: ok")) System.exit(1);
    }
}
