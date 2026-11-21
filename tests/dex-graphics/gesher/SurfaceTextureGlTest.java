package gesher;

import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.SurfaceTexture;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES11Ext;
import android.opengl.GLES20;
import android.os.Handler;
import android.os.HandlerThread;
import android.view.Surface;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;

/**
 * android.graphics.SurfaceTexture consumed by OpenGL ES (docs/graphics.md, "SurfaceTexture in
 * OpenGL ES"; compat/media/surface/surface_texture_gl.cc): the frames a producer queues reach
 * the app's GL_TEXTURE_EXTERNAL_OES texture through updateTexImage, as a video decoder's reach
 * Firefox's compositor.
 *
 * <pre>
 *   gesher.SurfaceTextureGlTest
 * </pre>
 *
 * <ul>
 *   <li>SurfaceTexture(texName): the identity transform and no time stamp before a frame;
 *       updateTexImage without a current context throws IllegalStateException; a Canvas
 *       frame (red top half, blue bottom half) latched in the context current at the first
 *       updateTexImage is sampled through samplerExternalOES the right way up with the
 *       transform matrix (the vertical flip), with the frame's time stamp; a second frame
 *       replaces it; no new frame keeps it; another context current is
 *       IllegalStateException;
 *   <li>detachFromGLContext deletes the texture; attachToGLContext in another context shows
 *       the latched frame in its new texture; attaching twice and detaching a detached one
 *       throw; releaseTexImage drops the frame (black, time stamp 0); release() makes
 *       updateTexImage throw;
 *   <li>Firefox's pattern: SurfaceTexture(0) detached at once with no context current,
 *       attached later on the rendering thread.
 * </ul>
 */
public final class SurfaceTextureGlTest {
  private static final int W = 32;
  private static final int H = 32;
  private static final int EXTERNAL = GLES11Ext.GL_TEXTURE_EXTERNAL_OES;

  private static int failures;
  private static EGLDisplay display;
  private static EGLConfig config;

  /** A context with a pbuffer of its own. */
  private static final class Gl {
    final EGLContext context;
    final EGLSurface surface;

    Gl() {
      context = EGL14.eglCreateContext(display, config, EGL14.EGL_NO_CONTEXT,
          new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
      surface = EGL14.eglCreatePbufferSurface(display, config,
          new int[] {EGL14.EGL_WIDTH, W, EGL14.EGL_HEIGHT, H, EGL14.EGL_NONE}, 0);
    }

    boolean makeCurrent() {
      return context != EGL14.EGL_NO_CONTEXT && surface != EGL14.EGL_NO_SURFACE
          && EGL14.eglMakeCurrent(display, surface, surface, context);
    }

    void destroy() {
      EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE,
          EGL14.EGL_NO_CONTEXT);
      EGL14.eglDestroySurface(display, surface);
      EGL14.eglDestroyContext(display, context);
    }
  }

  public static void main(String[] args) throws Exception {
    System.loadLibrary("android_runtime");
    display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
    int[] version = new int[2];
    check(display != EGL14.EGL_NO_DISPLAY && EGL14.eglInitialize(display, version, 0, version, 1),
        "EGL14: the default display initializes");
    EGLConfig[] configs = new EGLConfig[1];
    int[] count = new int[1];
    EGL14.eglChooseConfig(display, new int[] {EGL14.EGL_RENDERABLE_TYPE,
        EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8,
        EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8, EGL14.EGL_SURFACE_TYPE,
        EGL14.EGL_PBUFFER_BIT, EGL14.EGL_NONE}, 0, configs, 0, 1, count, 0);
    check(count[0] == 1, "EGL14: an RGBA8 ES 2 pbuffer config");
    if (count[0] != 1) {
      finish();
      return;
    }
    config = configs[0];
    HandlerThread thread = new HandlerThread("SurfaceTextureGlTest");
    thread.start();
    Handler handler = new Handler(thread.getLooper());
    Gl a = new Gl();
    Gl b = new Gl();
    check(a.makeCurrent() && b.makeCurrent(), "two ES 2 contexts on pbuffers");
    attachedAtConstruction(a, b, handler);
    detachedAtOnce(a, handler);
    a.destroy();
    b.destroy();
    EGL14.eglTerminate(display);
    thread.quitSafely();
    finish();
  }

