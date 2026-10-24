package gesher;

import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.HardwareRenderer;
import android.graphics.Paint;
import android.graphics.RecordingCanvas;
import android.graphics.RectF;
import android.graphics.RenderNode;
import android.graphics.Typeface;
import android.graphics.fonts.Font;
import android.graphics.fonts.FontFamily;
import android.view.Surface;
import com.gesher.app.graphics.GraphicsDebug;
import com.gesher.app.window.NativeWindow;
import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;

/**
 * hwui's SkiaGL pipeline (Skia's Ganesh over ANGLE's OpenGL ES on Direct3D 11)
 * against its CPU pipeline (docs/angle.md, ADR 0030). The same RenderNode tree
 * (a white and magenta root, a cyan child node drawn with drawRenderNode, text
 * in a font file's Typeface and a 16x16 bitmap of four colors scaled 2x) is
 * rendered by HardwareRenderer into a Win32 window in two processes:
 *
 * <pre>
 *   gesher.HardwareRendererGlTest &lt;font.ttf&gt; &lt;frame file&gt; cpu
 *       the CPU pipeline (the default); saves the frame it presented
 *   GESHER_HWUI_RENDERER=skiagl gesher.HardwareRendererGlTest &lt;font.ttf&gt; &lt;frame file&gt; gl
 *       SkiaGL: an EGL window surface on the window's HWND, eglSwapBuffers; the frame
 *       read back with glReadPixels (GraphicsDebug's hook) and from what Windows
 *       composed (PrintWindow with PW_RENDERFULLCONTENT) is compared with the saved one
 *   GESHER_HWUI_RENDERER=skiagl ... fallback
 *       without ANGLE: hwui says so and renders with the CPU pipeline, the same frame
 * </pre>
 */
public final class HardwareRendererGlTest {
  private static final int W = 240;
  private static final int H = 160;
  // Where the text goes (its pixels are compared by count: the GPU's glyph
  // masks are FreeType's, but sampled through an atlas).
  private static final int TEXT_LEFT = 124;
  private static final int TEXT_TOP = 112;
  private static final int TEXT_RIGHT = 236;
  private static final int TEXT_BOTTOM = 156;

  private static int failures;
  private static Object sKeepAlive;

  public static void main(String[] args) throws Exception {
    String fontPath = args[0];
    File frameFile = new File(args[1]);
    String mode = args[2];
    System.loadLibrary("android_runtime");
    String renderer = GraphicsDebug.renderer();
    System.out.println("INFO  pipeline " + renderer + ": " + GraphicsDebug.rendererDetail());
    boolean gl = renderer.equals("skiagl");
    check(gl == mode.equals("gl"),
        "hwui renders with the " + (mode.equals("gl") ? "SkiaGL" : "CPU") + " pipeline ("
            + renderer + ")");
    if (mode.equals("fallback")) {
      String detail = GraphicsDebug.rendererDetail();
      check(!gl && detail.contains("no EGL display"),
          "skiagl without ANGLE: the CPU pipeline renders (" + detail + ")");
    }
    if (gl) {
      GraphicsDebug.setGlReadback(true);
    }
    Typeface typeface = typeface(fontPath);
    if (typeface == null) {
      finish();
      return;
    }

    NativeWindow window = NativeWindow.create("Gesher HardwareRenderer GL test", W, H);
    window.show();
    Surface surface = window.getSurface();
    RenderNode content = scene(typeface);
    HardwareRenderer hwRenderer = new HardwareRenderer();
    hwRenderer.setContentRoot(content);
    hwRenderer.setSurface(surface);
    int result = hwRenderer.createRenderRequest().setWaitForPresent(true).syncAndDraw();
    check(result == HardwareRenderer.SYNC_OK, "HardwareRenderer.syncAndDraw (" + result + ")");
    hwRenderer.fence();

    if (gl) {
      int[] drawn = GraphicsDebug.lastGlFrame(window.getHwnd());
      check(drawn != null, "glReadPixels read the frame before eglSwapBuffers");
      if (drawn != null) {
        checkScene(drawn, "glReadPixels");
        compare(drawn, readFrame(frameFile), "glReadPixels");
      }
      // DWM composes the swap chain's frame asynchronously.
      int[] shown = null;
      for (int attempt = 0; attempt < 30; attempt++) {
        shown = GraphicsDebug.captureComposed(window.getHwnd());
        if (shown != null && at(shown, 10, 10) == Color.MAGENTA
            && at(shown, W - 40, 40) == Color.CYAN) {
          break;
        }
        Thread.sleep(100);
      }
      check(shown != null, "PrintWindow(PW_RENDERFULLCONTENT) reads the window back");
      if (shown != null) {
        checkScene(shown, "the window (PW_RENDERFULLCONTENT)");
        compare(shown, readFrame(frameFile), "the window (PW_RENDERFULLCONTENT)");
      }
    } else {
      // The CPU pipeline draws into the Surface's buffer in place.
      window.present();
      int[] back = window.readBackBuffer();
      checkScene(back, "the CPU pipeline");
      int[] shown = window.capture();
      check(shown != null && at(shown, 10, 10) == Color.MAGENTA
              && at(shown, W - 40, 40) == Color.CYAN,
          "PrintWindow reads the CPU frame back");
      if (mode.equals("cpu")) {
        writeFrame(frameFile, back);
        System.out.println("INFO  CPU frame saved to " + frameFile);
      } else {
        compare(back, readFrame(frameFile), "the CPU fallback");
      }
    }

    hwRenderer.destroy();
    surface.release();
    window.close();
    finish();
  }

