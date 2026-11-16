/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.io.BufferedInputStream;
import java.io.BufferedReader;
import java.io.EOFException;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;

/**
 * File-backed image feed for {@link WindowsCamera}: reads an uncompressed 24/32-bit BMP
 * image prepared from any Windows image format by {@code gesher-app} / Control Panel (or
 * directly via {@code GESHER_CAMERA_IMAGE}), applies rotation and optional dark-border
 * framing (for check/document scanners), and encodes it into BT.601 studio-range NV12.
 *
 * <p>Uses pure Java decoding (zero HWUI/Skia native dependency) and is loaded reflectively
 * from {@link WindowsCamera} so {@code WindowsCamera} keeps only {@code android.util.Size}
 * as an Android compile dependency for standalone host tests.
 */
public final class CameraImageSource implements WindowsCamera.ImageFrameProvider {
    private static final String ENV_CAMERA_IMAGE = "GESHER_CAMERA_IMAGE";
    private static final String IMAGE_FILENAME = "camera-image.img";
    private static final String META_FILENAME = "camera-image.meta";
    private static final int BG_R = 24;
    private static final int BG_G = 24;
    private static final int BG_B = 24;

    private String cachedPath;
    private long cachedLength = -1;
    private long cachedMtime = Long.MIN_VALUE;
    private long cachedMetaMtime = Long.MIN_VALUE;
    private int cachedWidth;
    private int cachedHeight;
    private int cachedRotation = -1;
    private boolean cachedBorder = true;
    private int cachedZoom = 100;
    private byte[] cachedNv12;
    private int frameCounter;

    @Override
    public boolean hasActiveImage() {
        return resolveImageFile() != null;
    }

    @Override
    public synchronized boolean readNv12(byte[] nv12, int width, int height) {
        if (nv12 == null || width <= 0 || height <= 0) return false;
        int required = width * height * 3 / 2;
        if (nv12.length < required) return false;

        File imageFile = resolveImageFile();
        if (imageFile == null) {
            clearCache();
            return false;
        }

        Meta meta = readMeta();
        long length = imageFile.length();
        long mtime = imageFile.lastModified();
        long metaMtime = meta.mtime;
        String path = imageFile.getPath();

        if (cachedNv12 == null
                || !path.equals(cachedPath)
                || cachedLength != length
                || cachedMtime != mtime
                || cachedMetaMtime != metaMtime
                || cachedWidth != width
                || cachedHeight != height
                || cachedRotation != meta.rotation
                || cachedBorder != meta.border
                || cachedZoom != meta.zoom) {
            byte[] encoded = buildNv12(path, width, height, meta.rotation, meta.border, meta.zoom);
            if (encoded == null) {
                return false;
            }
            cachedPath = path;
            cachedLength = length;
            cachedMtime = mtime;
            cachedMetaMtime = metaMtime;
            cachedWidth = width;
            cachedHeight = height;
            cachedRotation = meta.rotation;
            cachedBorder = meta.border;
            cachedZoom = meta.zoom;
            cachedNv12 = encoded;
        }

        System.arraycopy(cachedNv12, 0, nv12, 0, required);
        // Subtle 1-LSB luma alternation in the top-left corner pixel keeps OCR/camera pipelines
        // receiving live differing frames without visible impact.
        int y0 = nv12[0] & 0xFF;
        nv12[0] = (byte) (y0 ^ (++frameCounter & 1));
        return true;
    }

    private void clearCache() {
        cachedPath = null;
        cachedLength = -1;
        cachedMtime = Long.MIN_VALUE;
        cachedMetaMtime = Long.MIN_VALUE;
        cachedNv12 = null;
    }

    private static File resolveImageFile() {
        String envPath = System.getenv(ENV_CAMERA_IMAGE);
        if (envPath != null && !envPath.isEmpty()) {
            File f = new File(envPath);
            return f.isFile() && f.length() > 0 ? f : null;
        }
        String testEnv = System.getenv("GESHER_CAMERA_TEST");
        if (testEnv != null && !testEnv.isEmpty() && !"0".equals(testEnv)) {
            return null;
        }
        String dataDir = System.getenv("ANDROID_DATA");
        if (dataDir == null || dataDir.isEmpty()) return null;
        File f = new File(dataDir, IMAGE_FILENAME);
        return f.isFile() && f.length() > 0 ? f : null;
    }

    private static final class Meta {
        long mtime = Long.MIN_VALUE;
        int rotation = 0;
        boolean border = true;
        int zoom = 100;
    }

