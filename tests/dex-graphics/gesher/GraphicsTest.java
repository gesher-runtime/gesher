package gesher;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.graphics.fonts.Font;
import android.graphics.fonts.FontFamily;
import android.view.Surface;
import com.gesher.app.window.NativeWindow;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.nio.ByteBuffer;

/**
 * android.graphics on framework.jar with hwui's natives (docs/graphics.md):
 * Bitmap, Canvas drawing (rects, antialiased circles, paths, matrices), Paint
 * text with a font file, BitmapFactory decoding what Bitmap.compress encoded
 * (PNG, JPEG, WebP); then gesher-app's Win32 window: a Surface over it,
 * lockCanvas/unlockCanvasAndPost, and the window showing the frame.
 *
 *   gesher.GraphicsTest <font file> [<out dir>]
 */
public final class GraphicsTest {
  private static int failures;

  public static void main(String[] args) throws Exception {
    // libandroid_runtime (with hwui's natives), as gesher-app's Main loads it.
    System.loadLibrary("android_runtime");
    String fontPath = args.length > 0 ? args[0] : "/Windows/Fonts/arial.ttf";
    File outDir = args.length > 1 ? new File(args[1]) : null;
    bitmapAndCanvas();
    codecs(outDir);
    text(fontPath);
    window();
    if (failures == 0) {
      System.out.println("GraphicsTest all passed");
    } else {
      System.out.println("GraphicsTest: " + failures + " check(s) failed");
      System.exit(1);
    }
  }

  private static void bitmapAndCanvas() {
    Bitmap bitmap = Bitmap.createBitmap(64, 64, Bitmap.Config.ARGB_8888);
    check(bitmap.getWidth() == 64 && bitmap.getHeight() == 64 && bitmap.isMutable(),
        "Bitmap.createBitmap 64x64 ARGB_8888");
    Canvas canvas = new Canvas(bitmap);
    canvas.drawColor(Color.WHITE);
    Paint paint = new Paint();
    paint.setColor(Color.RED);
    canvas.drawRect(0, 0, 32, 64, paint);
    check(bitmap.getPixel(10, 10) == Color.RED && bitmap.getPixel(50, 10) == Color.WHITE,
        "Canvas.drawRect: red left half, white right half");

    // Android's byte order: ARGB_8888 is R, G, B, A in memory.
    ByteBuffer bytes = ByteBuffer.allocate(bitmap.getByteCount());
    bitmap.copyPixelsToBuffer(bytes);
    check(bytes.get(0) == (byte) 0xff && bytes.get(1) == 0 && bytes.get(2) == 0
            && bytes.get(3) == (byte) 0xff,
        "Bitmap.copyPixelsToBuffer: RGBA bytes");

    paint.setAntiAlias(true);
    paint.setColor(Color.BLUE);
    canvas.drawCircle(48, 48, 10, paint);
    int edge = bitmap.getPixel(48 + 10, 48);  // on the rim: blended
    check(bitmap.getPixel(48, 48) == Color.BLUE && edge != Color.BLUE && edge != Color.WHITE,
        "Canvas.drawCircle, antialiased");

    Path path = new Path();
    path.moveTo(0, 63);
    path.lineTo(20, 40);
    path.lineTo(40, 63);
    path.close();
    paint.setAntiAlias(false);
    paint.setColor(Color.GREEN);
    canvas.drawPath(path, paint);
    check(bitmap.getPixel(20, 60) == Color.GREEN && bitmap.getPixel(2, 42) == Color.RED,
        "Canvas.drawPath: a filled triangle");

    Matrix matrix = new Matrix();
    matrix.setTranslate(5, 7);
    matrix.postScale(2, 2);
    float[] point = {1, 1};
    matrix.mapPoints(point);
    check(point[0] == 12 && point[1] == 16, "Matrix: translate then scale");

    canvas.save();
    canvas.translate(32, 0);
    paint.setColor(Color.YELLOW);
    canvas.drawRect(0, 0, 8, 8, paint);
    canvas.restore();
    check(bitmap.getPixel(36, 4) == Color.YELLOW && bitmap.getPixel(4, 4) == Color.RED,
        "Canvas.translate, save/restore");
  }

  private static void codecs(File outDir) throws Exception {
    Bitmap bitmap = Bitmap.createBitmap(32, 16, Bitmap.Config.ARGB_8888);
    Canvas canvas = new Canvas(bitmap);
    canvas.drawColor(Color.rgb(0x20, 0x60, 0xc0));
    Paint paint = new Paint();
    paint.setColor(Color.rgb(0xf0, 0x80, 0x10));
    canvas.drawRect(0, 0, 16, 16, paint);

    for (Bitmap.CompressFormat format : new Bitmap.CompressFormat[] {
        Bitmap.CompressFormat.PNG, Bitmap.CompressFormat.JPEG, Bitmap.CompressFormat.WEBP_LOSSLESS}) {
      ByteArrayOutputStream out = new ByteArrayOutputStream();
      boolean compressed = bitmap.compress(format, 95, out);
      byte[] data = out.toByteArray();
      Bitmap decoded = BitmapFactory.decodeByteArray(data, 0, data.length);
      int tolerance = format == Bitmap.CompressFormat.JPEG ? 24 : 0;
      check(compressed && decoded != null && decoded.getWidth() == 32 && decoded.getHeight() == 16
              && near(decoded.getPixel(4, 8), Color.rgb(0xf0, 0x80, 0x10), tolerance)
              && near(decoded.getPixel(28, 8), Color.rgb(0x20, 0x60, 0xc0), tolerance),
          "Bitmap.compress " + format + " (" + data.length + " bytes), BitmapFactory.decodeByteArray");
      if (outDir != null && format == Bitmap.CompressFormat.PNG) {
        try (java.io.FileOutputStream f =
            new java.io.FileOutputStream(new File(outDir, "graphics_test.png"))) {
          f.write(data);
        }
      }
    }

    BitmapFactory.Options options = new BitmapFactory.Options();
    options.inJustDecodeBounds = true;
    ByteArrayOutputStream out = new ByteArrayOutputStream();
    bitmap.compress(Bitmap.CompressFormat.PNG, 100, out);
    BitmapFactory.decodeByteArray(out.toByteArray(), 0, out.size(), options);
    check(options.outWidth == 32 && options.outHeight == 16 && "image/png".equals(options.outMimeType),
        "BitmapFactory: inJustDecodeBounds (32x16 image/png)");
  }

