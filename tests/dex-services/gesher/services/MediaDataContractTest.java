/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.ImageFormat;
import android.graphics.Rect;
import android.graphics.YuvImage;
import android.media.MediaExtractor;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.util.Arrays;

/** Public media calls: extractor source ownership and right/bottom YUV crops. */
public final class MediaDataContractTest {
    private static int checks;

    private static void check(boolean condition, String label) {
        if (!condition) throw new AssertionError(label);
        checks++;
    }

    private interface Operation { void run() throws Exception; }

    private static void requiresIOException(Operation operation, String label) throws Exception {
        try { operation.run(); }
        catch (IOException expected) { checks++; return; }
        throw new AssertionError(label);
    }

    private static byte[] wav(byte[] pcm) {
        ByteBuffer result = ByteBuffer.allocate(44 + pcm.length).order(ByteOrder.LITTLE_ENDIAN);
        result.put(new byte[] {'R', 'I', 'F', 'F'}).putInt(36 + pcm.length);
        result.put(new byte[] {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '}).putInt(16);
        result.putShort((short) 1).putShort((short) 1).putInt(8000).putInt(16000);
        result.putShort((short) 2).putShort((short) 16);
        result.put(new byte[] {'d', 'a', 't', 'a'}).putInt(pcm.length).put(pcm);
        return result.array();
    }

    private static byte[] sample(MediaExtractor extractor) {
        ByteBuffer buffer = ByteBuffer.allocateDirect(65536);
        int size = extractor.readSampleData(buffer, 0);
        if (size < 0) return null;
        byte[] result = new byte[size];
        buffer.get(result);
        return result;
    }

    private static void extractor() throws Exception {
        File source = File.createTempFile("gesher-extractor-contract-", ".wav");
        File absent = new File(source.getParentFile(), source.getName() + ".absent");
        byte[] pcm = new byte[16000];
        for (int i = 0; i < pcm.length; i++) pcm[i] = (byte) (i * 17 + 3);
        Files.write(source.toPath(), wav(pcm));
        MediaExtractor extractor = new MediaExtractor();
        try {
            extractor.setDataSource(source.getAbsolutePath());
            check(extractor.getTrackCount() == 1, "WAV has one track");
            extractor.selectTrack(0);
            byte[] first = sample(extractor);
            check(first != null && first.length > 0, "first track sample is readable");
            long time = extractor.getSampleTime();

            requiresIOException(() -> extractor.setDataSource(source.getAbsolutePath()),
                    "second valid path must not replace the data source");
            requiresIOException(() -> extractor.setDataSource(absent.getAbsolutePath()),
                    "second missing path must not reset the first data source");
            try (FileInputStream input = new FileInputStream(source)) {
                requiresIOException(() -> extractor.setDataSource(input.getFD(), 0, source.length()),
                        "second file descriptor must not replace the data source");
                check(input.read() == 'R', "rejected setup leaves caller descriptor usable");
            }
            check(extractor.getSampleTime() == time, "second setup preserves sample timestamp");
            check(Arrays.equals(first, sample(extractor)), "second setup preserves selected sample");
            ByteArrayOutputStream decoded = new ByteArrayOutputStream();
            for (;;) {
                byte[] block = sample(extractor);
                if (block == null) break;
                decoded.write(block);
                if (!extractor.advance()) break;
            }
            check(Arrays.equals(pcm, decoded.toByteArray()), "original selected track reads to EOF");
        } finally {
            extractor.release();
            extractor.release();
        }

        MediaExtractor retry = new MediaExtractor();
        try {
            requiresIOException(() -> retry.setDataSource(absent.getAbsolutePath()),
                    "missing first source reports IOException");
            retry.setDataSource(source.getAbsolutePath());
            check(retry.getTrackCount() == 1, "first setup failure permits a successful retry");
            retry.selectTrack(0);
            check(sample(retry) != null, "retry track is readable");
        } finally {
            retry.release();
            check(source.delete(), "extractor releases its file descriptor");
        }
    }

    private static byte[] image(int format, int width, int height, int stride) {
        byte[] data = new byte[format == ImageFormat.NV21
                ? stride * height * 3 / 2 : stride * height];
        if (format == ImageFormat.NV21) {
            Arrays.fill(data, (byte) 128);
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) data[y * stride + x] = (byte) (x < width / 2 ? 64 : 192);
            }
        } else {
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x += 2) {
                    int offset = y * stride + x * 2;
                    data[offset] = data[offset + 2] = (byte) (x < width / 2 ? 64 : 192);
                    data[offset + 1] = data[offset + 3] = (byte) 128;
                }
            }
        }
        return data;
    }

    private static void crop(int format, int stride, Rect crop, boolean trimFinalPadding) {
        int width = 64, height = 32;
        byte[] data = image(format, width, height, stride);
        if (trimFinalPadding) {
            // The final row is present through its last used pixel; trailing
            // stride padding is not part of the crop's pixels.
            int end = format == ImageFormat.NV21
                    ? stride * height + (height / 2 - 1) * stride + width
                    : (height - 1) * stride + width * 2;
            data = Arrays.copyOf(data, end);
        }
        YuvImage image = new YuvImage(data, format, width, height,
                format == ImageFormat.NV21 ? new int[] {stride, stride} : new int[] {stride});
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        check(image.compressToJpeg(crop, 90, output), "cropped JPEG encoding succeeds");
        byte[] jpeg = output.toByteArray();
        Bitmap bitmap = BitmapFactory.decodeByteArray(jpeg, 0, jpeg.length);
        check(bitmap != null, "cropped JPEG decodes");
        try {
            check(bitmap.getWidth() == crop.width() && bitmap.getHeight() == crop.height(),
                    "JPEG has the crop dimensions");
            int color = bitmap.getPixel(bitmap.getWidth() - 4, bitmap.getHeight() - 4);
            check(Color.red(color) > 160 && Color.green(color) > 160 && Color.blue(color) > 160,
                    "bottom right pixel comes from the bright source half");
            if (crop.left == 0) {
                color = bitmap.getPixel(4, 4);
                check(Color.red(color) < 100 && Color.green(color) < 100 && Color.blue(color) < 100,
                        "full JPEG retains the dark left half");
            }
        } finally {
            bitmap.recycle();
        }
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        extractor();
        for (int format : new int[] {ImageFormat.NV21, ImageFormat.YUY2}) {
            int stride = format == ImageFormat.NV21 ? 64 : 128;
            crop(format, stride, new Rect(0, 0, 64, 32), false);
            crop(format, stride, new Rect(32, 0, 64, 32), false);
            crop(format, stride, new Rect(32, 8, 64, 32), false);
            crop(format, stride + 16, new Rect(32, 8, 64, 32), true);
            crop(format, stride, new Rect(34, 0, 64, 32), false);
            crop(format, stride, new Rect(40, 0, 64, 32), false);
            crop(format, stride + 16, new Rect(34, 8, 64, 32), true);
        }
        System.out.println("MediaDataContractTest all passed: " + checks + " checks");
    }
}
