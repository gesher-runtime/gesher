package gesher;

import android.graphics.Color;
import android.graphics.HardwareRenderer;
import android.graphics.Paint;
import android.graphics.RecordingCanvas;
import android.graphics.RenderNode;
import android.view.Surface;
import com.gesher.app.window.NativeWindow;

/**
 * hwui's renderer (HardwareRenderer, its RenderThread and the CPU pipeline,
 * SkiaCpu) drawing a RenderNode tree into a Surface over gesher-app's Win32
 * window (docs/graphics.md). The CPU pipeline draws into the window Surface's
 * buffer in place; NativeWindow.present() shows it.
 */
public final class HardwareRendererTest {
  private static int failures;

  public static void main(String[] args) throws Exception {
    // libandroid_runtime (with hwui's natives), as gesher-app's Main loads it.
    System.loadLibrary("android_runtime");
    final int w = 200;
    final int h = 120;
    NativeWindow window = NativeWindow.create("Gesher HardwareRenderer test", w, h);
    window.show();
    Surface surface = window.getSurface();

    RenderNode content = new RenderNode("content");
    content.setPosition(0, 0, w, h);
    RecordingCanvas canvas = content.beginRecording(w, h);
    canvas.drawColor(Color.WHITE);
    Paint paint = new Paint();
    paint.setColor(Color.MAGENTA);
    canvas.drawRect(0, 0, w / 2, h, paint);
    content.endRecording();

    // A child node with its own translation and alpha-free color.
    RenderNode child = new RenderNode("child");
    child.setPosition(0, 0, 40, 40);
    child.setTranslationX(w - 60);
    child.setTranslationY(20);
    RecordingCanvas childCanvas = child.beginRecording(40, 40);
    childCanvas.drawColor(Color.CYAN);
    child.endRecording();
    canvas = content.beginRecording(w, h);
    canvas.drawColor(Color.WHITE);
    canvas.drawRect(0, 0, w / 2, h, paint);
    canvas.drawRenderNode(child);
    content.endRecording();

    HardwareRenderer renderer = new HardwareRenderer();
    renderer.setContentRoot(content);
    renderer.setSurface(surface);
    int result = renderer.createRenderRequest().setWaitForPresent(true).syncAndDraw();
    check(result == HardwareRenderer.SYNC_OK, "HardwareRenderer.syncAndDraw (" + result + ")");
    renderer.fence();
    window.present();

    int[] back = window.readBackBuffer();
    check(at(back, 10, 10) == Color.MAGENTA && at(back, w - 70, 10) == Color.WHITE
            && at(back, w - 40, 40) == Color.CYAN,
        "the RenderNode tree drawn by hwui's CPU pipeline (magenta, white, cyan child)");
    int[] shown = window.capture();
    check(shown != null && at(shown, 10, 10) == Color.MAGENTA && at(shown, w - 40, 40) == Color.CYAN,
        "PrintWindow reads the rendered frame back");

    renderer.destroy();
    surface.release();
    window.close();
    if (failures == 0) {
      System.out.println("HardwareRendererTest all passed");
    } else {
      System.out.println("HardwareRendererTest: " + failures + " check(s) failed");
      System.exit(1);
    }
  }

  private static int at(int[] capture, int x, int y) {
    return capture[2 + y * capture[0] + x];
  }

  private static void check(boolean ok, String what) {
    System.out.println((ok ? "PASS  " : "FAIL  ") + what);
    if (!ok) failures++;
  }
}
