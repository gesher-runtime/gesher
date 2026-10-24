package gesher;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.graphics.fonts.Font;
import android.graphics.fonts.FontFamily;
import android.graphics.text.PositionedGlyphs;
import android.graphics.text.TextRunShaper;
import android.os.LocaleList;
import android.text.Layout;
import android.text.StaticLayout;
import android.text.TextPaint;
import com.gesher.app.fonts.SystemFontMap;
import com.gesher.app.fonts.WindowsFonts;
import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * Scripts beyond Latin and Hebrew through the system font map (docs/fonts.md, ADR 0044):
 * the bundled Noto fonts for Arabic (shaped and joined, right to left), Devanagari
 * (conjuncts) and Thai; Roboto for Cyrillic and Greek; and the fallback families of
 * Windows' own fonts, used in place: CJK (by locale), color emoji, symbols. "Not tofu" is
 * checked against a code point no font has, drawn as the default font's .notdef.
 *
 *   gesher.ScriptsTest    (with $ANDROID_ROOT's fonts and, through $GESHER_WINDOWS_FONTS
 *                          or %WINDIR%\Fonts on the current drive, Windows' fonts)
 */
public final class ScriptsTest {
  private static final String SEEN = "س";
  private static final String ZWNJ = "‌";
  private static final String MARHABA = "مرحبا";
  private static final String NAMASTE = "नमस्ते";
  private static final String KSSA = "क्ष";  // ka virama ssa: a conjunct
  private static final String THAI = "สวัสดี";  // sawasdee
  private static final String CYRILLIC = "Привет";
  private static final String GREEK = "Γειά";
  private static final String HAN = "直";  // differs between Chinese and Japanese forms
  private static final String TOFU = "͸";  // unassigned
  private static final String EMOJI = "😀";  // grinning face
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
    check(how != null, "system font map: " + how);
    if (how == null) {
      System.exit(1);
    }
    String windows = WindowsFonts.directory();
    check(windows != null && how.contains("of Windows' from"),
        "Windows' fonts appended to the fallback chain, in place from " + windows
            + (windows == null ? " (" + WindowsFonts.whyNot() + ")" : ""));
    arabic();
    devanagari();
    thai();
    cyrillicGreek();
    if (windows != null) {
      cjk(windows);
      emoji(windows);
    }
    if (failures == 0) {
      System.out.println("ScriptsTest all passed");
    } else {
      System.out.println("ScriptsTest: " + failures + " check(s) failed");
      System.exit(1);
    }
  }

  private static void arabic() {
    Paint def = paint(null);
    Typeface ui = fileTypeface(SystemFontMap.fontDir() + "NotoNaskhArabicUI-Regular.ttf", 0);
    if (ui == null) return;
    String joined = SEEN + SEEN + SEEN + SEEN;
    String apart = SEEN + ZWNJ + SEEN + ZWNJ + SEEN + ZWNJ + SEEN;
    float width = def.measureText(joined);
    float uiWidth = paint(ui).measureText(joined);
    check(def.hasGlyph(SEEN) && width > 20 && Math.abs(width - uiWidth) < 0.05f,
        "Arabic from the fallback chain in Noto Naskh Arabic UI (width " + width + " = "
            + uiWidth + ")");
    // Joined letters take their initial, medial and final forms (other glyphs than the
    // isolated letter); with ZWNJ between them each stays isolated.
    int isolated = glyphs(def, SEEN, true).iterator().next();
    Set<Integer> joinedGlyphs = glyphs(def, joined, true);
    Set<Integer> apartGlyphs = glyphs(def, apart, true);
    check(joinedGlyphs.size() >= 3 && !joinedGlyphs.contains(isolated)
            && apartGlyphs.contains(isolated),
        "Arabic shaped: four joined seens are glyphs " + joinedGlyphs + " (initial, medial,"
            + " final), apart (ZWNJ) the isolated " + apartGlyphs + ", alone " + isolated);
    int joinedParts = components(pixels(draw(def, joined)));
    int apartParts = components(pixels(draw(def, apart)));
    float apartWidth = def.measureText(apart);
    check(joinedParts < apartParts && width < apartWidth,
        "Arabic joined when drawn: " + joinedParts + " connected shapes (" + width
            + " px) against " + apartParts + " kept apart (" + apartWidth + " px)");
    TextPaint tp = new TextPaint(def);
    StaticLayout layout = StaticLayout.Builder.obtain(MARHABA, 0, MARHABA.length(), tp, 400)
        .build();
    float first = layout.getPrimaryHorizontal(0);
    float end = layout.getPrimaryHorizontal(MARHABA.length());
    check(layout.getParagraphDirection(0) == Layout.DIR_RIGHT_TO_LEFT && first > end
            && Math.abs(layout.getLineRight(0) - 400) < 1,
        "Arabic paragraph right to left, aligned right (offset 0 at x " + first + ", the end"
            + " at " + end + ")");
  }

  private static void devanagari() {
    Paint def = paint(null);
    Typeface ui = fileTypeface(SystemFontMap.fontDir() + "NotoSansDevanagariUI-VF.ttf", 0);
    if (ui == null) return;
    float width = def.measureText(NAMASTE);
    float uiWidth = paint(ui).measureText(NAMASTE);
    int conjunct = glyphCount(def, KSSA, false);
    check(def.hasGlyph(NAMASTE.substring(0, 1)) && Math.abs(width - uiWidth) < 0.05f
            && conjunct < KSSA.length(),
        "Devanagari in Noto Sans Devanagari UI (width " + width + " = " + uiWidth
            + "), ka-virama-ssa shaped into " + conjunct + " glyph(s)");
  }

  private static void thai() {
    Paint def = paint(null);
    Typeface ui = fileTypeface(SystemFontMap.fontDir() + "NotoSansThaiUI-Regular.ttf", 0);
    if (ui == null) return;
    float width = def.measureText(THAI);
    float uiWidth = paint(ui).measureText(THAI);
    check(def.hasGlyph(THAI.substring(0, 1)) && width > 20 && Math.abs(width - uiWidth) < 0.05f,
        "Thai in Noto Sans Thai UI (width " + width + " = " + uiWidth + ")");
  }

  private static void cyrillicGreek() {
    Paint def = paint(null);
    Typeface roboto = fileTypeface(SystemFontMap.fontDir() + "Roboto-Regular.ttf", 0);
    if (roboto == null) return;
    Paint r = paint(roboto);
    check(Math.abs(def.measureText(CYRILLIC) - r.measureText(CYRILLIC)) < 0.05f
            && Math.abs(def.measureText(GREEK) - r.measureText(GREEK)) < 0.05f
            && def.measureText(CYRILLIC) > 40,
        "Cyrillic and Greek in Roboto (" + def.measureText(CYRILLIC) + ", "
            + def.measureText(GREEK) + ")");
  }

  private static void cjk(String windows) {
    Paint def = paint(null);
    int[] tofu = pixels(draw(def, TOFU));
    int tofuInk = darkPixels(tofu);
    String[] samples = {"中文", "日本語", "한국어"};
    String[] names = {"Chinese", "Japanese", "Korean"};
    for (int i = 0; i < samples.length; i++) {
      String s = samples[i];
      int[] drawn = pixels(draw(def, s.substring(0, 1)));
      int ink = darkPixels(drawn);
      boolean notTofu = !java.util.Arrays.equals(drawn, tofu) && ink > tofuInk + 40;
      check(def.hasGlyph(s) && !def.hasGlyph(TOFU) && notTofu,
          names[i] + " " + s + " has glyphs, not tofu (" + ink + " dark pixels, the .notdef of "
              + "U+0378 " + tofuInk + ")");
    }
    if (new File(windows + "msyh.ttc").isFile()) {
      Typeface yahei = fileTypeface(windows + "msyh.ttc", 0);
      if (yahei != null) {
        Paint zh = paint(null);
        zh.setTextLocales(LocaleList.forLanguageTags("zh-CN"));
        float w = zh.measureText("中文");
        float alone = paint(yahei).measureText("中文");
        int[] a = pixels(draw(zh, HAN));
        int[] b = pixels(draw(paint(yahei), HAN));
        check(Math.abs(w - alone) < 0.05f && java.util.Arrays.equals(a, b),
            "Chinese (zh-CN) from Microsoft YaHei, Windows' msyh.ttc in place (width " + w
                + " = " + alone + ", same pixels)");
      }
    }
    if (new File(windows + "YuGothR.ttc").isFile() && new File(windows + "msyh.ttc").isFile()) {
      Paint ja = paint(null);
      ja.setTextLocales(LocaleList.forLanguageTags("ja-JP"));
      Paint zh = paint(null);
      zh.setTextLocales(LocaleList.forLanguageTags("zh-CN"));
      int diff = differing(pixels(draw(ja, HAN)), pixels(draw(zh, HAN)));
      check(diff > 20, "the text locale picks the CJK font: " + HAN + " in Japanese (Yu Gothic)"
          + " and Chinese (YaHei) differ in " + diff + " pixels");
    }
  }

  private static void emoji(String windows) {
    Paint def = paint(null);
    def.setTextSize(40);
    int[] drawn = pixels(draw(def, EMOJI));
    int colorful = colorful(drawn);
    check(def.hasGlyph(EMOJI) && colorful > 100,
        "emoji drawn in color (" + colorful + " colored pixels)");
    if (new File(windows + "seguiemj.ttf").isFile()) {
      Typeface segoe = fileTypeface(windows + "seguiemj.ttf", 0);
      if (segoe != null) {
        Paint alone = paint(segoe);
        alone.setTextSize(40);
        check(Math.abs(def.measureText(EMOJI) - alone.measureText(EMOJI)) < 0.05f,
            "emoji from Segoe UI Emoji, Windows' seguiemj.ttf in place (width "
                + def.measureText(EMOJI) + ")");
      }
    }
    String symbol = "☃";  // snowman, text style
    check(def.hasGlyph(symbol) && darkPixels(pixels(draw(def, symbol))) + colorful(
            pixels(draw(def, symbol))) > 20, "a symbol (U+2603) drawn");
  }

  // --- helpers --------------------------------------------------------------------------

  /** The distinct glyph ids of a run, shaped as one direction. */
  private static Set<Integer> glyphs(Paint paint, String text, boolean rtl) {
    PositionedGlyphs g = TextRunShaper.shapeTextRun(text, 0, text.length(), 0, text.length(),
        0f, 0f, rtl, paint);
    Set<Integer> out = new HashSet<>();
    for (int i = 0; i < g.glyphCount(); i++) {
      out.add(g.getGlyphId(i));
    }
    return out;
  }

  private static int glyphCount(Paint paint, String text, boolean rtl) {
    return TextRunShaper.shapeTextRun(text, 0, text.length(), 0, text.length(), 0f, 0f, rtl,
        paint).glyphCount();
  }

  private static Paint paint(Typeface typeface) {
    Paint paint = new Paint();
    if (typeface != null) paint.setTypeface(typeface);
    paint.setTextSize(32);
    paint.setAntiAlias(true);
    paint.setColor(Color.BLACK);
    return paint;
  }

  private static final int W = 240;
  private static final int H = 64;

  private static Bitmap draw(Paint paint, String text) {
    Bitmap bitmap = Bitmap.createBitmap(W, H, Bitmap.Config.ARGB_8888);
    Canvas canvas = new Canvas(bitmap);
    canvas.drawColor(Color.WHITE);
    canvas.drawText(text, 8, 46, paint);
    return bitmap;
  }

  private static int[] pixels(Bitmap bitmap) {
    int[] out = new int[bitmap.getWidth() * bitmap.getHeight()];
    bitmap.getPixels(out, 0, bitmap.getWidth(), 0, 0, bitmap.getWidth(), bitmap.getHeight());
    return out;
  }

  private static int differing(int[] a, int[] b) {
    int n = 0;
    for (int i = 0; i < a.length; i++) {
      if (a[i] != b[i]) n++;
    }
    return n;
  }

  private static boolean dark(int c) {
    return Color.red(c) < 128 && Color.green(c) < 128 && Color.blue(c) < 128;
  }

  private static int darkPixels(int[] pixels) {
    int n = 0;
    for (int c : pixels) {
      if (dark(c)) n++;
    }
    return n;
  }

  /** Pixels clearly not gray: a color emoji's, not black text's antialiasing. */
  private static int colorful(int[] pixels) {
    int n = 0;
    for (int c : pixels) {
      int r = Color.red(c);
      int g = Color.green(c);
      int b = Color.blue(c);
      int max = Math.max(r, Math.max(g, b));
      int min = Math.min(r, Math.min(g, b));
      if (max - min > 80) n++;
    }
    return n;
  }

  /** Shapes of dark pixels, 8-connected. */
  private static int components(int[] pixels) {
    boolean[] seen = new boolean[pixels.length];
    int[] stack = new int[pixels.length];
    int n = 0;
    for (int i = 0; i < pixels.length; i++) {
      if (seen[i] || !dark(pixels[i])) continue;
      n++;
      int top = 0;
      stack[top++] = i;
      seen[i] = true;
      while (top > 0) {
        int p = stack[--top];
        int x = p % W;
        int y = p / W;
        for (int dy = -1; dy <= 1; dy++) {
          for (int dx = -1; dx <= 1; dx++) {
            int nx = x + dx;
            int ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
            int q = ny * W + nx;
            if (!seen[q] && dark(pixels[q])) {
              seen[q] = true;
              stack[top++] = q;
            }
          }
        }
      }
    }
    return n;
  }

  /** A Typeface over one font file (a collection's font {@code index}) and nothing else. */
  private static Typeface fileTypeface(String path, int index) {
    File file = new File(path);
    try {
      Font font = new Font.Builder(file).setTtcIndex(index).build();
      FontFamily family = new FontFamily.Builder(font).build();
      Method fromArray = Typeface.class.getDeclaredMethod(
          "nativeCreateFromArray", long[].class, long.class, int.class, int.class);
      fromArray.setAccessible(true);
      long nativeTypeface = (Long) fromArray.invoke(null, new long[] {family.getNativePtr()}, 0L,
          font.getStyle().getWeight(), font.getStyle().getSlant());
      Constructor<Typeface> make = Typeface.class.getDeclaredConstructor(long.class, String.class);
      make.setAccessible(true);
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