  private static void attachedAtConstruction(Gl a, Gl b, Handler handler) throws Exception {
    a.makeCurrent();
    int texA = newTexture();
    SurfaceTexture st = new SurfaceTexture(texA);
    st.setDefaultBufferSize(W, H);
    Semaphore frames = new Semaphore(0);
    st.setOnFrameAvailableListener(t -> frames.release(), handler);
    float[] m = new float[16];
    st.getTransformMatrix(m);
    check(isIdentity(m) && st.getTimestamp() == 0,
        "before a frame: the identity transform and no time stamp");

    release();
    check(throwsIllegalState(st::updateTexImage),
        "updateTexImage without a current context throws IllegalStateException");

    Surface producer = new Surface(st);
    draw(producer, Color.RED, Color.BLUE);
    check(frames.tryAcquire(5, TimeUnit.SECONDS), "a Canvas frame reaches onFrameAvailable");
    a.makeCurrent();
    st.updateTexImage();
    long first = st.getTimestamp();
    st.getTransformMatrix(m);
    check(first != 0 && m[0] == 1f && m[5] == -1f && m[13] == 1f,
        "updateTexImage latches the frame: its time stamp (" + first + "), the vertical flip");
    int[] f = sample(texA, m);
    check(f[at(W / 2, H - 3)] == Color.RED && f[at(W / 2, 2)] == Color.BLUE,
        "samplerExternalOES shows the frame the right way up (top " + hex(f[at(W / 2, H - 3)])
            + ", bottom " + hex(f[at(W / 2, 2)]) + ")");

    draw(producer, Color.GREEN, Color.GREEN);
    check(frames.tryAcquire(5, TimeUnit.SECONDS), "a second frame reaches onFrameAvailable");
    st.updateTexImage();
    f = sample(texA, m);
    check(f[at(W / 2, H / 2)] == Color.GREEN && st.getTimestamp() > first,
        "the next updateTexImage latches the second frame (" + hex(f[at(W / 2, H / 2)]) + ")");
    st.updateTexImage();
    f = sample(texA, m);
    check(f[at(W / 2, H / 2)] == Color.GREEN, "with no new frame the texture keeps the latched one");

    b.makeCurrent();
    check(throwsIllegalState(st::updateTexImage),
        "updateTexImage with another context current throws IllegalStateException");

    a.makeCurrent();
    st.detachFromGLContext();
    check(!GLES20.glIsTexture(texA), "detachFromGLContext deletes the texture");
    check(throwsRuntime(st::detachFromGLContext),
        "detachFromGLContext of a detached SurfaceTexture throws");

    b.makeCurrent();
    int texB = newTexture();
    st.attachToGLContext(texB);
    f = sample(texB, m);
    check(f[at(W / 2, H / 2)] == Color.GREEN,
        "attachToGLContext in another context shows the latched frame in its texture ("
            + hex(f[at(W / 2, H / 2)]) + ")");
    check(throwsRuntime(() -> st.attachToGLContext(texB)),
        "attachToGLContext of an attached SurfaceTexture throws");

    st.releaseTexImage();
    f = sample(texB, m);
    check(st.getTimestamp() == 0 && f[at(W / 2, H / 2)] == Color.BLACK,
        "releaseTexImage drops the frame (" + hex(f[at(W / 2, H / 2)]) + ", time stamp "
            + st.getTimestamp() + ")");
    draw(producer, Color.WHITE, Color.WHITE);
    check(frames.tryAcquire(5, TimeUnit.SECONDS), "a frame after releaseTexImage arrives");
    st.updateTexImage();
    f = sample(texB, m);
    check(f[at(W / 2, H / 2)] == Color.WHITE, "and is latched (" + hex(f[at(W / 2, H / 2)]) + ")");

    producer.release();
    st.release();
    check(st.isReleased() && throwsRuntime(st::updateTexImage),
        "after release() updateTexImage throws");
    GLES20.glDeleteTextures(1, new int[] {texB}, 0);
  }