    private static Meta readMeta() {
        Meta m = new Meta();
        String dataDir = System.getenv("ANDROID_DATA");
        if (dataDir == null || dataDir.isEmpty()) return m;
        File f = new File(dataDir, META_FILENAME);
        if (!f.isFile()) return m;
        m.mtime = f.lastModified();
        try (BufferedReader br = new BufferedReader(
                new InputStreamReader(new FileInputStream(f), StandardCharsets.UTF_8))) {
            String line;
            while ((line = br.readLine()) != null) {
                int eq = line.indexOf('=');
                if (eq <= 0) continue;
                String k = line.substring(0, eq).trim();
                String v = line.substring(eq + 1).trim();
                if ("rotation".equals(k)) {
                    try {
                        int r = Integer.parseInt(v) % 360;
                        if (r < 0) r += 360;
                        if (r == 0 || r == 90 || r == 180 || r == 270) {
                            m.rotation = r;
                        }
                    } catch (NumberFormatException ignored) {
                    }
                } else if ("border".equals(k)) {
                    m.border = !"0".equals(v) && !"false".equalsIgnoreCase(v);
                } else if ("zoom".equals(k)) {
                    try {
                        int z = Integer.parseInt(v);
                        if (z >= 100 && z <= 250) {
                            m.zoom = z;
                        }
                    } catch (NumberFormatException ignored) {
                    }
                }
            }
        } catch (IOException ignored) {
        }
        return m;
    }

    private static final class RgbImage {
        final int width;
        final int height;
        final int[] rgb;

        RgbImage(int width, int height, int[] rgb) {
            this.width = width;
            this.height = height;
            this.rgb = rgb;
        }
    }

    static byte[] buildNv12(String path, int targetW, int targetH, int extraRotation, boolean border) {
        return buildNv12(path, targetW, targetH, extraRotation, border, 100);
    }

    static byte[] buildNv12(String path, int targetW, int targetH, int extraRotation,
            boolean border, int zoomPercent) {
        RgbImage src = decodeBmp(path);
        if (src == null) return null;

        int clampedZoom = Math.max(100, Math.min(250, zoomPercent));
        float zoom = clampedZoom / 100.0f;
        boolean swap = (extraRotation == 90 || extraRotation == 270);
        int rotW = swap ? src.height : src.width;
        int rotH = swap ? src.width : src.height;

        float marginFrac = border ? 0.06f : 0f;
        float boxLeft = targetW * marginFrac;
        float boxTop = targetH * marginFrac;
        float boxRight = targetW - boxLeft;
        float boxBottom = targetH - boxTop;
        float boxW = boxRight - boxLeft;
        float boxH = boxBottom - boxTop;
        float scale = Math.min(boxW / rotW, boxH / rotH) * zoom;
        float invScale = 1.0f / scale;
        int sub = (scale < 0.72f) ? Math.min(4, Math.max(2, (int) Math.ceil(invScale))) : 1;

        int[] outRgb = new int[targetW * targetH];
        int bgColor = (BG_R << 16) | (BG_G << 8) | BG_B;
        float[] sxy = new float[2];
        int minDrawX = targetW;
        int maxDrawX = -1;
        int minDrawY = targetH;
        int maxDrawY = -1;

        for (int ty = 0; ty < targetH; ty++) {
            int rowOffset = ty * targetW;
            boolean inBoxY = (ty + 0.5f) >= boxTop && (ty + 0.5f) < boxBottom;
            float ryCenter = (ty + 0.5f - targetH * 0.5f) * invScale + rotH * 0.5f - 0.5f;
            boolean inSrcY = inBoxY && ryCenter >= -0.5f && ryCenter < rotH - 0.5f;
            if (!inSrcY) {
                for (int tx = 0; tx < targetW; tx++) outRgb[rowOffset + tx] = bgColor;
                continue;
            }
            for (int tx = 0; tx < targetW; tx++) {
                boolean inBoxX = (tx + 0.5f) >= boxLeft && (tx + 0.5f) < boxRight;
                float rxCenter = (tx + 0.5f - targetW * 0.5f) * invScale + rotW * 0.5f - 0.5f;
                if (!inBoxX || rxCenter < -0.5f || rxCenter >= rotW - 0.5f) {
                    outRgb[rowOffset + tx] = bgColor;
                    continue;
                }
                if (tx < minDrawX) minDrawX = tx;
                if (tx > maxDrawX) maxDrawX = tx;
                if (ty < minDrawY) minDrawY = ty;
                if (ty > maxDrawY) maxDrawY = ty;

                if (sub <= 1) {
                    mapRotatedToSource(rxCenter, ryCenter, src.width, src.height, extraRotation, sxy);
                    outRgb[rowOffset + tx] = sampleBilinear(src.rgb, src.width, src.height, sxy[0], sxy[1]);
                } else {
                    int rSum = 0;
                    int gSum = 0;
                    int bSum = 0;
                    int samples = sub * sub;
                    for (int syIdx = 0; syIdx < sub; syIdx++) {
                        float ry = (ty + (syIdx + 0.5f) / sub - targetH * 0.5f) * invScale + rotH * 0.5f - 0.5f;
                        for (int sxIdx = 0; sxIdx < sub; sxIdx++) {
                            float rx = (tx + (sxIdx + 0.5f) / sub - targetW * 0.5f) * invScale + rotW * 0.5f - 0.5f;
                            mapRotatedToSource(rx, ry, src.width, src.height, extraRotation, sxy);
                            int px = sampleBilinear(src.rgb, src.width, src.height, sxy[0], sxy[1]);
                            rSum += (px >> 16) & 0xFF;
                            gSum += (px >> 8) & 0xFF;
                            bSum += px & 0xFF;
                        }
                    }
                    outRgb[rowOffset + tx] = ((rSum / samples) << 16)
                            | ((gSum / samples) << 8)
                            | (bSum / samples);
                }
            }
        }

        if (maxDrawX - minDrawX >= 4 && maxDrawY - minDrawY >= 4) {
            sharpenRegion(outRgb, targetW, minDrawX + 1, minDrawY + 1, maxDrawX - 1, maxDrawY - 1);
        }

        byte[] nv12 = new byte[targetW * targetH * 3 / 2];
        argbToNv12(outRgb, targetW, targetH, nv12);
        return nv12;
    }

