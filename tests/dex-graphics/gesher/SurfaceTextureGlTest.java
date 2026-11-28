package gesher;

import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.SurfaceTexture;
import android.hardware.DataSpace;
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
 *   gesher.SurfaceTextureGlTest path/to/reader_buffer_test.so
 *   gesher.SurfaceTextureGlTest --android-dataspace path/to/reader_buffer_test.so
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
 *   <li>detachFromGLContext deletes the texture and prevents latching until reattachment;
 *       attachToGLContext in another context shows the latched frame in its new texture;
 *       attaching twice and detaching a detached one throw; releaseTexImage drops the frame
 *       (black, time stamp 0); release() makes updateTexImage throw;
 *   <li>Firefox's pattern: SurfaceTexture(0) detached at once with no context current,
 *       attached later on the rendering thread.
 * </ul>
 */
public final class SurfaceTextureGlTest {
  private static final int W = 32;
  private static final int H = 32;
  private static final int EXTERNAL = GLES11Ext.GL_TEXTURE_EXTERNAL_OES;

  // Tests only. The shared fixture is also used by ImageReaderPrivateTest.
  private static native int nativeTagProducer(Surface surface, int dataspace, int rgba, boolean post);

  private static int failures;
  private static EGLDisplay display;
  private static EGLConfig config;

  /** A context with a pbuffer of its own. */
  private static final class Gl implements AutoCloseable {
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

    @Override
    public void close() {
      EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE,
          EGL14.EGL_NO_CONTEXT);
      if (surface != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, surface);
      if (context != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(display, context);
    }
  }

  public static void main(String[] args) throws Exception {
    if (args.length != 0) {
      boolean onAndroid = args.length == 2 && "--android-dataspace".equals(args[0]);
      if (args.length != 1 && !onAndroid) throw new IllegalArgumentException("Invalid arguments");
      System.out.println("SurfaceTexture dataspace: loading the native producer");
      if (!onAndroid) System.loadLibrary("android_runtime");
      System.load(args[onAndroid ? 1 : 0]);
      runDataspace();
      finish();
      return;
    }

    System.loadLibrary("android_runtime");
    if (!initializeEgl()) {
      finish();
      return;
    }
    HandlerThread thread = new HandlerThread("SurfaceTextureGlTest");
    thread.start();
    Handler handler = new Handler(thread.getLooper());
    try (Gl a = new Gl(); Gl b = new Gl()) {
      check(a.makeCurrent() && b.makeCurrent(), "two ES 2 contexts on pbuffers");
      detachedAtConstruction();
      attachedAtConstruction(a, b, handler);
      detachedAtOnce(a, handler);
      singleBuffer(a, b, handler);
    } finally {
      EGL14.eglTerminate(display);
      thread.quitSafely();
    }
    finish();
  }

  // The standalone Android APK and both ART command-line paths run these same checks.
  static boolean runDataspace() {
    System.out.println("SurfaceTexture dataspace: initializing EGL");
    if (!initializeEgl()) return false;
    try (Gl a = new Gl(); Gl b = new Gl()) {
      boolean ready = a.makeCurrent() && b.makeCurrent() && a.makeCurrent();
      check(ready, "two ES 2 contexts on pbuffers");
      if (ready) taggedFrames(a, b);
    } finally {
      EGL14.eglTerminate(display);
      EGL14.eglReleaseThread();
    }
    return failures == 0;
  }

  private static boolean initializeEgl() {
    display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
    int[] version = new int[2];
    boolean initialized = display != EGL14.EGL_NO_DISPLAY
        && EGL14.eglInitialize(display, version, 0, version, 1);
    check(initialized, "EGL14: the default display initializes");
    if (!initialized) return false;
    EGLConfig[] configs = new EGLConfig[1];
    int[] count = new int[1];
    EGL14.eglChooseConfig(display, new int[] {EGL14.EGL_RENDERABLE_TYPE,
        EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8,
        EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8, EGL14.EGL_SURFACE_TYPE,
        EGL14.EGL_PBUFFER_BIT, EGL14.EGL_NONE}, 0, configs, 0, 1, count, 0);
    check(count[0] == 1, "EGL14: an RGBA8 ES 2 pbuffer config");
    if (count[0] != 1) {
      EGL14.eglTerminate(display);
      return false;
    }
    config = configs[0];
    return true;
  }