  // GeckoSurfaceTexture: new SurfaceTexture(0, singleBufferMode) and detachFromGLContext()
  // on a binder thread, attachToGLContext(texName) later on the compositor's.
  private static void detachedAtOnce(Gl a, Handler handler) throws Exception {
    release();
    SurfaceTexture st = new SurfaceTexture(0, false);
    boolean detached = true;
    try {
      st.detachFromGLContext();
    } catch (RuntimeException e) {
      detached = false;
    }
    check(detached, "SurfaceTexture(0) detaches with no context current");
    st.setDefaultBufferSize(W, H);
    Semaphore frames = new Semaphore(0);
    st.setOnFrameAvailableListener(t -> frames.release(), handler);
    Surface producer = new Surface(st);
    draw(producer, Color.MAGENTA, Color.MAGENTA);
    check(frames.tryAcquire(5, TimeUnit.SECONDS), "its producer's frame arrives");
    a.makeCurrent();
    int tex = newTexture();
    st.attachToGLContext(tex);
    st.updateTexImage();
    float[] m = new float[16];
    st.getTransformMatrix(m);
    int[] f = sample(tex, m);
    check(f[at(W / 2, H / 2)] == Color.MAGENTA,
        "attached later on the rendering thread, updateTexImage latches it ("
            + hex(f[at(W / 2, H / 2)]) + ")");
    producer.release();
    st.release();
    GLES20.glDeleteTextures(1, new int[] {tex}, 0);
  }

  // --- helpers ------------------------------------------------------------------------------

  private interface Call {
    void run();
  }

  private static boolean throwsIllegalState(Call call) {
    try {
      call.run();
      return false;
    } catch (IllegalStateException e) {
      return true;
    }
  }

  private static boolean throwsRuntime(Call call) {
    try {
      call.run();
      return false;
    } catch (RuntimeException e) {
      return true;
    }
  }

