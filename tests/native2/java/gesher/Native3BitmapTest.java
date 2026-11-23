package gesher;

import android.graphics.Bitmap;

/** libjnigraphics through an NDK library, with the framework on the boot class path. */
public final class Native3BitmapTest {
    static native String bitmap(Bitmap bitmap);

    public static void main(String[] args) throws Exception {
        // Let the framework own its natives while the test loader owns the NDK library below.
        String root = System.getenv("ANDROID_ROOT");
        Class.forName("com.gesher.app.Main")
                .getMethod("bootstrap", String.class, String.class, String.class)
                .invoke(null, "native3 bitmap", root + "/build.prop",
                        root + "/framework/framework-res.apk");
        System.load(args[0]);
        Bitmap bitmap = Bitmap.createBitmap(4, 3, Bitmap.Config.ARGB_8888);
        bitmap.eraseColor(0xffff0000);
        String report = bitmap(bitmap);
        System.out.println(report);
        boolean changed = bitmap.getPixel(0, 0) == 0xff00ff00;
        System.out.println("native3 Java Bitmap sees native pixels: " + changed);
        bitmap.recycle();
        if (!report.startsWith("native2 bitmap: ok") || !changed) System.exit(1);
    }
}