    private static void mapRotatedToSource(float rx, float ry, int srcW, int srcH,
            int extraRotation, float[] out) {
        switch (extraRotation) {
            case 90:
                out[0] = ry;
                out[1] = (srcH - 1) - rx;
                break;
            case 180:
                out[0] = (srcW - 1) - rx;
                out[1] = (srcH - 1) - ry;
                break;
            case 270:
                out[0] = (srcW - 1) - ry;
                out[1] = rx;
                break;
            default:
                out[0] = rx;
                out[1] = ry;
                break;
        }
    }

    /** Mild edge-enhancing unsharp filter so small CMC7 digits stay crisp at 1280x720. */
    private static void sharpenRegion(int[] rgb, int stride, int x0, int y0, int x1, int y1) {
        int[] src = rgb.clone();
        for (int y = y0; y <= y1; y++) {
            int row = y * stride;
            for (int x = x0; x <= x1; x++) {
                int i = row + x;
                int c = src[i];
                int n = src[i - stride];
                int s = src[i + stride];
                int w = src[i - 1];
                int e = src[i + 1];
                int r = sharpenChannel((c >> 16) & 0xFF,
                        ((n >> 16) & 0xFF) + ((s >> 16) & 0xFF) + ((w >> 16) & 0xFF) + ((e >> 16) & 0xFF));
                int g = sharpenChannel((c >> 8) & 0xFF,
                        ((n >> 8) & 0xFF) + ((s >> 8) & 0xFF) + ((w >> 8) & 0xFF) + ((e >> 8) & 0xFF));
                int b = sharpenChannel(c & 0xFF,
                        (n & 0xFF) + (s & 0xFF) + (w & 0xFF) + (e & 0xFF));
                rgb[i] = (r << 16) | (g << 8) | b;
            }
        }
    }

    private static int sharpenChannel(int center, int neighborSum) {
        int diff = center * 4 - neighborSum;
        if (Math.abs(diff) < 16) return center;
        return clamp(center + (diff * 35) / 400);
    }

    private static int sampleBilinear(int[] rgb, int w, int h, float fx, float fy) {
        if (fx < 0f) fx = 0f;
        if (fy < 0f) fy = 0f;
        int x0 = (int) fx;
        int y0 = (int) fy;
        if (x0 >= w - 1) x0 = w - 1;
        if (y0 >= h - 1) y0 = h - 1;
        int x1 = Math.min(x0 + 1, w - 1);
        int y1 = Math.min(y0 + 1, h - 1);
        float tx = fx - x0;
        float ty = fy - y0;
        if (tx < 0f) tx = 0f;
        if (ty < 0f) ty = 0f;

        int c00 = rgb[y0 * w + x0];
        int c10 = rgb[y0 * w + x1];
        int c01 = rgb[y1 * w + x0];
        int c11 = rgb[y1 * w + x1];

        int r = bilerp((c00 >> 16) & 0xFF, (c10 >> 16) & 0xFF, (c01 >> 16) & 0xFF, (c11 >> 16) & 0xFF, tx, ty);
        int g = bilerp((c00 >> 8) & 0xFF, (c10 >> 8) & 0xFF, (c01 >> 8) & 0xFF, (c11 >> 8) & 0xFF, tx, ty);
        int b = bilerp(c00 & 0xFF, c10 & 0xFF, c01 & 0xFF, c11 & 0xFF, tx, ty);
        return (r << 16) | (g << 8) | b;
    }

    private static int bilerp(int c00, int c10, int c01, int c11, float tx, float ty) {
        float top = c00 + (c10 - c00) * tx;
        float bot = c01 + (c11 - c01) * tx;
        int v = Math.round(top + (bot - top) * ty);
        return clamp(v);
    }

