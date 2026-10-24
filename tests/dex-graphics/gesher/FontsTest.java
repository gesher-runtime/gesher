package gesher;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.graphics.fonts.Font;
import android.graphics.fonts.FontFamily;
import android.graphics.fonts.SystemFonts;
import com.gesher.app.fonts.SystemFontMap;
import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * The system fonts (docs/fonts.md): gesher-app's SystemFontMap installs the font map
 * from $ANDROID_ROOT/etc/fonts.xml and $ANDROID_ROOT/fonts through a SharedMemory, as
 * an app process gets it from the system server; then Typeface.DEFAULT and the other
 * static typefaces, the named families and aliases, SystemFonts.getAvailableFonts, and
 * text through minikin: Latin in Roboto, Hebrew from the fallback chain in Noto Sans
 * Hebrew (same widths and pixels as that font alone), bold, italic, serif, monospace.
 *
 *   gesher.FontsTest      (with $ANDROID_ROOT set and the fonts installed there)
 */
public final class FontsTest {
  private static final String LATIN = "Gesher";
  private static final String HEBREW = "שלום";  // shalom
  private static int failures;
  private static final List<Object> sKeepAlive = new ArrayList<>();

  public static void main(String[] args) throws Exception {
    System.loadLibrary("android_runtime");
    String how = null;
    try {
      how = SystemFontMap.load();
    } catch (Throwable t) {
      t.printStackTrace();
    }
    check(how != null, "system font map from " + SystemFontMap.fontsXml() + ": " + how);
    if (how != null) {
      check(how.contains("through SharedMemory"),
          "font map serialized into a SharedMemory and installed from it (Typeface.setSystemFontMap)");
      families();
      defaults();
      available();
      latin();
      hebrew();
      styles();
    }
    if (failures == 0) {
      System.out.println("FontsTest all passed");
    } else {
      System.out.println("FontsTest: " + failures + " check(s) failed");
      System.exit(1);
    }
  }

  private static void families() {
    Map<String, Typeface> map = Typeface.getSystemFontMap();
    String[] names = {"sans-serif", "sans-serif-medium", "sans-serif-condensed", "serif",
        "monospace", "arial", "times", "courier"};
    List<String> missing = new ArrayList<>();
    for (String name : names) {
      if (!map.containsKey(name)) missing.add(name);
    }
    // courier is an alias of serif-monospace, which Gesher's fonts.xml leaves out.
    check(missing.size() == 1 && missing.get(0).equals("courier"),
        "named families and aliases (" + map.size() + " in the map; missing " + missing + ")");
  }

  private static void defaults() {
    boolean ok;
    String detail;
    try {
      long def = Typeface.DEFAULT.getNativeInstance();
      long bold = Typeface.DEFAULT_BOLD.getNativeInstance();
      ok = def != 0 && bold != 0 && def != bold
          && Typeface.SANS_SERIF.getNativeInstance() != 0
          && Typeface.SERIF.getNativeInstance() != 0
          && Typeface.MONOSPACE.getNativeInstance() != 0;
      detail = "DEFAULT " + Typeface.DEFAULT.getWeight() + ", DEFAULT_BOLD "
          + Typeface.DEFAULT_BOLD.getWeight();
    } catch (RuntimeException e) {
      e.printStackTrace();
      ok = false;
      detail = e.toString();
    }
    check(ok, "Typeface.DEFAULT, DEFAULT_BOLD, SANS_SERIF, SERIF, MONOSPACE initialized ("
        + detail + ")");
    check(Typeface.DEFAULT.getStyle() == Typeface.NORMAL && Typeface.DEFAULT_BOLD.isBold()
            && Typeface.DEFAULT_BOLD.getWeight() == 700,
        "Typeface.DEFAULT is normal, DEFAULT_BOLD bold (700)");
    Typeface bold = Typeface.create("sans-serif", Typeface.BOLD);
    Typeface italic = Typeface.create("sans-serif", Typeface.ITALIC);
    Typeface light = Typeface.create(Typeface.DEFAULT, 300, false);
    Typeface medium = Typeface.create("sans-serif-medium", Typeface.NORMAL);
    check(bold.isBold() && italic.isItalic() && !italic.isBold() && light.getWeight() == 300
            && medium.getWeight() == 500,
        "Typeface.create: sans-serif bold/italic, weight 300, sans-serif-medium (500)");
  }

