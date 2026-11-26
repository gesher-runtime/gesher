/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.graphics.PixelFormat;
import android.media.Image;
import android.media.ImageReader;
import android.view.Surface;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.util.Arrays;

/** Explicit synthetic fixture for the real ART/JNI camera-to-ImageReader path. */
public final class CameraNv12RgbaTest {
    private static Method deliver;
    private static int checks;

    private static void require(boolean value, String reason) {
        ++checks;
        if (!value) throw new AssertionError(reason);
    }

    private static byte[] input(int width, int height, int seed) {
        byte[] bytes = new byte[width * height * 3 / 2];
        for (int n = 0; n < bytes.length; ++n) {
            seed ^= seed << 13;
            seed ^= seed >>> 17;
            seed ^= seed << 5;
            bytes[n] = (byte) seed;
        }
        return bytes;
    }

    private static int channel(long numerator) {
        return (int) Math.max(0L, Math.min(255L, Math.floorDiv(numerator, 256L)));
    }

    private static byte[] oracle(byte[] source, int width, int height, int tw, int th) {
        byte[] bytes = new byte[tw * th * 4];
        for (int y = 0; y < th; ++y) {
            int sy = (int) ((long) y * height / th);
            for (int x = 0; x < tw; ++x) {
                int sx = (int) ((long) x * width / tw);
                int uv = width * height + sy / 2 * width + sx / 2 * 2;
                long luma = (source[sy * width + sx] & 255) * 298L - 4768;
                int u = source[uv] & 255, v = source[uv + 1] & 255;
                int at = (y * tw + x) * 4;
                bytes[at] = (byte) channel(luma + v * 409L - 52224);
                bytes[at + 1] = (byte) channel(luma - u * 100L - v * 208L + 39552);
                bytes[at + 2] = (byte) channel(luma + u * 516L - 65920);
                bytes[at + 3] = (byte) 255;
            }
        }
        return bytes;
    }

    private static void push(Surface surface, byte[] source, int w, int h, long timestamp)
            throws Exception {
        int status = (Integer) deliver.invoke(null, surface, source, w, h, timestamp, null);
        require(status == 0, "real camera JNI delivery failed: " + status);
    }

    private static byte[] pixels(Image image) {
        Image.Plane[] planes = image.getPlanes();
        require(planes.length == 1, "RGBA has one plane");
        Image.Plane plane = planes[0];
        int width = image.getWidth(), height = image.getHeight();
        require(plane.getPixelStride() == 4 && plane.getRowStride() >= width * 4,
                "RGBA plane retains pixel and row stride");
        ByteBuffer buffer = plane.getBuffer();
        require(buffer.isDirect(), "RGBA plane is native direct storage");
        byte[] bytes = new byte[width * height * 4];
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width * 4; ++x) {
                bytes[(y * width * 4) + x] = buffer.get(y * plane.getRowStride() + x);
            }
        }
        return bytes;
    }

    private static void checkImage(ImageReader reader, Image image, byte[] expected,
            long timestamp) {
        require(image != null, "delivered image can be acquired");
        require(image.getWidth() == reader.getWidth() && image.getHeight() == reader.getHeight()
                && image.getFormat() == PixelFormat.RGBA_8888,
                "image dimensions and format remain exact");
        require(image.getTimestamp() == timestamp && image.getDataSpace() == reader.getDataSpace(),
                "image timestamp and dataspace remain exact");
        require(Arrays.equals(pixels(image), expected), "complete RGBA pixel oracle");
    }

    private static void colorAndScalingCases() throws Exception {
        int[][] cases = {{2, 2, 2, 2}, {2, 6, 2, 6}, {6, 2, 6, 2}, {16, 8, 16, 8},
                {258, 130, 258, 130}, {258, 130, 129, 65}, {16, 8, 17, 9},
                {16, 8, 16, 7}, {8, 4, 16, 8}, {16, 8, 1, 1}};
        for (int n = 0; n < cases.length; ++n) {
            int[] c = cases[n];
            byte[] source = input(c[0], c[1], 0x31415926 + n);
            byte[] saved = source.clone();
            byte[] expected = oracle(source, c[0], c[1], c[2], c[3]);
            try (ImageReader reader = ImageReader.newInstance(c[2], c[3], PixelFormat.RGBA_8888, 2)) {
                push(reader.getSurface(), source, c[0], c[1], n);
                try (Image image = reader.acquireNextImage()) {
                    checkImage(reader, image, expected, n);
                }
                require(Arrays.equals(source, saved), "JNI delivery must not change the NV12 producer array");
                require(reader.acquireNextImage() == null, "released single frame leaves no pending image");
            }
        }
        System.out.println("CameraNv12RgbaTest: real JNI matching/scaled pixels and metadata passed");
    }

    private static void independentOwnership() throws Exception {
        byte[] source = input(16, 8, 17);
        byte[] firstExpected = oracle(source, 16, 8, 16, 8);
        try (ImageReader reader = ImageReader.newInstance(16, 8, PixelFormat.RGBA_8888, 2)) {
            push(reader.getSurface(), source, 16, 8, 101);
            try (Image first = reader.acquireNextImage()) {
                checkImage(reader, first, firstExpected, 101);
                System.arraycopy(input(16, 8, 29), 0, source, 0, source.length);
                byte[] secondExpected = oracle(source, 16, 8, 16, 8);
                push(reader.getSurface(), source, 16, 8, 102);
                try (Image second = reader.acquireNextImage()) {
                    checkImage(reader, second, secondExpected, 102);
                    checkImage(reader, first, firstExpected, 101);
                    boolean rejected = false;
                    try (Image unexpected = reader.acquireNextImage()) {
                        require(unexpected == null, "no third acquired image may exist");
                    } catch (IllegalStateException expected) {
                        rejected = true;
                    }
                    require(rejected, "maxImages prevents an additional acquisition");
                    ByteBuffer writable = first.getPlanes()[0].getBuffer();
                    require(!writable.isReadOnly(), "native RGBA image storage remains independently writable");
                    firstExpected[0] ^= (byte) 0xff;
                    writable.put(0, firstExpected[0]);
                    checkImage(reader, second, secondExpected, 102);
                }
                push(reader.getSurface(), source, 16, 8, 103);
                try (Image third = reader.acquireNextImage()) {
                    checkImage(reader, third, secondExpected, 103);
                    checkImage(reader, first, firstExpected, 101);
                }
            }
        }
        // No public Image/ByteBuffer is inspected after ImageReader.close().
        try (ImageReader reader = ImageReader.newInstance(16, 8, PixelFormat.RGBA_8888, 2)) {
            for (int n = 1; n <= 5; ++n) push(reader.getSurface(), source, 16, 8, n);
            try (Image fourth = reader.acquireNextImage(); Image fifth = reader.acquireNextImage()) {
                byte[] expected = oracle(source, 16, 8, 16, 8);
                checkImage(reader, fourth, expected, 4);
                checkImage(reader, fifth, expected, 5);
            }
        }
        System.out.println("CameraNv12RgbaTest: held writable snapshots, bounded FIFO and maxImages passed");
    }

    public static void main(String[] args) {
        try {
            MediaTestBootstrap.initialize("nv12-rgba-contract");
            deliver = Class.forName("com.gesher.server.CameraFrames").getMethod("deliver",
                    Surface.class, byte[].class, int.class, int.class, long.class, byte[].class);
            colorAndScalingCases();
            independentOwnership();
            System.out.println("CameraNv12RgbaTest all passed: " + checks);
            System.exit(0);
        } catch (Throwable failure) {
            failure.printStackTrace();
            System.exit(1);
        }
    }
}