    private static RgbImage decodeBmp(String path) {
        try (InputStream in = new BufferedInputStream(new FileInputStream(path), 65536)) {
            byte[] header = new byte[54];
            readFully(in, header, 0, 54);
            if (header[0] != 'B' || header[1] != 'M') return null;

            int dataOffset = le32(header, 10);
            int dibSize = le32(header, 14);
            if (dibSize < 40 || dataOffset < 54) return null;

            int w = le32(header, 18);
            int rawH = le32(header, 22);
            int planes = le16(header, 26);
            int bpp = le16(header, 28);
            int compression = le32(header, 30);
            if (planes != 1 || w <= 0 || rawH == 0 || w > 8192 || Math.abs(rawH) > 8192) return null;
            if ((bpp != 24 && bpp != 32) || (compression != 0 && compression != 3)) return null;

            boolean topDown = rawH < 0;
            int h = Math.abs(rawH);
            int skip = dataOffset - 54;
            while (skip > 0) {
                long s = in.skip(skip);
                if (s <= 0) {
                    if (in.read() < 0) return null;
                    s = 1;
                }
                skip -= (int) s;
            }

            int bytesPerPixel = bpp / 8;
            int rowStride = ((w * bytesPerPixel) + 3) & ~3;
            byte[] rowBuf = new byte[rowStride];
            int[] pixels = new int[w * h];

            for (int y = 0; y < h; y++) {
                readFully(in, rowBuf, 0, rowStride);
                int dstY = topDown ? y : (h - 1 - y);
                int dstRow = dstY * w;
                for (int x = 0; x < w; x++) {
                    int off = x * bytesPerPixel;
                    int b = rowBuf[off] & 0xFF;
                    int g = rowBuf[off + 1] & 0xFF;
                    int r = rowBuf[off + 2] & 0xFF;
                    pixels[dstRow + x] = (r << 16) | (g << 8) | b;
                }
            }
            return new RgbImage(w, h, pixels);
        } catch (IOException e) {
            return null;
        }
    }

    private static void readFully(InputStream in, byte[] buf, int off, int len) throws IOException {
        int remaining = len;
        while (remaining > 0) {
            int n = in.read(buf, off, remaining);
            if (n < 0) throw new EOFException();
            off += n;
            remaining -= n;
        }
    }

    private static int le16(byte[] b, int off) {
        return (b[off] & 0xFF) | ((b[off + 1] & 0xFF) << 8);
    }

    private static int le32(byte[] b, int off) {
        return (b[off] & 0xFF)
                | ((b[off + 1] & 0xFF) << 8)
                | ((b[off + 2] & 0xFF) << 16)
                | ((b[off + 3] & 0xFF) << 24);
    }

    /** BT.601 studio-range RGB -> NV12 matching {@code Nv12ToRgba} in {@code camera_frames.cc}. */
    static void argbToNv12(int[] argb, int width, int height, byte[] nv12) {
        int yPlaneSize = width * height;
        for (int y = 0; y < height; y++) {
            int row = y * width;
            for (int x = 0; x < width; x++) {
                int px = argb[row + x];
                int r = (px >> 16) & 0xFF;
                int g = (px >> 8) & 0xFF;
                int b = px & 0xFF;
                int luma = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
                nv12[row + x] = (byte) clamp(luma);
            }
        }
        int cw = width / 2;
        int ch = height / 2;
        for (int cy = 0; cy < ch; cy++) {
            int r0 = (cy * 2) * width;
            int r1 = r0 + width;
            int uvRow = yPlaneSize + cy * width;
            for (int cx = 0; cx < cw; cx++) {
                int c0 = cx * 2;
                int p00 = argb[r0 + c0];
                int p01 = argb[r0 + c0 + 1];
                int p10 = argb[r1 + c0];
                int p11 = argb[r1 + c0 + 1];
                int r = (((p00 >> 16) & 0xFF) + ((p01 >> 16) & 0xFF)
                        + ((p10 >> 16) & 0xFF) + ((p11 >> 16) & 0xFF) + 2) >> 2;
                int g = (((p00 >> 8) & 0xFF) + ((p01 >> 8) & 0xFF)
                        + ((p10 >> 8) & 0xFF) + ((p11 >> 8) & 0xFF) + 2) >> 2;
                int b = ((p00 & 0xFF) + (p01 & 0xFF)
                        + (p10 & 0xFF) + (p11 & 0xFF) + 2) >> 2;
                int u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                int v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                nv12[uvRow + cx * 2] = (byte) clamp(u);
                nv12[uvRow + cx * 2 + 1] = (byte) clamp(v);
            }
        }
    }

    private static int clamp(int v) {
        return v < 0 ? 0 : (v > 255 ? 255 : v);
    }
}