  private static void release() {
    EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE,
        EGL14.EGL_NO_CONTEXT);
  }

  private static int newTexture() {
    int[] t = new int[1];
    GLES20.glGenTextures(1, t, 0);
    return t[0];
  }

  // A Canvas frame: `top` over the upper half, `bottom` over the lower one.
  private static void draw(Surface surface, int top, int bottom) {
    Canvas c = surface.lockCanvas(null);
    Paint p = new Paint();
    p.setColor(top);
    c.drawRect(0, 0, c.getWidth(), c.getHeight() / 2f, p);
    p.setColor(bottom);
    c.drawRect(0, c.getHeight() / 2f, c.getWidth(), c.getHeight(), p);
    surface.unlockCanvasAndPost(c);
  }

  private static final String VERTEX =
      "attribute vec2 aPos;\n"
      + "attribute vec2 aUv;\n"
      + "uniform mat4 uTx;\n"
      + "varying vec2 vUv;\n"
      + "void main() {\n"
      + "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
      + "  vUv = (uTx * vec4(aUv, 0.0, 1.0)).xy;\n"
      + "}\n";
  private static final String FRAGMENT =
      "#extension GL_OES_EGL_image_external : require\n"
      + "precision mediump float;\n"
      + "varying vec2 vUv;\n"
      + "uniform samplerExternalOES uTex;\n"
      + "void main() { gl_FragColor = texture2D(uTex, vUv); }\n";

  // The external texture drawn over the current pbuffer with the transform; its pixels (ARGB,
  // bottom-up rows as glReadPixels returns them).
  private static int[] sample(int texture, float[] transform) {
    int program = GLES20.glCreateProgram();
    int vs = shader(GLES20.GL_VERTEX_SHADER, VERTEX);
    int fs = shader(GLES20.GL_FRAGMENT_SHADER, FRAGMENT);
    GLES20.glAttachShader(program, vs);
    GLES20.glAttachShader(program, fs);
    GLES20.glLinkProgram(program);
    int[] ok = new int[1];
    GLES20.glGetProgramiv(program, GLES20.GL_LINK_STATUS, ok, 0);
    if (ok[0] == 0) {
      check(false, "the samplerExternalOES program links (" + GLES20.glGetProgramInfoLog(program)
          + ")");
    }
    GLES20.glViewport(0, 0, W, H);
    GLES20.glClearColor(0.5f, 0.5f, 0.5f, 1f);
    GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
    GLES20.glUseProgram(program);
    GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
    GLES20.glBindTexture(EXTERNAL, texture);
    GLES20.glUniform1i(GLES20.glGetUniformLocation(program, "uTex"), 0);
    GLES20.glUniformMatrix4fv(GLES20.glGetUniformLocation(program, "uTx"), 1, false, transform, 0);
    float[] v = {-1f, -1f, 0f, 0f, 1f, -1f, 1f, 0f, -1f, 1f, 0f, 1f, 1f, 1f, 1f, 1f};
    FloatBuffer buffer = ByteBuffer.allocateDirect(v.length * 4).order(ByteOrder.nativeOrder())
        .asFloatBuffer();
    buffer.put(v).position(0);
    int pos = GLES20.glGetAttribLocation(program, "aPos");
    int uv = GLES20.glGetAttribLocation(program, "aUv");
    GLES20.glVertexAttribPointer(pos, 2, GLES20.GL_FLOAT, false, 16, buffer);
    GLES20.glEnableVertexAttribArray(pos);
    buffer.position(2);
    GLES20.glVertexAttribPointer(uv, 2, GLES20.GL_FLOAT, false, 16, buffer);
    GLES20.glEnableVertexAttribArray(uv);
    GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
    GLES20.glDisableVertexAttribArray(pos);
    GLES20.glDisableVertexAttribArray(uv);
    ByteBuffer pixels = ByteBuffer.allocateDirect(W * H * 4).order(ByteOrder.nativeOrder());
    GLES20.glReadPixels(0, 0, W, H, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, pixels);
    GLES20.glUseProgram(0);
    GLES20.glDeleteProgram(program);
    GLES20.glDeleteShader(vs);
    GLES20.glDeleteShader(fs);
    int[] argb = new int[W * H];
    for (int i = 0; i < argb.length; i++) {
      argb[i] = Color.argb(pixels.get(i * 4 + 3) & 0xff, pixels.get(i * 4) & 0xff,
          pixels.get(i * 4 + 1) & 0xff, pixels.get(i * 4 + 2) & 0xff);
    }
    return argb;
  }

  private static int shader(int type, String source) {
    int shader = GLES20.glCreateShader(type);
    GLES20.glShaderSource(shader, source);
    GLES20.glCompileShader(shader);
    int[] ok = new int[1];
    GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, ok, 0);
    if (ok[0] == 0) {
      check(false, "a shader compiles (" + GLES20.glGetShaderInfoLog(shader) + ")");
    }
    return shader;
  }

  private static boolean isIdentity(float[] m) {
    for (int i = 0; i < 16; i++) {
      if (m[i] != (i % 5 == 0 ? 1f : 0f)) {
        return false;
      }
    }
    return true;
  }

  private static int at(int x, int y) {
    return y * W + x;
  }

  private static String hex(int color) {
    return String.format("#%08x", color);
  }

  private static void finish() {
    if (failures == 0) {
      System.out.println("SurfaceTextureGlTest all passed");
    } else {
      System.out.println("SurfaceTextureGlTest: " + failures + " check(s) failed");
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