  /**
   * The tree: the root is white with a magenta left half, text and the bitmap
   * on the right; a cyan 40x40 child node drawn with drawRenderNode.
   */
  private static RenderNode scene(Typeface typeface) {
    RenderNode child = new RenderNode("child");
    child.setPosition(0, 0, 40, 40);
    child.setTranslationX(W - 60);
    child.setTranslationY(20);
    RecordingCanvas childCanvas = child.beginRecording(40, 40);
    childCanvas.drawColor(Color.CYAN);
    child.endRecording();

    int[] colors = new int[16 * 16];
    for (int y = 0; y < 16; y++) {
      for (int x = 0; x < 16; x++) {
        colors[y * 16 + x] = y < 8 ? (x < 8 ? Color.RED : Color.GREEN)
                                   : (x < 8 ? Color.BLUE : Color.YELLOW);
      }
    }
    Bitmap bitmap = Bitmap.createBitmap(colors, 16, 16, Bitmap.Config.ARGB_8888);

    RenderNode content = new RenderNode("content");
    content.setPosition(0, 0, W, H);
    RecordingCanvas canvas = content.beginRecording(W, H);
    canvas.drawColor(Color.WHITE);
    Paint magenta = new Paint();
    magenta.setColor(Color.MAGENTA);
    canvas.drawRect(0, 0, W / 2, H, magenta);
    Paint nearest = new Paint();
    nearest.setFilterBitmap(false);
    canvas.drawBitmap(bitmap, null, new RectF(130, 70, 162, 102), nearest);
    Paint text = new Paint();
    text.setTypeface(typeface);
    text.setTextSize(28);
    text.setAntiAlias(true);
    text.setColor(Color.BLACK);
    canvas.drawText("Gesher", 128, 146, text);
    canvas.drawRenderNode(child);
    content.endRecording();
    return content;
  }

  /** The tree's colors where they must be, in a read-back frame. */
  private static void checkScene(int[] f, String what) {
    check(f[0] == W && f[1] == H, what + ": " + f[0] + "x" + f[1]);
    if (f[0] != W || f[1] != H) {
      return;
    }
    check(at(f, 10, 10) == Color.MAGENTA && at(f, W / 2 - 2, H - 2) == Color.MAGENTA
            && at(f, W / 2 + 2, 4) == Color.WHITE,
        what + ": the root's magenta half and white");
    check(at(f, W - 40, 40) == Color.CYAN && at(f, W - 62, 40) == Color.WHITE,
        what + ": the cyan child node");
    check(at(f, 134, 74) == Color.RED && at(f, 158, 74) == Color.GREEN
            && at(f, 134, 98) == Color.BLUE && at(f, 158, 98) == Color.YELLOW,
        what + ": the bitmap, scaled 2x");
    int dark = darkTextPixels(f);
    check(dark > 150, what + ": the text (" + dark + " dark pixels)");
  }