  private static void available() {
    Set<Font> fonts = SystemFonts.getAvailableFonts();
    String dir = new File(SystemFontMap.fontDir()).getAbsolutePath();
    boolean roboto = false;
    boolean hebrew = false;
    boolean inDir = true;
    for (Font font : fonts) {
      File file = font.getFile();
      if (file == null) continue;
      inDir &= file.getAbsolutePath().startsWith(dir);
      roboto |= file.getName().equals("Roboto-Regular.ttf");
      hebrew |= file.getName().equals("NotoSansHebrew-Regular.ttf");
    }
    check(!fonts.isEmpty() && roboto && hebrew && inDir,
        "SystemFonts.getAvailableFonts: " + fonts.size() + " fonts in " + dir
            + " (Roboto " + roboto + ", Noto Sans Hebrew " + hebrew + ")");
  }

  private static void latin() {
    Paint def = paint(null);
    Paint withDefault = paint(Typeface.DEFAULT);
    float width = def.measureText(LATIN);
    check(width > 60 && width < 200 && Math.abs(width - withDefault.measureText(LATIN)) < 0.01f,
        "Paint.measureText with the default typeface (" + width + ")");
    Typeface roboto = fileTypeface("Roboto-Regular.ttf");
    if (roboto != null) {
      float robotoWidth = paint(roboto).measureText(LATIN);
      check(Math.abs(robotoWidth - width) < 0.05f,
          "Latin text in Roboto (" + width + " = Roboto-Regular.ttf alone " + robotoWidth + ")");
    }
    int dark = darkPixels(draw(def, LATIN));
    check(dark > 150 && dark < 3000, "Canvas.drawText Latin, default typeface (" + dark
        + " dark pixels)");
  }

  private static void hebrew() {
    Paint def = paint(null);
    Typeface noto = fileTypeface("NotoSansHebrew-Regular.ttf");
    Typeface roboto = fileTypeface("Roboto-Regular.ttf");
    if (noto == null || roboto == null) return;
    Paint notoPaint = paint(noto);
    Paint robotoPaint = paint(roboto);
    check(def.hasGlyph("ש") && notoPaint.hasGlyph("ש") && !robotoPaint.hasGlyph("ש"),
        "Paint.hasGlyph(shin): default typeface (fallback) and Noto Sans Hebrew yes, Roboto no");
    float width = def.measureText(HEBREW);
    float notoWidth = notoPaint.measureText(HEBREW);
    float robotoWidth = robotoPaint.measureText(HEBREW);
    check(width > 20 && Math.abs(width - notoWidth) < 0.05f && Math.abs(width - robotoWidth) > 1,
        "Hebrew from the fallback chain in Noto Sans Hebrew (width " + width + " = Noto "
            + notoWidth + ", Roboto alone " + robotoWidth + ")");
    int[] drawn = pixels(draw(def, HEBREW));
    int[] expected = pixels(draw(notoPaint, HEBREW));
    int dark = darkPixels(drawn);
    check(dark > 100 && java.util.Arrays.equals(drawn, expected),
        "Canvas.drawText Hebrew, default typeface: Noto Sans Hebrew's glyphs (" + dark
            + " dark pixels, identical)");
    // Right to left: the last letter (final mem) is drawn leftmost, where the first
    // (shin) would be in left-to-right order.
    int[] lastAlone = pixels(draw(def, HEBREW.substring(3)));
    int[] firstAlone = pixels(draw(def, HEBREW.substring(0, 1)));
    int columns = 8 + (int) def.measureText(HEBREW.substring(3)) - 1;
    int asLast = differing(drawn, lastAlone, columns);
    int asFirst = differing(drawn, firstAlone, columns);
    check(asLast < asFirst, "Hebrew drawn right to left (the leftmost glyph is the last"
        + " letter: " + asLast + " pixels differ from it, " + asFirst + " from the first)");
    rtlLayout(def);

    String mixed = LATIN + " " + HEBREW;
    float mixedWidth = def.measureText(mixed);
    check(mixedWidth > def.measureText(LATIN) + width,
        "mixed Latin and Hebrew (bidi) measured (" + mixedWidth + ")");
  }

