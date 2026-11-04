package gesher;

import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.SurfaceTexture;
import android.opengl.EGL14;
import android.opengl.GLES20;
import android.opengl.GLUtils;
import android.os.Handler;
import android.os.HandlerThread;
import android.view.Surface;
import com.gesher.app.graphics.GraphicsDebug;
import com.gesher.app.window.NativeWindow;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import javax.microedition.khronos.egl.EGL10;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.egl.EGLContext;
import javax.microedition.khronos.egl.EGLDisplay;
import javax.microedition.khronos.egl.EGLSurface;
import javax.microedition.khronos.opengles.GL10;

/**
 * The framework's Java OpenGL ES and EGL bindings over ANGLE (docs/angle.md, "Java
 * apps"): what GLSurfaceView and apps' renderers call.
 *
 * <pre>
 *   gesher.GlBindingsTest
 * </pre>
 *
 * <ul>
 *   <li>EGL10 (EGLContext.getEGL(), com.google.android.gles_jni.EGLImpl): the default
 *       display, an RGBA8 ES 2 config, a context and a pbuffer; GLES20 shaders draw a
 *       quad, glReadPixels reads it back; GLUtils.texImage2D uploads a Bitmap that a
 *       textured quad shows;
 *   <li>GL10 (GLImpl) on an ES 1 context: glClear and glReadPixels;
 *   <li>an EGL10 window surface on a Win32 window's Surface (an HWND: ANGLE's swap
 *       chain), one frame swapped and read back from what Windows composed;
 *   <li>window surfaces on a Surface over a SurfaceTexture (what a SurfaceView's
 *       surface is) with EGL10, and on the SurfaceTexture itself with EGL10 and EGL14:
 *       the swapped frame reaches the SurfaceTexture (onFrameAvailable).
 * </ul>
 */
public final class GlBindingsTest {
  private static final int EGL_CONTEXT_CLIENT_VERSION = 0x3098;
  private static final int EGL_OPENGL_ES2_BIT = 4;
  private static final int W = 64;
  private static final int H = 64;

  private static int failures;

  public static void main(String[] args) throws Exception {
    System.loadLibrary("android_runtime");
    EGL10 egl = (EGL10) EGLContext.getEGL();
    EGLDisplay display = egl.eglGetDisplay(EGL10.EGL_DEFAULT_DISPLAY);
    int[] version = new int[2];
    check(display != EGL10.EGL_NO_DISPLAY && egl.eglInitialize(display, version),
        "EGL10: the default display initializes (EGL " + version[0] + "." + version[1] + ")");
    EGLConfig config = chooseConfig(egl, display);
    check(config != null, "EGL10: an RGBA8 ES 2 window and pbuffer config");
    if (config == null) {
      finish();
      return;
    }
    pbuffer(egl, display, config);
    gl10(egl, display);
    window(egl, display, config);
    surfaceTexture(egl, display, config);
    egl.eglTerminate(display);
    finish();
  }

  private static EGLConfig chooseConfig(EGL10 egl, EGLDisplay display) {
    int[] attribs = {EGL10.EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL10.EGL_RED_SIZE, 8,
        EGL10.EGL_GREEN_SIZE, 8, EGL10.EGL_BLUE_SIZE, 8, EGL10.EGL_ALPHA_SIZE, 8,
        EGL10.EGL_SURFACE_TYPE, EGL10.EGL_WINDOW_BIT | EGL10.EGL_PBUFFER_BIT, EGL10.EGL_NONE};
    EGLConfig[] configs = new EGLConfig[1];
    int[] count = new int[1];
    if (!egl.eglChooseConfig(display, attribs, configs, 1, count) || count[0] < 1) {
      return null;
    }
    return configs[0];
  }

  private static EGLContext context(EGL10 egl, EGLDisplay display, EGLConfig config, int es) {
    return egl.eglCreateContext(display, config, EGL10.EGL_NO_CONTEXT,
        new int[] {EGL_CONTEXT_CLIENT_VERSION, es, EGL10.EGL_NONE});
  }

  // --- a pbuffer: GLES20 shaders, glReadPixels, GLUtils ----------------------------------

