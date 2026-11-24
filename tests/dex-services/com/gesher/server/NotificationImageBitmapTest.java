/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.ColorSpace;
import android.graphics.drawable.Icon;
import java.io.*;
import java.lang.reflect.Field;
import java.nio.file.Files;
import java.util.Arrays;

/** Actual ART Bitmap/PNG/filesystem boundary, with no Windows notification UI. */
public final class NotificationImageBitmapTest {
    private static int checks;
    private static void check(boolean yes, String why) { if (!yes) throw new AssertionError(why); checks++; }
    private static boolean staged(NotificationImageStore.Prepared p) throws Exception {
        Field f = p.getClass().getDeclaredField("staged"); f.setAccessible(true); return f.get(p) != null;
    }
    private static File publish(String key, Object owner, Bitmap bitmap, boolean encode) throws Exception {
        try (var image = NotificationIcons.prepareLargeIcon("fixture.notification.bitmap", key, owner,
                Icon.createWithBitmap(bitmap))) {
            check(image != null && staged(image) == encode, "incorrect encode/cache decision: " + key);
            File file = image.publish(); check(file != null && file.isFile(), "PNG was not published"); return file;
        }
    }
    private static int[] pixels(Bitmap bitmap) {
        int[] result = new int[bitmap.getWidth() * bitmap.getHeight()];
        bitmap.getPixels(result, 0, bitmap.getWidth(), 0, 0, bitmap.getWidth(), bitmap.getHeight()); return result;
    }
    private static void equal(File file, Bitmap input) throws Exception {
        Bitmap decoded = new NotificationPngFixture.Saved(file).decode();
        try {
            check(decoded.getWidth() == input.getWidth() && decoded.getHeight() == input.getHeight(),
                    "PNG dimensions changed: source=" + input.getWidth() + "x" + input.getHeight()
                    + " decoded=" + decoded.getWidth() + "x" + decoded.getHeight());
            check(Arrays.equals(pixels(input), pixels(decoded)), "PNG pixels differ from actual Bitmap source");
        } finally { decoded.recycle(); }
    }
    private static void equalToDirectPng(File file, Bitmap input) throws Exception {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        check(input.compress(Bitmap.CompressFormat.PNG, 100, bytes), "direct reference encoder failed");
        byte[] body = bytes.toByteArray();
        Bitmap reference = BitmapFactory.decodeByteArray(body, 0, body.length);
        check(reference != null, "native reference PNG byte decoder returned null");
        try {
            Bitmap actual = new NotificationPngFixture.Saved(file).decode();
            try {
                check(reference.getWidth() == actual.getWidth() && reference.getHeight() == actual.getHeight()
                                && Arrays.equals(pixels(reference), pixels(actual)),
                        "transparent image differs from direct Bitmap PNG encoding");
            } finally { actual.recycle(); }
        } finally { reference.recycle(); }
    }

