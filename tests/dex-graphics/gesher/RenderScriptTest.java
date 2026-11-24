package gesher;

import android.graphics.Bitmap;
import android.renderscript.Allocation;
import android.renderscript.Element;
import android.renderscript.Matrix4f;
import android.renderscript.RSRuntimeException;
import android.renderscript.RenderScript;
import android.renderscript.RenderScriptCacheDir;
import android.renderscript.ScriptC;
import android.renderscript.ScriptIntrinsic3DLUT;
import android.renderscript.ScriptIntrinsicBLAS;
import android.renderscript.ScriptIntrinsicBlend;
import android.renderscript.ScriptIntrinsicBlur;
import android.renderscript.ScriptIntrinsicColorMatrix;
import android.renderscript.ScriptIntrinsicConvolve3x3;
import android.renderscript.ScriptIntrinsicConvolve5x5;
import android.renderscript.ScriptIntrinsicHistogram;
import android.renderscript.ScriptIntrinsicLUT;
import android.renderscript.ScriptIntrinsicResize;
import android.renderscript.ScriptIntrinsicYuvToRGB;
import android.renderscript.Type;
import java.io.File;
import java.nio.ByteBuffer;
import java.util.Random;

/**
 * android.renderscript on Gesher's CPU runtime (docs/renderscript.md): librs_jni.so
 * loads, a context is created and destroyed, Elements, Types and Allocations
 * (from a Bitmap, typed, copied from and to arrays and Bitmaps), and the
 * intrinsics run on known inputs. Each result is compared with an independent
 * Java computation of the intrinsic's definition (cpu_ref's arithmetic: float
 * sums, truncation or +0.5 rounding, clamped edges); float results may differ by
 * one step of the last digit, so those comparisons allow 1 (2 for the
 * interpolating intrinsics). What Gesher does not have fails as Android fails:
 * ScriptC (no bitcode compiler: UnsupportedOperationException, "This device does
 * not have an ABI that supports ScriptC."), ScriptIntrinsicBLAS and USAGE_IO
 * allocations (RSRuntimeException).
 * Last, several threads create and use contexts at once.
 *
 *   gesher.RenderScriptTest [<cache dir>]
 */
public final class RenderScriptTest {
  private static int failures;

  public static void main(String[] args) throws Exception {
    // libandroid_runtime (SystemProperties, hwui's Bitmap), as gesher-app's Main loads it.
    System.loadLibrary("android_runtime");
    File cache = new File(args.length > 0 ? args[0] : System.getProperty("java.io.tmpdir"),
        "gesher-rs-test");
    cache.mkdirs();
    // ActivityThread.setupGraphicsSupport does this for an app.
    RenderScriptCacheDir.setupDiskCache(cache);

    RenderScript rs = RenderScript.create(null, 22);  // below 23: not a process context, so destroy() ends it
    check(rs != null, "RenderScript.create");
    try {
      blurBitmap(rs);
      blurU8(rs);
      arrays(rs);
      colorMatrix(rs);
      convolve3x3(rs);
      convolve5x5(rs);
      lut(rs);
      blend(rs);
      histogram(rs);
      lut3d(rs);
      resize(rs);
      yuvToRgb(rs);
      unsupported(rs);
      rs.finish();
    } finally {
      rs.destroy();
    }
    // A second context after the first is destroyed (the context thread and
    // the message thread are gone and come back).
    RenderScript rs2 = RenderScript.create(null, 22);
    try {
      blurU8(rs2);
    } finally {
      rs2.destroy();
    }

    concurrent();

    if (failures == 0) {
      System.out.println("RenderScriptTest all passed");
      // The process context's MessageThread runs for the life of the process,
      // as in an app: end the VM explicitly.
      System.exit(0);
    } else {
      System.out.println("RenderScriptTest: " + failures + " check(s) failed");
      System.exit(1);
    }
  }

  // --- Inputs --------------------------------------------------------------------------