  private static void text(String fontPath) {
    File file = new File(fontPath);
    if (!file.exists()) {
      check(false, "font file " + fontPath + " exists");
      return;
    }
    Typeface typeface;
    try {
      Font font = new Font.Builder(file).build();
      typeface = new Typeface.CustomFallbackBuilder(new FontFamily.Builder(font).build()).build();
    } catch (Exception e) {
      e.printStackTrace();
      check(false, "Font.Builder, Typeface.CustomFallbackBuilder (" + fontPath + ")");
      return;
    }
    Paint paint = new Paint();
    paint.setTypeface(typeface);
    paint.setTextSize(32);
    paint.setAntiAlias(true);
    paint.setColor(Color.BLACK);
    float width = paint.measureText("Gesher");
    Rect bounds = new Rect();
    paint.getTextBounds("Gesher", 0, 6, bounds);
    check(width > 60 && width < 200 && bounds.width() > 50 && bounds.height() > 15,
        "Paint.measureText/getTextBounds (" + width + ", " + bounds + ")");

    Bitmap bitmap = Bitmap.createBitmap(160, 48, Bitmap.Config.ARGB_8888);
    Canvas canvas = new Canvas(bitmap);
    canvas.drawColor(Color.WHITE);
    canvas.drawText("Gesher", 8, 36, paint);
    int dark = 0;
    for (int y = 0; y < 48; y++) {
      for (int x = 0; x < 160; x++) {
        if (Color.red(bitmap.getPixel(x, y)) < 128) dark++;
      }
    }
    check(dark > 150 && dark < 3000, "Canvas.drawText: glyphs drawn (" + dark + " dark pixels)");
  }

  private static void window() throws Exception {
    final int w = 240;
    final int h = 160;
    NativeWindow window = NativeWindow.create("Gesher graphics test", w, h);
    window.show();
    check(window.getWidth() == w && window.getHeight() == h, "NativeWindow: a 240x160 window");
    Surface surface = window.getSurface();
    check(surface != null && surface.isValid(), "NativeWindow.getSurface: a valid Surface");

    Canvas canvas = surface.lockCanvas(null);
    check(canvas.getWidth() == w && canvas.getHeight() == h,
        "Surface.lockCanvas: a 240x160 canvas");
    canvas.drawColor(Color.WHITE);
    Paint paint = new Paint();
    paint.setColor(Color.RED);
    canvas.drawRect(0, 0, w / 2, h, paint);
    paint.setColor(Color.rgb(0x20, 0x60, 0xc0));
    canvas.drawRect(w / 2, 0, w, h, paint);
    paint.setColor(Color.GREEN);
    paint.setAntiAlias(true);
    canvas.drawCircle(w / 2, h / 2, 30, paint);
    surface.unlockCanvasAndPost(canvas);
    check(window.getFramesPresented() == 1, "Surface.unlockCanvasAndPost presents a frame");

    Thread.sleep(200);
    int[] back = window.readBackBuffer();
    check(back != null && at(back, 10, 10) == Color.RED
            && at(back, w - 10, 10) == Color.rgb(0x20, 0x60, 0xc0)
            && at(back, w / 2, h / 2) == Color.GREEN,
        "the window's back buffer holds the frame (red, 0x2060c0, green)");
    int[] shown = window.capture();
    check(shown != null && shown[0] == w && shown[1] == h && at(shown, 10, 10) == Color.RED
            && at(shown, w - 10, 10) == Color.rgb(0x20, 0x60, 0xc0)
            && at(shown, w / 2, h / 2) == Color.GREEN,
        "PrintWindow reads the frame back from the window");

    // A partial redraw keeps the rest of the last frame.
    Rect dirty = new Rect(0, 0, 20, 20);
    canvas = surface.lockCanvas(dirty);
    canvas.drawColor(Color.BLACK);
    surface.unlockCanvasAndPost(canvas);
    back = window.readBackBuffer();
    check(at(back, 5, 5) == Color.BLACK && at(back, 30, 30) == Color.RED,
        "Surface.lockCanvas(dirty): a partial redraw");

    surface.release();
    window.close();
  }

  private static int at(int[] capture, int x, int y) {
    return capture[2 + y * capture[0] + x];
  }

  private static boolean near(int a, int b, int tolerance) {
    return Math.abs(Color.red(a) - Color.red(b)) <= tolerance
        && Math.abs(Color.green(a) - Color.green(b)) <= tolerance
        && Math.abs(Color.blue(a) - Color.blue(b)) <= tolerance;
  }

  private static void check(boolean ok, String what) {
    System.out.println((ok ? "PASS  " : "FAIL  ") + what);
    if (!ok) failures++;
  }
}