  private static void detachedAtConstruction() {
    SurfaceTexture texture = new SurfaceTexture(false);
    try {
      check(throwsIllegalState(texture::updateTexImage),
          "a detached SurfaceTexture with an empty queue cannot adopt the current context");
    } finally {
      texture.release();
    }
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

    long beforeDetach = st.getTimestamp();
    draw(producer, Color.CYAN, Color.CYAN);
    check(frames.tryAcquire(5, TimeUnit.SECONDS), "a Canvas frame can queue while detached");
    b.makeCurrent();
    check(throwsIllegalState(st::updateTexImage) && st.getTimestamp() == beforeDetach,
        "updateTexImage while detached leaves the current frame intact");
    int texB = newTexture();
    st.attachToGLContext(texB);
    f = sample(texB, m);
    check(f[at(W / 2, H / 2)] == Color.GREEN,
        "attachToGLContext in another context shows the latched frame in its texture ("
            + hex(f[at(W / 2, H / 2)]) + ")");
    check(throwsRuntime(() -> st.attachToGLContext(texB)),
        "attachToGLContext of an attached SurfaceTexture throws");

    st.updateTexImage();
    f = sample(texB, m);
    check(f[at(W / 2, H / 2)] == Color.CYAN && st.getTimestamp() > beforeDetach,
        "updateTexImage after reattachment latches the frame queued while detached");

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

  private static void taggedFrames(Gl a, Gl b) {
    int name = newTexture();
    SurfaceTexture texture = new SurfaceTexture(name);
    Surface producer = new Surface(texture);
    int textureName = name;
    Gl textureContext = a;
    try {
      texture.setDefaultBufferSize(W, H);
      check(texture.getDataSpace() == DataSpace.DATASPACE_UNKNOWN, "no latched frame has no dataspace");
      int status = nativeTagProducer(producer, DataSpace.DATASPACE_SRGB, 0xff0000ff, true);
      check(status == 0, "NDK producer posts red pixels tagged sRGB (" + status + ")");
      if (status != 0) return;
      check(nativeTagProducer(producer, DataSpace.DATASPACE_SRGB_LINEAR, 0, false) == 0,
          "NDK producer changes its setting without posting");
      check(texture.getDataSpace() == DataSpace.DATASPACE_UNKNOWN,
          "publication alone does not update SurfaceTexture dataspace");

      texture.updateTexImage();
      float[] matrix = new float[16];
      texture.getTransformMatrix(matrix);
      check(texture.getDataSpace() == DataSpace.DATASPACE_SRGB,
          "updateTexImage latches the posted sRGB tag (" + texture.getDataSpace() + ")");
      int pixel = sample(name, matrix)[at(W / 2, H / 2)];
      check(pixel == Color.RED, "the sRGB frame samples red (" + hex(pixel) + ")");

      status = nativeTagProducer(producer, DataSpace.DATASPACE_SRGB_LINEAR, 0xff00ff00, true);
      check(status == 0, "NDK producer posts green pixels tagged linear sRGB (" + status + ")");
      if (status != 0) return;
      if (!b.makeCurrent()) {
        check(false, "make the other EGL context current");
        return;
      }
      check(throwsIllegalState(texture::updateTexImage)
              && texture.getDataSpace() == DataSpace.DATASPACE_SRGB,
          "failed update in another context keeps the old dataspace");
      if (!a.makeCurrent()) {
        check(false, "return to the original EGL context");
        return;
      }
      texture.updateTexImage();
      texture.getTransformMatrix(matrix);
      check(texture.getDataSpace() == DataSpace.DATASPACE_SRGB_LINEAR,
          "the next latch updates the dataspace (" + texture.getDataSpace() + ")");
      pixel = sample(name, matrix)[at(W / 2, H / 2)];
      check(pixel == Color.GREEN, "the linear sRGB frame samples green (" + hex(pixel) + ")");
      texture.updateTexImage();
      check(texture.getDataSpace() == DataSpace.DATASPACE_SRGB_LINEAR,
          "update with no queued frame preserves the latched dataspace");

      texture.detachFromGLContext();
      textureName = 0;
      check(texture.getDataSpace() == DataSpace.DATASPACE_SRGB_LINEAR,
          "detach preserves the dataspace");
      if (!b.makeCurrent()) {
        check(false, "make the destination EGL context current");
        return;
      }
      textureContext = b;
      textureName = newTexture();
      texture.attachToGLContext(textureName);
      check(texture.getDataSpace() == DataSpace.DATASPACE_SRGB_LINEAR,
          "reattachment preserves the dataspace");
      texture.releaseTexImage();
      check(texture.getDataSpace() == DataSpace.DATASPACE_UNKNOWN,
          "releaseTexImage clears the dataspace with the frame");
    } finally {
      producer.release();
      texture.release();
      if (textureName != 0 && textureContext.makeCurrent()) {
        GLES20.glDeleteTextures(1, new int[] {textureName}, 0);
      }
    }
  }

  private static void singleBuffer(Gl a, Gl b, Handler handler) throws Exception {
    a.makeCurrent();
    int name = newTexture();
    SurfaceTexture texture = new SurfaceTexture(name, true);
    Surface producer = new Surface(texture);
    float[] matrix = new float[16];
    try {
      texture.setDefaultBufferSize(W, H);
      Semaphore frames = new Semaphore(0);
      texture.setOnFrameAvailableListener(ignored -> frames.release(), handler);
      draw(producer, Color.RED, Color.RED);
      check(frames.tryAcquire(5, TimeUnit.SECONDS), "single buffer queues one producer frame");
      texture.updateTexImage();
      texture.getTransformMatrix(matrix);
      check(sample(name, matrix)[at(W / 2, H / 2)] == Color.RED,
          "single buffer latches the actual Canvas pixels");

      b.makeCurrent();
      check(throwsIllegalState(texture::releaseTexImage),
          "wrong EGL context cannot return the single buffer");
      a.makeCurrent();
      check(throwsRuntime(() -> draw(producer, Color.BLUE, Color.BLUE)),
          "single-buffer Canvas producer is blocked before releaseTexImage");
      texture.updateTexImage();
      check(sample(name, matrix)[at(W / 2, H / 2)] == Color.RED,
          "failed producer lock leaves the latched image intact");

      texture.releaseTexImage();
      draw(producer, Color.GREEN, Color.GREEN);
      check(frames.tryAcquire(5, TimeUnit.SECONDS), "single buffer returns to the producer");
      texture.updateTexImage();
      texture.getTransformMatrix(matrix);
      check(sample(name, matrix)[at(W / 2, H / 2)] == Color.GREEN,
          "returned single buffer carries the next actual Canvas frame");
    } finally {
      producer.release();
      texture.release();
      GLES20.glDeleteTextures(1, new int[] {name}, 0);
    }
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