  private static byte[] rgba(int w, int h, long seed, boolean opaque) {
    Random random = new Random(seed);
    byte[] p = new byte[w * h * 4];
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        int i = (y * w + x) * 4;
        // A blocky pattern with noise: edges for the filters to smooth.
        int block = ((x / 8) + (y / 8)) % 2 == 0 ? 200 : 40;
        p[i] = (byte) clamp(block + random.nextInt(41) - 20);
        p[i + 1] = (byte) clamp(x * 255 / Math.max(1, w - 1));
        p[i + 2] = (byte) clamp(y * 255 / Math.max(1, h - 1));
        p[i + 3] = opaque ? (byte) 255 : (byte) (64 + random.nextInt(192));
      }
    }
    return p;
  }

  private static int u(byte b) {
    return b & 0xff;
  }

  private static int clamp(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
  }

  private static int maxDiff(byte[] a, byte[] b) {
    if (a.length != b.length) return Integer.MAX_VALUE;
    int m = 0;
    for (int i = 0; i < a.length; i++) m = Math.max(m, Math.abs(u(a[i]) - u(b[i])));
    return m;
  }

  private static int maxDiffFromInput(byte[] a, byte[] b) {
    return maxDiff(a, b);
  }

  // --- Threads -----------------------------------------------------------------------

  /**
   * Contexts created and used on several threads at once, as apps do from the UI
   * thread and a background Handler (Leumi's BlurView and its screen capture):
   * each thread creates its own context and blurs Bitmaps repeatedly, while
   * another thread shares the process context of create(null, 23).
   */
  private static void concurrent() throws InterruptedException {
    final int threads = 4;
    final int rounds = 6;
    final int[] bad = new int[threads];
    final Throwable[] thrown = new Throwable[threads];
    Thread[] workers = new Thread[threads];
    for (int t = 0; t < threads; t++) {
      final int id = t;
      workers[t] = new Thread(() -> {
        try {
          // Even threads: a context of their own; odd: the shared process context.
          RenderScript rs = id % 2 == 0 ? RenderScript.create(null, 22) : RenderScript.create(null, 23);
          for (int r = 0; r < rounds; r++) {
            if (!blurOnce(rs, 48 + id * 8, 32 + r * 4, 100 + id * 10 + r)) bad[id]++;
          }
          if (id % 2 == 0) rs.destroy();
        } catch (Throwable e) {
          thrown[id] = e;
        }
      }, "rs-worker-" + t);
      workers[t].start();
    }
    for (Thread w : workers) w.join(120_000);
    for (int t = 0; t < threads; t++) {
      check(!workers[t].isAlive() && thrown[t] == null && bad[t] == 0,
          "RenderScript on thread " + t + (t % 2 == 0 ? " (own context)" : " (process context)")
          + ": " + rounds + " Bitmap blurs match the reference"
          + (thrown[t] != null ? " - threw " + thrown[t] : "")
          + (workers[t].isAlive() ? " - did not finish" : ""));
    }
  }

  private static boolean blurOnce(RenderScript rs, int w, int h, long seed) {
    float radius = 6f;
    byte[] pixels = rgba(w, h, seed, true);
    Bitmap bitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
    bitmap.copyPixelsFromBuffer(ByteBuffer.wrap(pixels));
    Allocation input = fromBitmap(rs, bitmap);
    Allocation output = Allocation.createTyped(rs, input.getType());
    ScriptIntrinsicBlur blur = ScriptIntrinsicBlur.create(rs, Element.U8_4(rs));
    blur.setRadius(radius);
    blur.setInput(input);
    blur.forEach(output);
    output.copyTo(bitmap);
    input.destroy();
    output.destroy();
    blur.destroy();
    ByteBuffer got = ByteBuffer.allocate(w * h * 4);
    bitmap.copyPixelsToBuffer(got);
    return maxDiff(got.array(), blurReference(pixels, w, h, 4, radius)) <= 1;
  }

  // --- Blur --------------------------------------------------------------------------

  /** cpu_ref's Gaussian weights (rsCpuIntrinsicBlur.cpp ComputeGaussianWeights). */
  private static float[] blurWeights(float radius) {
    float e = 2.718281828459045f;
    float pi = 3.1415926535897932f;
    float sigma = 0.4f * radius + 0.6f;
    float coeff1 = 1.0f / ((float) Math.sqrt(2.0f * pi) * sigma);
    float coeff2 = -1.0f / (2.0f * sigma * sigma);
    int iradius = (int) ((float) Math.ceil(radius) + 0.5f);
    float[] w = new float[2 * iradius + 1];
    float normalize = 0f;
    for (int r = -iradius; r <= iradius; r++) {
      w[r + iradius] = coeff1 * (float) Math.pow(e, (float) r * (float) r * coeff2);
      normalize += w[r + iradius];
    }
    normalize = 1.0f / normalize;
    for (int i = 0; i < w.length; i++) w[i] *= normalize;
    return w;
  }

  /** A separable Gaussian blur with clamped edges, truncated to bytes, as cpu_ref's. */
  private static byte[] blurReference(byte[] in, int w, int h, int channels, float radius) {
    float[] k = blurWeights(radius);
    int ir = k.length / 2;
    float[] vertical = new float[w * h * channels];
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        for (int c = 0; c < channels; c++) {
          float sum = 0f;
          for (int r = -ir; r <= ir; r++) {
            int yy = Math.min(Math.max(y + r, 0), h - 1);
            sum += (float) u(in[(yy * w + x) * channels + c]) * k[r + ir];
          }
          vertical[(y * w + x) * channels + c] = sum;
        }
      }
    }
    byte[] out = new byte[in.length];
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        for (int c = 0; c < channels; c++) {
          float sum = 0f;
          for (int r = -ir; r <= ir; r++) {
            int xx = Math.min(Math.max(x + r, 0), w - 1);
            sum += vertical[(y * w + xx) * channels + c] * k[r + ir];
          }
          out[(y * w + x) * channels + c] = (byte) (int) sum;
        }
      }
    }
    return out;
  }

  /**
   * Allocation.createFromBitmap(rs, bitmap) as an app targeting API 18+ gets it:
   * the one-argument form reads the target SDK from the app's Context, which this
   * test has none of.
   */
  private static Allocation fromBitmap(RenderScript rs, Bitmap bitmap) {
    return Allocation.createFromBitmap(rs, bitmap, Allocation.MipmapControl.MIPMAP_NONE,
        Allocation.USAGE_SHARED | Allocation.USAGE_SCRIPT | Allocation.USAGE_GRAPHICS_TEXTURE);
  }

  /** The common app pattern (Soundcore's BlurUtil): a Bitmap blurred into itself. */
  private static void blurBitmap(RenderScript rs) {
    int w = 96, h = 64;
    float radius = 8f;
    byte[] pixels = rgba(w, h, 1, true);
    Bitmap bitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
    bitmap.copyPixelsFromBuffer(ByteBuffer.wrap(pixels));

    Allocation input = fromBitmap(rs, bitmap);
    Allocation output = Allocation.createTyped(rs, input.getType());
    check(input.getType().getX() == w && input.getType().getY() == h
        && input.getElement().isCompatible(Element.U8_4(rs)), "Allocation.createFromBitmap: a U8_4 "
        + w + "x" + h + " Allocation");
    ScriptIntrinsicBlur blur = ScriptIntrinsicBlur.create(rs, Element.U8_4(rs));
    blur.setRadius(radius);
    blur.setInput(input);
    blur.forEach(output);
    output.copyTo(bitmap);
    input.destroy();
    output.destroy();
    blur.destroy();

    ByteBuffer got = ByteBuffer.allocate(w * h * 4);
    bitmap.copyPixelsToBuffer(got);
    byte[] expected = blurReference(pixels, w, h, 4, radius);
    int diff = maxDiff(got.array(), expected);
    check(diff <= 1, "ScriptIntrinsicBlur radius 8 on a " + w + "x" + h
        + " Bitmap matches the Gaussian reference (max difference " + diff + ")");
    check(maxDiffFromInput(got.array(), pixels) > 40, "ScriptIntrinsicBlur changed the image");
  }

  private static void blurU8(RenderScript rs) {
    int w = 40, h = 30;
    byte[] in = new byte[w * h];
    Random random = new Random(2);
    for (int i = 0; i < in.length; i++) in[i] = (byte) random.nextInt(256);
    Type t = Type.createXY(rs, Element.U8(rs), w, h);
    Allocation ain = Allocation.createTyped(rs, t);
    Allocation aout = Allocation.createTyped(rs, t);
    ain.copyFrom(in);
    ScriptIntrinsicBlur blur = ScriptIntrinsicBlur.create(rs, Element.U8(rs));
    blur.setRadius(2.5f);
    blur.setInput(ain);
    blur.forEach(aout);
    byte[] out = new byte[w * h];
    aout.copyTo(out);
    int diff = maxDiff(out, blurReference(in, w, h, 1, 2.5f));
    check(diff <= 1, "ScriptIntrinsicBlur U8 radius 2.5 (max difference " + diff + ")");
  }

  // --- Elements, Types, Allocations ----------------------------------------------------

  private static void arrays(RenderScript rs) {
    Type t = new Type.Builder(rs, Element.F32(rs)).setX(17).setY(3).create();
    check(t.getX() == 17 && t.getY() == 3 && t.getCount() == 51, "Type 17x3 F32");
    Allocation a = Allocation.createTyped(rs, t, Allocation.USAGE_SCRIPT | Allocation.USAGE_SHARED);
    float[] f = new float[51];
    for (int i = 0; i < f.length; i++) f[i] = i * 1.5f - 7f;
    a.copyFrom(f);
    float[] back = new float[51];
    a.copyTo(back);
    boolean same = true;
    for (int i = 0; i < f.length; i++) same &= f[i] == back[i];
    check(same, "Allocation F32: copyFrom(float[]) then copyTo(float[])");

    // An Allocation's initial contents are undefined (the driver does not clear
    // memory for elements without references): fill it first.
    Allocation ints = Allocation.createSized(rs, Element.I32(rs), 10);
    ints.copyFrom(new int[] {1, 1, 1, 1, 1, 1, 1, 1, 1, 1});
    ints.copy1DRangeFrom(2, 3, new int[] {7, -8, 9});
    int[] ib = new int[10];
    ints.copyTo(ib);
    check(ib[0] == 1 && ib[1] == 1 && ib[2] == 7 && ib[3] == -8 && ib[4] == 9 && ib[5] == 1
        && ib[9] == 1, "Allocation I32: copy1DRangeFrom into part of an Allocation");

    Allocation dst = Allocation.createTyped(rs, t);
    dst.copyFrom(a);
    float[] copied = new float[51];
    dst.copyTo(copied);
    check(copied[50] == f[50] && copied[0] == f[0], "Allocation.copyFrom(Allocation)");
    a.destroy();
    ints.destroy();
    dst.destroy();
  }

  // --- Color matrix, convolutions --------------------------------------------------------

  private static void colorMatrix(RenderScript rs) {
    int w = 23, h = 11;
    byte[] in = rgba(w, h, 3, false);
    Type t = Type.createXY(rs, Element.U8_4(rs), w, h);
    Allocation ain = Allocation.createTyped(rs, t);
    Allocation aout = Allocation.createTyped(rs, t);
    ain.copyFrom(in);
    ScriptIntrinsicColorMatrix cm = ScriptIntrinsicColorMatrix.create(rs);
    Matrix4f m = new Matrix4f(new float[] {
        0.30f, 0.10f, -0.20f, 0f,
        0.50f, 0.80f, 0.10f, 0f,
        0.20f, 0.10f, 1.10f, 0f,
        0f, 0f, 0f, 1f});
    cm.setColorMatrix(m);
    cm.setAdd(0.1f, 0f, -0.05f, 0f);
    cm.forEach(ain, aout);
    byte[] out = new byte[in.length];
    aout.copyTo(out);
    float[] c = m.getArray();
    float[] add = {0.1f * 255f, 0f, -0.05f * 255f, 0f};
    byte[] expected = new byte[in.length];
    for (int i = 0; i < in.length; i += 4) {
      float x = u(in[i]), y = u(in[i + 1]), z = u(in[i + 2]), wv = u(in[i + 3]);
      for (int k = 0; k < 4; k++) {
        float sum = x * c[k] + y * c[4 + k] + z * c[8 + k] + wv * c[12 + k];
        sum += add[k];
        sum = sum < 0 ? 0 : (sum > 255.5f ? 255.5f : sum);
        expected[i + k] = (byte) (int) sum;
      }
    }
    int diff = maxDiff(out, expected);
    check(diff <= 1, "ScriptIntrinsicColorMatrix with an add vector (max difference " + diff + ")");
  }

  private static byte[] convolveReference(byte[] in, int w, int h, float[] k, int size) {
    int r = size / 2;
    byte[] out = new byte[in.length];
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        for (int c = 0; c < 4; c++) {
          float sum = 0f;
          for (int j = -r; j <= r; j++) {
            for (int i = -r; i <= r; i++) {
              int xx = Math.min(Math.max(x + i, 0), w - 1);
              int yy = Math.min(Math.max(y + j, 0), h - 1);
              sum += (float) u(in[(yy * w + xx) * 4 + c]) * k[(j + r) * size + (i + r)];
            }
          }
          sum = Math.min(Math.max(sum + 0.5f, 0f), 255f);
          out[(y * w + x) * 4 + c] = (byte) (int) sum;
        }
      }
    }
    return out;
  }

  private static void convolve3x3(RenderScript rs) {
    int w = 31, h = 17;
    byte[] in = rgba(w, h, 4, false);
    Type t = Type.createXY(rs, Element.U8_4(rs), w, h);
    Allocation ain = Allocation.createTyped(rs, t);
    Allocation aout = Allocation.createTyped(rs, t);
    ain.copyFrom(in);
    float[] k = {0f, -1f, 0f, -1f, 5f, -1f, 0f, -1f, 0f};  // sharpen
    ScriptIntrinsicConvolve3x3 conv = ScriptIntrinsicConvolve3x3.create(rs, Element.U8_4(rs));
    conv.setCoefficients(k);
    conv.setInput(ain);
    conv.forEach(aout);
    byte[] out = new byte[in.length];
    aout.copyTo(out);
    int diff = maxDiff(out, convolveReference(in, w, h, k, 3));
    check(diff <= 1, "ScriptIntrinsicConvolve3x3 sharpen (max difference " + diff + ")");
  }

  private static void convolve5x5(RenderScript rs) {
    int w = 29, h = 19;
    byte[] in = rgba(w, h, 5, false);
    Type t = Type.createXY(rs, Element.U8_4(rs), w, h);
    Allocation ain = Allocation.createTyped(rs, t);
    Allocation aout = Allocation.createTyped(rs, t);
    ain.copyFrom(in);
    float[] k = new float[25];
    for (int i = 0; i < 25; i++) k[i] = ((i % 7) - 2) / 25f + (i == 12 ? 0.6f : 0f);
    ScriptIntrinsicConvolve5x5 conv = ScriptIntrinsicConvolve5x5.create(rs, Element.U8_4(rs));
    conv.setCoefficients(k);
    conv.setInput(ain);
    conv.forEach(aout);
    byte[] out = new byte[in.length];
    aout.copyTo(out);
    int diff = maxDiff(out, convolveReference(in, w, h, k, 5));
    check(diff <= 1, "ScriptIntrinsicConvolve5x5 (max difference " + diff + ")");
  }

  // --- LUT, blend, histogram ---------------------------------------------------------------

  private static void lut(RenderScript rs) {
    int w = 16, h = 16;
    byte[] in = rgba(w, h, 6, false);
    Type t = Type.createXY(rs, Element.U8_4(rs), w, h);
    Allocation ain = Allocation.createTyped(rs, t);
    Allocation aout = Allocation.createTyped(rs, t);
    ain.copyFrom(in);
    ScriptIntrinsicLUT lut = ScriptIntrinsicLUT.create(rs, Element.U8_4(rs));
    for (int i = 0; i < 256; i++) {
      lut.setRed(i, 255 - i);
      lut.setGreen(i, (i * i) / 255);
      lut.setBlue(i, i / 2);
      lut.setAlpha(i, i);
    }
    lut.forEach(ain, aout);
    byte[] out = new byte[in.length];
    aout.copyTo(out);
    byte[] expected = new byte[in.length];
    for (int i = 0; i < in.length; i += 4) {
      expected[i] = (byte) (255 - u(in[i]));
      expected[i + 1] = (byte) (u(in[i + 1]) * u(in[i + 1]) / 255);
      expected[i + 2] = (byte) (u(in[i + 2]) / 2);
      expected[i + 3] = in[i + 3];
    }
    check(maxDiff(out, expected) == 0, "ScriptIntrinsicLUT: per-channel tables, exact");
    lut.destroy();
  }

  private static void blend(RenderScript rs) {
    int w = 37, h = 9;
    byte[] src = rgba(w, h, 7, false);
    byte[] dst = rgba(w, h, 8, false);
    Type t = Type.createXY(rs, Element.U8_4(rs), w, h);
    Allocation ain = Allocation.createTyped(rs, t);
    Allocation aout = Allocation.createTyped(rs, t);
    ain.copyFrom(src);
    aout.copyFrom(dst);
    ScriptIntrinsicBlend blend = ScriptIntrinsicBlend.create(rs, Element.U8_4(rs));
    blend.forEachSrcOver(ain, aout);
    byte[] out = new byte[src.length];
    aout.copyTo(out);
    byte[] expected = new byte[src.length];
    for (int i = 0; i < src.length; i += 4) {
      int sa = u(src[i + 3]);
      for (int c = 0; c < 4; c++) {
        // in + ((out * (255 - in.a)) >> 8), clipped (rsCpuIntrinsicBlend.cpp).
        expected[i + c] = (byte) clamp(u(src[i + c]) + ((u(dst[i + c]) * (255 - sa)) >> 8));
      }
    }
    check(maxDiff(out, expected) == 0, "ScriptIntrinsicBlend source-over, exact");
  }

  private static void histogram(RenderScript rs) {
    int w = 50, h = 20;
    byte[] in = rgba(w, h, 9, false);
    Type t = Type.createXY(rs, Element.U8_4(rs), w, h);
    Allocation ain = Allocation.createTyped(rs, t);
    ain.copyFrom(in);
    Allocation hist = Allocation.createSized(rs, Element.U32_4(rs), 256);
    ScriptIntrinsicHistogram histogram = ScriptIntrinsicHistogram.create(rs, Element.U8_4(rs));
    histogram.setOutput(hist);
    histogram.forEach(ain);
    int[] counts = new int[256 * 4];
    hist.copyTo(counts);
    int[] expected = new int[256 * 4];
    for (int i = 0; i < in.length; i += 4) {
      for (int c = 0; c < 4; c++) expected[u(in[i + c]) * 4 + c]++;
    }
    boolean same = true;
    for (int i = 0; i < counts.length; i++) same &= counts[i] == expected[i];
    check(same, "ScriptIntrinsicHistogram: per-channel counts of " + (w * h) + " pixels, exact");
  }

  // --- 3D LUT, resize, YUV --------------------------------------------------------------------

  private static void lut3d(RenderScript rs) {
    int n = 17;
    Type cube = new Type.Builder(rs, Element.U8_4(rs)).setX(n).setY(n).setZ(n).create();
    Allocation lutAlloc = Allocation.createTyped(rs, cube);
    byte[] table = new byte[n * n * n * 4];
    for (int b = 0; b < n; b++) {
      for (int g = 0; g < n; g++) {
        for (int r = 0; r < n; r++) {
          int i = ((b * n + g) * n + r) * 4;
          // An inverting table: each grid point maps a colour to 255 minus it.
          table[i] = (byte) (255 - r * 255 / (n - 1));
          table[i + 1] = (byte) (255 - g * 255 / (n - 1));
          table[i + 2] = (byte) (255 - b * 255 / (n - 1));
          table[i + 3] = (byte) 255;
        }
      }
    }
    lutAlloc.copyFromUnchecked(table);
    int w = 32, h = 8;
    byte[] in = rgba(w, h, 10, true);
    Type t = Type.createXY(rs, Element.U8_4(rs), w, h);
    Allocation ain = Allocation.createTyped(rs, t);
    Allocation aout = Allocation.createTyped(rs, t);
    ain.copyFrom(in);
    ScriptIntrinsic3DLUT lut = ScriptIntrinsic3DLUT.create(rs, Element.U8_4(rs));
    lut.setLUT(lutAlloc);
    lut.forEach(ain, aout);
    byte[] out = new byte[in.length];
    aout.copyTo(out);
    // Trilinear interpolation of a linear table reproduces the linear map.
    int diff = 0;
    for (int i = 0; i < in.length; i += 4) {
      for (int c = 0; c < 3; c++) diff = Math.max(diff, Math.abs(u(out[i + c]) - (255 - u(in[i + c]))));
    }
    check(diff <= 3, "ScriptIntrinsic3DLUT: an inverting 17^3 table inverts colours (max difference "
        + diff + ")");
  }

  private static void resize(RenderScript rs) {
    int w = 64, h = 16, ow = 32, oh = 8;
    byte[] in = new byte[w * h * 4];
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        int i = (y * w + x) * 4;
        in[i] = (byte) (x * 4);         // a horizontal ramp
        in[i + 1] = (byte) 100;         // constant
        in[i + 2] = (byte) (y * 16);    // a vertical ramp
        in[i + 3] = (byte) 255;
      }
    }
    Allocation ain = Allocation.createTyped(rs, Type.createXY(rs, Element.U8_4(rs), w, h));
    Allocation aout = Allocation.createTyped(rs, Type.createXY(rs, Element.U8_4(rs), ow, oh));
    ain.copyFrom(in);
    ScriptIntrinsicResize resize = ScriptIntrinsicResize.create(rs);
    resize.setInput(ain);
    resize.forEach_bicubic(aout);
    byte[] out = new byte[ow * oh * 4];
    aout.copyTo(out);
    // Halving samples the source at 2x+0.5 (pixel centres): a ramp's value there.
    int diff = 0;
    for (int y = 0; y < oh; y++) {
      for (int x = 1; x < ow - 1; x++) {
        int i = (y * ow + x) * 4;
        diff = Math.max(diff, Math.abs(u(out[i]) - Math.round((2 * x + 0.5f) * 4)));
        diff = Math.max(diff, Math.abs(u(out[i + 1]) - 100));
        diff = Math.max(diff, Math.abs(u(out[i + 3]) - 255));
      }
    }
    check(diff <= 2, "ScriptIntrinsicResize bicubic 64x16 to 32x8 keeps a ramp and a constant (max difference "
        + diff + ")");
  }

  private static void yuvToRgb(RenderScript rs) {
    int w = 16, h = 8;
    byte[] nv21 = new byte[w * h * 3 / 2];
    Random random = new Random(11);
    for (int i = 0; i < nv21.length; i++) nv21[i] = (byte) random.nextInt(256);
    Allocation ain = Allocation.createSized(rs, Element.U8(rs), nv21.length);
    ain.copyFrom(nv21);
    Allocation aout = Allocation.createTyped(rs, Type.createXY(rs, Element.U8_4(rs), w, h));
    ScriptIntrinsicYuvToRGB yuv = ScriptIntrinsicYuvToRGB.create(rs, Element.U8_4(rs));
    yuv.setInput(ain);
    yuv.forEach(aout);
    byte[] out = new byte[w * h * 4];
    aout.copyTo(out);
    byte[] expected = new byte[w * h * 4];
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        int yy = u(nv21[y * w + x]) - 16;
        int c = w * h + (y >> 1) * w + (x >> 1) * 2;  // NV21: V then U
        int vv = u(nv21[c]) - 128;
        int uu = u(nv21[c + 1]) - 128;
        int i = (y * w + x) * 4;
        expected[i] = (byte) clamp((yy * 298 + vv * 409 + 128) >> 8);
        expected[i + 1] = (byte) clamp((yy * 298 - uu * 100 - vv * 208 + 128) >> 8);
        expected[i + 2] = (byte) clamp((yy * 298 + uu * 516 + 128) >> 8);
        expected[i + 3] = (byte) 255;
      }
    }
    check(maxDiff(out, expected) == 0, "ScriptIntrinsicYuvToRGB NV21, exact");
  }

  // --- What fails as on Android ------------------------------------------------------------

  private static void unsupported(RenderScript rs) {
    byte[] notCompiled = {'B', 'C', (byte) 0xc0, (byte) 0xde, 0, 0, 0, 0};
    // ScriptC checks for the platform's libRS.so (System.loadLibrary("RS")) and,
    // without it, throws as Android does on a device whose ABI has no ScriptC.
    try {
      new ScriptC(rs, "gesher_rs_test", notCompiled, notCompiled) {};
      check(false, "ScriptC without a bitcode compiler: created");
    } catch (UnsupportedOperationException e) {
      check("This device does not have an ABI that supports ScriptC.".equals(e.getMessage()),
          "ScriptC without a bitcode compiler: \"" + e.getMessage() + "\"");
    }
    try {
      ScriptIntrinsicBLAS.create(rs);
      check(false, "ScriptIntrinsicBLAS: created");
    } catch (RSRuntimeException e) {
      check(true, "ScriptIntrinsicBLAS: " + e.getMessage());
    }
    try {
      Allocation.createTyped(rs, Type.createXY(rs, Element.U8_4(rs), 4, 4),
          Allocation.USAGE_IO_INPUT | Allocation.USAGE_SCRIPT);
      check(false, "Allocation USAGE_IO_INPUT: created");
    } catch (RSRuntimeException e) {
      check(true, "Allocation USAGE_IO_INPUT: " + e.getMessage());
    }
    // The context still works after the failures.
    blurU8(rs);
  }

  private static void check(boolean ok, String what) {
    System.out.println((ok ? "PASS  " : "FAIL  ") + what);
    if (!ok) failures++;
  }
}