  private static void pbuffer(EGL10 egl, EGLDisplay display, EGLConfig config) {
    EGLContext context = context(egl, display, config, 2);
    EGLSurface surface = egl.eglCreatePbufferSurface(display, config,
        new int[] {EGL10.EGL_WIDTH, W, EGL10.EGL_HEIGHT, H, EGL10.EGL_NONE});
    boolean current = context != EGL10.EGL_NO_CONTEXT && surface != EGL10.EGL_NO_SURFACE
        && egl.eglMakeCurrent(display, surface, surface, context);
    check(current, "EGL10: an ES 2 context current on a " + W + "x" + H + " pbuffer");
    if (!current) {
      return;
    }
    String renderer = GLES20.glGetString(GLES20.GL_RENDERER);
    String glVersion = GLES20.glGetString(GLES20.GL_VERSION);
    System.out.println("INFO  GL_RENDERER " + renderer + ", GL_VERSION " + glVersion);
    check(renderer != null && renderer.contains("ANGLE") && glVersion.startsWith("OpenGL ES"),
        "GLES20.glGetString: ANGLE's OpenGL ES");

    // Blue, with a red quad over the left half.
    GLES20.glViewport(0, 0, W, H);
    GLES20.glClearColor(0f, 0f, 1f, 1f);
    GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
    int program = program(COLOR_FRAGMENT);
    GLES20.glUseProgram(program);
    GLES20.glUniform4f(GLES20.glGetUniformLocation(program, "uColor"), 1f, 0f, 0f, 1f);
    quad(program, -1f, 0f);
    int[] f = readPixels(W, H);
    check(GLES20.glGetError() == GLES20.GL_NO_ERROR && f[at(10, 32)] == Color.RED
            && f[at(54, 32)] == Color.BLUE,
        "GLES20: shaders draw a red quad on blue, glReadPixels reads it ("
            + hex(f[at(10, 32)]) + ", " + hex(f[at(54, 32)]) + ")");
    GLES20.glDeleteProgram(program);

    // A 2x2 Bitmap (red, green / blue, white) through GLUtils.texImage2D, nearest filtering.
    Bitmap bitmap = Bitmap.createBitmap(2, 2, Bitmap.Config.ARGB_8888);
    bitmap.setPixel(0, 0, Color.RED);
    bitmap.setPixel(1, 0, Color.GREEN);
    bitmap.setPixel(0, 1, Color.BLUE);
    bitmap.setPixel(1, 1, Color.WHITE);
    int[] texture = new int[1];
    GLES20.glGenTextures(1, texture, 0);
    GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, texture[0]);
    GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_NEAREST);
    GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_NEAREST);
    GLUtils.texImage2D(GLES20.GL_TEXTURE_2D, 0, bitmap, 0);
    program = program(TEXTURE_FRAGMENT);
    GLES20.glUseProgram(program);
    GLES20.glUniform1i(GLES20.glGetUniformLocation(program, "uTex"), 0);
    quad(program, -1f, 1f);
    f = readPixels(W, H);
    // The bitmap's top row is the texture's first, at v = 0: the bottom of the
    // quad, which glReadPixels returns first.
    check(GLES20.glGetError() == GLES20.GL_NO_ERROR && f[at(10, 10)] == Color.RED
            && f[at(54, 10)] == Color.GREEN && f[at(10, 54)] == Color.BLUE
            && f[at(54, 54)] == Color.WHITE,
        "GLUtils.texImage2D: a Bitmap as a texture, drawn ("
            + hex(f[at(10, 10)]) + ", " + hex(f[at(54, 10)]) + ", " + hex(f[at(10, 54)]) + ", "
            + hex(f[at(54, 54)]) + ")");
    GLES20.glDeleteTextures(1, texture, 0);
    GLES20.glDeleteProgram(program);

    egl.eglMakeCurrent(display, EGL10.EGL_NO_SURFACE, EGL10.EGL_NO_SURFACE, EGL10.EGL_NO_CONTEXT);
    egl.eglDestroySurface(display, surface);
    egl.eglDestroyContext(display, context);
  }

  // --- GL10 on an ES 1 context ---------------------------------------------------------------

  private static void gl10(EGL10 egl, EGLDisplay display) {
    int[] attribs = {EGL10.EGL_RENDERABLE_TYPE, 1 /* EGL_OPENGL_ES_BIT */, EGL10.EGL_RED_SIZE, 8,
        EGL10.EGL_GREEN_SIZE, 8, EGL10.EGL_BLUE_SIZE, 8, EGL10.EGL_ALPHA_SIZE, 8,
        EGL10.EGL_SURFACE_TYPE, EGL10.EGL_PBUFFER_BIT, EGL10.EGL_NONE};
    EGLConfig[] configs = new EGLConfig[1];
    int[] count = new int[1];
    boolean chosen = egl.eglChooseConfig(display, attribs, configs, 1, count) && count[0] == 1;
    EGLContext context = chosen ? context(egl, display, configs[0], 1) : EGL10.EGL_NO_CONTEXT;
    EGLSurface surface = chosen ? egl.eglCreatePbufferSurface(display, configs[0],
        new int[] {EGL10.EGL_WIDTH, 8, EGL10.EGL_HEIGHT, 8, EGL10.EGL_NONE}) : EGL10.EGL_NO_SURFACE;
    boolean current = context != EGL10.EGL_NO_CONTEXT && surface != EGL10.EGL_NO_SURFACE
        && egl.eglMakeCurrent(display, surface, surface, context);
    check(current, "EGL10: an ES 1 context current on a pbuffer");
    if (!current) {
      return;
    }
    GL10 gl = (GL10) context.getGL();
    gl.glClearColor(0f, 1f, 0f, 1f);
    gl.glClear(GL10.GL_COLOR_BUFFER_BIT);
    ByteBuffer pixels = ByteBuffer.allocateDirect(8 * 8 * 4).order(ByteOrder.nativeOrder());
    gl.glReadPixels(0, 0, 8, 8, GL10.GL_RGBA, GL10.GL_UNSIGNED_BYTE, pixels);
    int color = Color.argb(pixels.get(3) & 0xff, pixels.get(0) & 0xff, pixels.get(1) & 0xff,
        pixels.get(2) & 0xff);
    check(gl.glGetError() == GL10.GL_NO_ERROR && color == Color.GREEN,
        "GL10 (GLImpl): glClear on ES 1, glReadPixels (" + hex(color) + ", "
            + gl.glGetString(GL10.GL_VERSION) + ")");
    egl.eglMakeCurrent(display, EGL10.EGL_NO_SURFACE, EGL10.EGL_NO_SURFACE, EGL10.EGL_NO_CONTEXT);
    egl.eglDestroySurface(display, surface);
    egl.eglDestroyContext(display, context);
  }

  // --- a window surface on a Win32 window --------------------------------------------------

  private static void window(EGL10 egl, EGLDisplay display, EGLConfig config) throws Exception {
    int ww = 160;
    int wh = 120;
    NativeWindow window = NativeWindow.create("Gesher GL bindings test", ww, wh);
    window.show();
    Surface surface = window.getSurface();
    EGLContext context = context(egl, display, config, 2);
    EGLSurface windowSurface = egl.eglCreateWindowSurface(display, config, surface, null);
    boolean current = windowSurface != EGL10.EGL_NO_SURFACE
        && egl.eglMakeCurrent(display, windowSurface, windowSurface, context);
    int[] size = new int[1];
    if (current) {
      egl.eglQuerySurface(display, windowSurface, EGL10.EGL_WIDTH, size);
    }
    check(current && size[0] == ww,
        "EGL10: a window surface on a Win32 window's Surface (" + size[0] + " wide), current");
    if (current) {
      // Green, with a red quad over the left half.
      GLES20.glViewport(0, 0, ww, wh);
      GLES20.glClearColor(0f, 1f, 0f, 1f);
      GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
      int program = program(COLOR_FRAGMENT);
      GLES20.glUseProgram(program);
      GLES20.glUniform4f(GLES20.glGetUniformLocation(program, "uColor"), 1f, 0f, 0f, 1f);
      quad(program, -1f, 0f);
      boolean swapped = egl.eglSwapBuffers(display, windowSurface);
      check(swapped && GLES20.glGetError() == GLES20.GL_NO_ERROR,
          "EGL10: eglSwapBuffers presents the frame");
      // DWM composes the swap chain's frame asynchronously.
      int[] shown = null;
      for (int attempt = 0; attempt < 30; attempt++) {
        shown = GraphicsDebug.captureComposed(window.getHwnd());
        if (shown != null && shown[20 * ww + 20] == Color.RED
            && shown[20 * ww + ww - 20] == Color.GREEN) {
          break;
        }
        Thread.sleep(100);
      }
      check(shown != null && shown[20 * ww + 20] == Color.RED
              && shown[20 * ww + ww - 20] == Color.GREEN,
          "the window shows the frame (PW_RENDERFULLCONTENT: "
              + (shown == null ? "none" : hex(shown[20 * ww + 20]) + ", "
                  + hex(shown[20 * ww + ww - 20])) + ")");
      GLES20.glDeleteProgram(program);
    }
    egl.eglMakeCurrent(display, EGL10.EGL_NO_SURFACE, EGL10.EGL_NO_SURFACE, EGL10.EGL_NO_CONTEXT);
    if (windowSurface != EGL10.EGL_NO_SURFACE) {
      egl.eglDestroySurface(display, windowSurface);
    }
    egl.eglDestroyContext(display, context);
    surface.release();
    window.close();
  }

  // --- window surfaces on a SurfaceTexture (a SurfaceView's surface) -------------------------

  private interface WindowSurfaceMaker {
    // Makes the window surface current and returns whether it is.
    boolean make(SurfaceTexture texture);

    void swapAndDestroy();
  }

  private static void surfaceTexture(EGL10 egl, EGLDisplay display, EGLConfig config)
      throws Exception {
    HandlerThread thread = new HandlerThread("GlBindingsTest frames");
    thread.start();
    Handler handler = new Handler(thread.getLooper());

    EGLContext context = context(egl, display, config, 2);
    EGLSurface[] made = new EGLSurface[1];
    Surface[] surfaces = new Surface[1];
    frames("EGL10: a window surface on a Surface over a SurfaceTexture (a SurfaceView's)", handler,
        new WindowSurfaceMaker() {
          @Override
          public boolean make(SurfaceTexture texture) {
            surfaces[0] = new Surface(texture);
            made[0] = egl.eglCreateWindowSurface(display, config, surfaces[0], null);
            return made[0] != EGL10.EGL_NO_SURFACE
                && egl.eglMakeCurrent(display, made[0], made[0], context);
          }

          @Override
          public void swapAndDestroy() {
            egl.eglSwapBuffers(display, made[0]);
            egl.eglMakeCurrent(display, EGL10.EGL_NO_SURFACE, EGL10.EGL_NO_SURFACE,
                EGL10.EGL_NO_CONTEXT);
            egl.eglDestroySurface(display, made[0]);
            surfaces[0].release();
          }
        });
    frames("EGL10: a window surface on a SurfaceTexture", handler, new WindowSurfaceMaker() {
      @Override
      public boolean make(SurfaceTexture texture) {
        made[0] = egl.eglCreateWindowSurface(display, config, texture, null);
        return made[0] != EGL10.EGL_NO_SURFACE
            && egl.eglMakeCurrent(display, made[0], made[0], context);
      }

      @Override
      public void swapAndDestroy() {
        egl.eglSwapBuffers(display, made[0]);
        egl.eglMakeCurrent(display, EGL10.EGL_NO_SURFACE, EGL10.EGL_NO_SURFACE,
            EGL10.EGL_NO_CONTEXT);
        egl.eglDestroySurface(display, made[0]);
      }
    });
    egl.eglDestroyContext(display, context);

    // EGL14, on the same display.
    android.opengl.EGLDisplay display14 = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
    int[] version = new int[2];
    EGL14.eglInitialize(display14, version, 0, version, 1);
    android.opengl.EGLConfig[] configs14 = new android.opengl.EGLConfig[1];
    int[] count = new int[1];
    EGL14.eglChooseConfig(display14, new int[] {EGL14.EGL_RENDERABLE_TYPE,
        EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8,
        EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8, EGL14.EGL_SURFACE_TYPE,
        EGL14.EGL_WINDOW_BIT, EGL14.EGL_NONE}, 0, configs14, 0, 1, count, 0);
    android.opengl.EGLContext context14 = count[0] == 1
        ? EGL14.eglCreateContext(display14, configs14[0], EGL14.EGL_NO_CONTEXT,
            new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0)
        : EGL14.EGL_NO_CONTEXT;
    android.opengl.EGLSurface[] made14 = new android.opengl.EGLSurface[1];
    frames("EGL14: a window surface on a SurfaceTexture", handler, new WindowSurfaceMaker() {
      @Override
      public boolean make(SurfaceTexture texture) {
        if (context14 == EGL14.EGL_NO_CONTEXT) {
          return false;
        }
        made14[0] = EGL14.eglCreateWindowSurface(display14, configs14[0], texture,
            new int[] {EGL14.EGL_NONE}, 0);
        return made14[0] != EGL14.EGL_NO_SURFACE
            && EGL14.eglMakeCurrent(display14, made14[0], made14[0], context14);
      }

      @Override
      public void swapAndDestroy() {
        EGL14.eglSwapBuffers(display14, made14[0]);
        EGL14.eglMakeCurrent(display14, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE,
            EGL14.EGL_NO_CONTEXT);
        EGL14.eglDestroySurface(display14, made14[0]);
      }
    });
    if (context14 != EGL14.EGL_NO_CONTEXT) {
      EGL14.eglDestroyContext(display14, context14);
    }
    thread.quitSafely();
  }

  // One frame through a window surface on a new SurfaceTexture: it must arrive.
  private static void frames(String what, Handler handler, WindowSurfaceMaker maker)
      throws Exception {
    SurfaceTexture texture = new SurfaceTexture(false);
    texture.setDefaultBufferSize(48, 32);
    CountDownLatch frame = new CountDownLatch(1);
    texture.setOnFrameAvailableListener(t -> frame.countDown(), handler);
    boolean current = maker.make(texture);
    check(current, what + ": current");
    if (current) {
      GLES20.glClearColor(1f, 0f, 1f, 1f);
      GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
      maker.swapAndDestroy();
      boolean arrived = frame.await(5, TimeUnit.SECONDS);
      texture.updateTexImage();
      check(arrived && texture.getTimestamp() != 0,
          what + ": eglSwapBuffers posts the frame to it (onFrameAvailable)");
    }
    texture.release();
  }

  // --- helpers ------------------------------------------------------------------------------

  private static final String VERTEX =
      "attribute vec2 aPos;\n"
      + "attribute vec2 aUv;\n"
      + "varying vec2 vUv;\n"
      + "void main() { gl_Position = vec4(aPos, 0.0, 1.0); vUv = aUv; }\n";
  private static final String COLOR_FRAGMENT =
      "precision mediump float;\n"
      + "uniform vec4 uColor;\n"
      + "void main() { gl_FragColor = uColor; }\n";
  private static final String TEXTURE_FRAGMENT =
      "precision mediump float;\n"
      + "varying vec2 vUv;\n"
      + "uniform sampler2D uTex;\n"
      + "void main() { gl_FragColor = texture2D(uTex, vUv); }\n";

  private static int shader(int type, String source) {
    int shader = GLES20.glCreateShader(type);
    GLES20.glShaderSource(shader, source);
    GLES20.glCompileShader(shader);
    int[] ok = new int[1];
    GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, ok, 0);
    check(ok[0] != 0, "GLES20: a shader compiles" + (ok[0] != 0 ? ""
        : " (" + GLES20.glGetShaderInfoLog(shader) + ")"));
    return shader;
  }

  private static int program(String fragment) {
    int program = GLES20.glCreateProgram();
    GLES20.glAttachShader(program, shader(GLES20.GL_VERTEX_SHADER, VERTEX));
    GLES20.glAttachShader(program, shader(GLES20.GL_FRAGMENT_SHADER, fragment));
    GLES20.glLinkProgram(program);
    int[] ok = new int[1];
    GLES20.glGetProgramiv(program, GLES20.GL_LINK_STATUS, ok, 0);
    check(ok[0] != 0, "GLES20: a program links" + (ok[0] != 0 ? ""
        : " (" + GLES20.glGetProgramInfoLog(program) + ")"));
    return program;
  }

  // A quad from x0 to x1 over the whole height, its texture coordinates 0..1.
  private static void quad(int program, float x0, float x1) {
    float[] v = {x0, -1f, 0f, 0f, x1, -1f, 1f, 0f, x0, 1f, 0f, 1f, x1, 1f, 1f, 1f};
    FloatBuffer buffer = ByteBuffer.allocateDirect(v.length * 4).order(ByteOrder.nativeOrder())
        .asFloatBuffer();
    buffer.put(v).position(0);
    int pos = GLES20.glGetAttribLocation(program, "aPos");
    int uv = GLES20.glGetAttribLocation(program, "aUv");
    GLES20.glVertexAttribPointer(pos, 2, GLES20.GL_FLOAT, false, 16, buffer);
    GLES20.glEnableVertexAttribArray(pos);
    if (uv >= 0) {
      buffer.position(2);
      GLES20.glVertexAttribPointer(uv, 2, GLES20.GL_FLOAT, false, 16, buffer);
      GLES20.glEnableVertexAttribArray(uv);
    }
    GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
    GLES20.glDisableVertexAttribArray(pos);
    if (uv >= 0) {
      GLES20.glDisableVertexAttribArray(uv);
    }
  }

  // ARGB colors, bottom-up rows as glReadPixels returns them.
  private static int[] readPixels(int w, int h) {
    ByteBuffer pixels = ByteBuffer.allocateDirect(w * h * 4).order(ByteOrder.nativeOrder());
    GLES20.glReadPixels(0, 0, w, h, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, pixels);
    int[] argb = new int[w * h];
    for (int i = 0; i < argb.length; i++) {
      argb[i] = Color.argb(pixels.get(i * 4 + 3) & 0xff, pixels.get(i * 4) & 0xff,
          pixels.get(i * 4 + 1) & 0xff, pixels.get(i * 4 + 2) & 0xff);
    }
    return argb;
  }

  private static int at(int x, int y) {
    return y * W + x;
  }

  private static String hex(int color) {
    return String.format("#%08x", color);
  }

  private static void finish() {
    if (failures == 0) {
      System.out.println("GlBindingsTest all passed");
    } else {
      System.out.println("GlBindingsTest: " + failures + " check(s) failed");
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