  /** Compares a frame with the CPU pipeline's, pixel by pixel. */
  private static void compare(int[] f, int[] cpu, String what) {
    if (cpu == null || f[0] != cpu[0] || f[1] != cpu[1]) {
      check(false, what + " has the CPU frame's size");
      return;
    }
    int differ = 0;
    int far = 0;
    int worst = 0;
    for (int y = 0; y < H; y++) {
      for (int x = 0; x < W; x++) {
        if (x >= TEXT_LEFT && x < TEXT_RIGHT && y >= TEXT_TOP && y < TEXT_BOTTOM) {
          continue;
        }
        int d = diff(at(f, x, y), at(cpu, x, y));
        worst = Math.max(worst, d);
        if (d > 2) {
          differ++;
        }
      }
    }
    int dark = darkTextPixels(f);
    int darkCpu = darkTextPixels(cpu);
    System.out.println("INFO  " + what + " vs the CPU frame: " + differ
        + " pixels differ by more than 2 outside the text (worst " + worst + "); text " + dark
        + " dark pixels, CPU " + darkCpu);
    check(differ <= W * H / 200, what + ": the same frame as the CPU pipeline outside the text");
    check(Math.abs(dark - darkCpu) <= darkCpu / 10 + 5,
        what + ": the same text as the CPU pipeline (" + dark + " / " + darkCpu + ")");
  }

  private static int darkTextPixels(int[] f) {
    int dark = 0;
    for (int y = TEXT_TOP; y < TEXT_BOTTOM; y++) {
      for (int x = TEXT_LEFT; x < TEXT_RIGHT; x++) {
        if (Color.red(at(f, x, y)) < 128) {
          dark++;
        }
      }
    }
    return dark;
  }

  private static int diff(int a, int b) {
    int d = Math.abs(Color.red(a) - Color.red(b));
    d = Math.max(d, Math.abs(Color.green(a) - Color.green(b)));
    return Math.max(d, Math.abs(Color.blue(a) - Color.blue(b)));
  }

  private static int at(int[] f, int x, int y) {
    return f[2 + y * f[0] + x] | 0xff000000;
  }

  private static void writeFrame(File file, int[] f) throws IOException {
    try (DataOutputStream out =
             new DataOutputStream(new BufferedOutputStream(new FileOutputStream(file)))) {
      out.writeInt(f.length);
      for (int v : f) {
        out.writeInt(v);
      }
    }
  }

  private static int[] readFrame(File file) {
    try (DataInputStream in =
             new DataInputStream(new BufferedInputStream(new FileInputStream(file)))) {
      int[] f = new int[in.readInt()];
      for (int i = 0; i < f.length; i++) {
        f[i] = in.readInt();
      }
      return f;
    } catch (IOException e) {
      System.out.println("INFO  no CPU frame in " + file + ": " + e);
      return null;
    }
  }

  // A Typeface over the font file, without a system fallback (as GraphicsTest makes it).
  private static Typeface typeface(String fontPath) {
    try {
      Font font = new Font.Builder(new File(fontPath)).build();
      FontFamily family = new FontFamily.Builder(font).build();
      Method fromArray = Typeface.class.getDeclaredMethod(
          "nativeCreateFromArray", long[].class, long.class, int.class, int.class);
      fromArray.setAccessible(true);
      long nativeTypeface =
          (Long) fromArray.invoke(null, new long[] {family.getNativePtr()}, 0L, 400, 0);
      Constructor<Typeface> make = Typeface.class.getDeclaredConstructor(long.class, String.class);
      make.setAccessible(true);
      sKeepAlive = family;
      return make.newInstance(nativeTypeface, null);
    } catch (Exception e) {
      e.printStackTrace();
      check(false, "a Typeface over " + fontPath);
      return null;
    }
  }

  private static void finish() {
    if (failures == 0) {
      System.out.println("HardwareRendererGlTest all passed");
    } else {
      System.out.println("HardwareRendererGlTest: " + failures + " check(s) failed");
      System.exit(1);
    }
  }

  private static void check(boolean ok, String what) {
    System.out.println((ok ? "PASS  " : "FAIL  ") + what);
    if (!ok) {
      failures++;
    }
  }
}