    private static void originalWriterControl(Bitmap input) throws Exception {
        // The pre-JAVA-07 large-icon pipeline: compress to byte[], write .tmp,
        // then rename. This isolates the descriptor decoder from our new store.
        ByteArrayOutputStream encoded = new ByteArrayOutputStream();
        check(input.compress(Bitmap.CompressFormat.PNG, 100, encoded), "original writer encoder failed");
        File file = new File(Device.dataDir(), "original-writer-control.png");
        File temp = new File(file.getPath() + ".tmp");
        try {
            try (OutputStream out = new FileOutputStream(temp)) { out.write(encoded.toByteArray()); }
            check(temp.renameTo(file), "original writer rename failed");
            equal(file, input);
            Bitmap byDescriptor = BitmapFactory.decodeFile(file.getPath());
            System.out.println("NOTIFICATION_PNG_ORIGINAL_WRITER_CONTROL file=" + file.getCanonicalPath()
                    + " byte_decoder=passed file_descriptor_decoder="
                    + (byDescriptor == null ? "null" : byDescriptor.getWidth() + "x" + byDescriptor.getHeight()));
            if (byDescriptor != null) {
                try {
                    check(byDescriptor.getWidth() == input.getWidth() && byDescriptor.getHeight() == input.getHeight()
                                    && Arrays.equals(pixels(input), pixels(byDescriptor)),
                            "available file-descriptor decoder differs from the original writer source");
                } finally { byDescriptor.recycle(); }
            }
        } finally { Files.deleteIfExists(temp.toPath()); Files.deleteIfExists(file.toPath()); }
    }
    private static void correctness() throws Exception {
        Object owner = new Object();
        int[] input = new int[1031 * 7];
        for (int i = 0; i < input.length; ++i) input[i] = 0xff000000 | ((i * 0x3d17) & 0xffffff);
        Bitmap bitmap = Bitmap.createBitmap(1031, 7, Bitmap.Config.ARGB_8888);
        bitmap.setPixels(input, 0, 1031, 0, 0, 1031, 7);
        originalWriterControl(bitmap);
        File png = publish("primary", owner, bitmap, true); equal(png, bitmap);
        publish("primary", owner, bitmap, false);
        Bitmap same = bitmap.copy(Bitmap.Config.ARGB_8888, true);
        publish("primary", owner, same, false);
        int generation = bitmap.getGenerationId(); bitmap.setPixel(1030, 6, 0xff12abef);
        check(generation != bitmap.getGenerationId(), "fixture did not mutate Bitmap generation");
        equal(publish("primary", owner, bitmap, true), bitmap);
        publish("primary", new Object(), bitmap, true);
        publish("primary", owner, bitmap, true);
        long stamp = png.lastModified(); byte[] corrupt = Files.readAllBytes(png.toPath()); corrupt[corrupt.length / 2] ^= 1;
        Files.write(png.toPath(), corrupt); png.setLastModified(stamp);
        equal(publish("primary", owner, bitmap, true), bitmap);
        byte[] before = Files.readAllBytes(png.toPath());
        same.eraseColor(0xff17294b);
        try (var ignored = NotificationIcons.prepareLargeIcon("fixture.notification.bitmap", "primary", owner, Icon.createWithBitmap(same))) { }
        check(Arrays.equals(before, Files.readAllBytes(png.toPath())), "abandoned prepared PNG changed published image");
        Bitmap rgb565 = bitmap.copy(Bitmap.Config.RGB_565, true);
        equal(publish("565", owner, rgb565, true), rgb565); publish("565", owner, rgb565, false);
        Bitmap alpha = Bitmap.createBitmap(513, 3, Bitmap.Config.ARGB_8888);
        int[] transparent = new int[513 * 3];
        for (int i = 0; i < transparent.length; ++i) transparent[i] = ((i & 255) << 24) | ((i * 0x5b13) & 0xffffff);
        alpha.setPixels(transparent, 0, 513, 0, 0, 513, 3);
        equalToDirectPng(publish("alpha", owner, alpha, true), alpha); publish("alpha", owner, alpha, false);
        alpha.setPixel(512, 2, 0x4312abef);
        equalToDirectPng(publish("alpha", owner, alpha, true), alpha);
        alpha.setHasAlpha(false); equalToDirectPng(publish("alpha", owner, alpha, true), alpha);
        alpha.setHasAlpha(true); equalToDirectPng(publish("alpha", owner, alpha, true), alpha);
        Bitmap wide = Bitmap.createBitmap(17, 9, Bitmap.Config.RGBA_F16, true, ColorSpace.get(ColorSpace.Named.DISPLAY_P3));
        wide.eraseColor(0xff345678);
        publish("wide", owner, wide, true); publish("wide", owner, wide, true);
        NotificationIcons.removeLargeIcon("fixture.notification.bitmap", "primary"); check(!png.exists(), "cancel retained PNG");
        NotificationIcons.removeLargeIcons("fixture.notification.bitmap");
        bitmap.recycle(); same.recycle(); rgb565.recycle(); alpha.recycle(); wide.recycle();
    }

    private static long median(long[] values) { long[] sorted = values.clone(); Arrays.sort(sorted); return sorted[sorted.length / 2]; }
    private static void measurement() throws Exception {
        Bitmap bitmap = Bitmap.createBitmap(256, 256, Bitmap.Config.ARGB_8888);
        int[] input = new int[256 * 256];
        int value = 0x12345678;
        for (int i = 0; i < input.length; ++i) { value ^= value << 13; value ^= value >>> 17; value ^= value << 5; input[i] = 0xff000000 | value & 0xffffff; }
        bitmap.setPixels(input, 0, 256, 0, 0, 256, 256);
        Object owner = new Object(); publish("measured", owner, bitmap, true);
        File baseline = new File(Device.dataDir(), "baseline-image.png");
        long[] old = new long[31], hit = new long[31]; long oldCopied = 0;
        for (int i = 0; i < old.length; ++i) {
            long start = System.nanoTime();
            ByteArrayOutputStream encoded = new ByteArrayOutputStream();
            check(bitmap.compress(Bitmap.CompressFormat.PNG, 100, encoded), "baseline Bitmap encoder failed");
            byte[] body = encoded.toByteArray(); oldCopied += body.length;
            try (OutputStream file = new FileOutputStream(baseline)) { file.write(body); }
            old[i] = System.nanoTime() - start;
            start = System.nanoTime(); publish("measured", owner, bitmap, false); hit[i] = System.nanoTime() - start;
        }
        System.out.println("NOTIFICATION_IMAGE_BITMAP_METRICS baseline_ns=" + Arrays.toString(old)
                + " reuse_ns=" + Arrays.toString(hit) + " baseline_median_ns=" + median(old)
                + " reuse_median_ns=" + median(hit) + " removed_toByteArray_bytes=" + oldCopied
                + " bitmap=256x256 loops=31 scope=component_elapsed_not_whole_app");
        NotificationIcons.removeLargeIcons("fixture.notification.bitmap"); Files.deleteIfExists(baseline.toPath()); bitmap.recycle();
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        File data = Files.createTempDirectory(new File(System.getenv("ANDROID_DATA")).toPath(), "notification-bitmap-").toFile();
        android.system.Os.setenv("ANDROID_DATA", data.getPath(), true);
        correctness(); measurement();
        System.out.println("NotificationImageBitmapTest passed: " + checks + " checks");
    }
}