  private static void styles() {
    Paint regular = paint(Typeface.DEFAULT);
    Paint bold = paint(Typeface.DEFAULT_BOLD);
    // Advances are whole pixels (hinted), which at 32 px rounds Roboto's 400 and 700
    // instances of "Gesher" (100.7 and 102.5 px) to the same 102: widths at 200 px.
    Paint regularLarge = paint(Typeface.DEFAULT);
    Paint boldLarge = paint(Typeface.DEFAULT_BOLD);
    regularLarge.setTextSize(200);
    boldLarge.setTextSize(200);
    float regularWidth = regularLarge.measureText(LATIN);
    float boldWidth = boldLarge.measureText(LATIN);
    int regularDark = darkPixels(draw(regular, LATIN));
    int boldDark = darkPixels(draw(bold, LATIN));
    check(boldWidth > regularWidth + 5 && boldDark > regularDark * 1.15,
        "bold differs from regular (at 200 px " + boldWidth + " > " + regularWidth + " wide, "
            + boldDark + " > " + regularDark + " dark pixels)");
    Paint createdBoldPaint = paint(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
    createdBoldPaint.setTextSize(200);
    float createdBold = createdBoldPaint.measureText(LATIN);
    check(Math.abs(createdBold - boldWidth) < 0.01f,
        "Typeface.create(DEFAULT, BOLD) is DEFAULT_BOLD's face (" + createdBold + ")");

    int[] upright = pixels(draw(regular, LATIN));
    int[] slanted = pixels(draw(paint(Typeface.create(Typeface.DEFAULT, Typeface.ITALIC)), LATIN));
    int differ = 0;
    for (int i = 0; i < upright.length; i++) {
      if (upright[i] != slanted[i]) differ++;
    }
    check(differ > 100, "italic differs from upright (" + differ + " pixels)");

    Typeface notoBold = fileTypeface("NotoSansHebrew-Bold.ttf");
    if (notoBold != null) {
      float hebrewBold = bold.measureText(HEBREW);
      float notoBoldWidth = paint(notoBold).measureText(HEBREW);
      check(Math.abs(hebrewBold - notoBoldWidth) < 0.05f
              && Math.abs(hebrewBold - regular.measureText(HEBREW)) > 0.5f,
          "bold Hebrew from the fallback chain in Noto Sans Hebrew Bold (" + hebrewBold + " = "
              + notoBoldWidth + ")");
    }

    Paint mono = paint(Typeface.MONOSPACE);
    float narrow = mono.measureText("iiii");
    float wide = mono.measureText("WWWW");
    check(narrow > 10 && Math.abs(narrow - wide) < 0.01f
            && Math.abs(regular.measureText("iiii") - regular.measureText("WWWW")) > 5,
        "monospace: iiii and WWWW equally wide (" + narrow + ", " + wide + ")");
    // Drawn too (e2e: Editor's monospace EditText showed no text).
    int monoDark = darkPixels(draw(mono, LATIN));
    check(monoDark > 150 && monoDark < 3000,
        "Canvas.drawText Latin, monospace (" + monoDark + " dark pixels)");

    Typeface notoSerif = fileTypeface("NotoSerif-Regular.ttf");
    if (notoSerif != null) {
      float serif = paint(Typeface.SERIF).measureText(LATIN);
      float serifAlone = paint(notoSerif).measureText(LATIN);
      check(Math.abs(serif - serifAlone) < 0.05f
              && Math.abs(serif - regular.measureText(LATIN)) > 1,
          "serif is Noto Serif (" + serif + " = " + serifAlone + ", sans-serif "
              + regular.measureText(LATIN) + ")");
    }
  }

  /** A Hebrew paragraph through StaticLayout (minikin's line breaker): right to left. */
  private static void rtlLayout(Paint paint) {
    try {
      android.text.TextPaint textPaint = new android.text.TextPaint(paint);
      String text = HEBREW + " " + HEBREW;
      android.text.StaticLayout layout = android.text.StaticLayout.Builder
          .obtain(text, 0, text.length(), textPaint, 400).build();
      check(layout.getLineCount() == 1
              && layout.getParagraphDirection(0) == android.text.Layout.DIR_RIGHT_TO_LEFT
              && layout.getLineLeft(0) > 100 && Math.abs(layout.getLineRight(0) - 400) < 1,
          "StaticLayout: a Hebrew paragraph is right to left, aligned right (line "
              + layout.getLineLeft(0) + ".." + layout.getLineRight(0) + " of 400)");
    } catch (Throwable t) {
      t.printStackTrace();
      check(false, "StaticLayout: a Hebrew paragraph (" + t + ")");
    }
  }

  // --- helpers --------------------------------------------------------------------------

  private static Paint paint(Typeface typeface) {
    Paint paint = new Paint();
    if (typeface != null) paint.setTypeface(typeface);
    paint.setTextSize(32);
    paint.setAntiAlias(true);
    paint.setColor(Color.BLACK);
    return paint;
  }

  private static Bitmap draw(Paint paint, String text) {
    Bitmap bitmap = Bitmap.createBitmap(200, 48, Bitmap.Config.ARGB_8888);
    Canvas canvas = new Canvas(bitmap);
    canvas.drawColor(Color.WHITE);
    canvas.drawText(text, 8, 36, paint);
    return bitmap;
  }

  private static int[] pixels(Bitmap bitmap) {
    int[] out = new int[bitmap.getWidth() * bitmap.getHeight()];
    bitmap.getPixels(out, 0, bitmap.getWidth(), 0, 0, bitmap.getWidth(), bitmap.getHeight());
    return out;
  }

  /** Pixels that differ between two 200x48 drawings in the columns left of {@code columns}. */
  private static int differing(int[] a, int[] b, int columns) {
    int n = 0;
    for (int y = 0; y < 48; y++) {
      for (int x = 0; x < columns && x < 200; x++) {
        if (a[y * 200 + x] != b[y * 200 + x]) n++;
      }
    }
    return n;
  }

  private static int darkPixels(Bitmap bitmap) {
    return darkPixels(pixels(bitmap));
  }

  private static int darkPixels(int[] pixels) {
    int dark = 0;
    for (int c : pixels) {
      if (Color.red(c) < 128) dark++;
    }
    return dark;
  }

  /**
   * A Typeface over one font file of $ANDROID_ROOT/fonts and nothing else (no system
   * fallback): what the default typeface's text is compared with.
   */
  private static Typeface fileTypeface(String name) {
    File file = new File(SystemFontMap.fontDir() + name);
    try {
      Font font = new Font.Builder(file).build();
      FontFamily family = new FontFamily.Builder(font).build();
      Method fromArray = Typeface.class.getDeclaredMethod(
          "nativeCreateFromArray", long[].class, long.class, int.class, int.class);
      fromArray.setAccessible(true);
      long nativeTypeface = (Long) fromArray.invoke(null, new long[] {family.getNativePtr()}, 0L,
          font.getStyle().getWeight(), font.getStyle().getSlant());
      Constructor<Typeface> make = Typeface.class.getDeclaredConstructor(long.class, String.class);
      make.setAccessible(true);
      // The family's native object goes with it: keep it while the Typeface uses it.
      sKeepAlive.add(family);
      return make.newInstance(nativeTypeface, null);
    } catch (Exception e) {
      e.printStackTrace();
      check(false, "a Typeface over " + file + " alone");
      return null;
    }
  }

  private static void check(boolean ok, String what) {
    System.out.println((ok ? "PASS  " : "FAIL  ") + what);
    if (!ok) failures++;
  }
}
