package gesher;

import android.graphics.Paint;
import android.graphics.Typeface;

/** Managed default-map contract without Main or SystemFontMap.load(). */
public final class Native3DefaultFontTest {
    public static void main(String[] args) {
        boolean baseline = args.length == 1 && args[0].equals("--expect-uninitialized");
        System.loadLibrary("android_runtime");
        try {
            Paint paint = new Paint();
            paint.setTypeface(Typeface.DEFAULT);
            paint.setTextSize(32);
            float latin = paint.measureText("Gesher");
            float hebrew = paint.measureText("שלום");
            if (!(latin > 0 && Float.isFinite(latin) && hebrew > 0 && Float.isFinite(hebrew)
                    && paint.hasGlyph("A") && paint.hasGlyph("א"))) {
                throw new AssertionError("Default font has no meaningful Latin/Hebrew glyphs");
            }
            if (baseline) throw new AssertionError("Baseline unexpectedly initialized its font map");
            System.out.println("FONT_DEX_DEFAULT_PASS latin=" + latin + " hebrew=" + hebrew);
        } catch (IllegalStateException failure) {
            if (!baseline || !"The Typeface is not fully initialized.".equals(failure.getMessage())) {
                throw failure;
            }
            boolean fromTypeface = false;
            for (StackTraceElement frame : failure.getStackTrace()) {
                if (frame.getClassName().equals("android.graphics.Typeface")) fromTypeface = true;
            }
            if (!fromTypeface) throw failure;
            System.out.println("FONT_DEX_DEFAULT_BASELINE_UNINITIALIZED " + failure.getMessage());
        }
    }
}
