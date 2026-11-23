/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.Notification;
import android.graphics.Bitmap;
import android.graphics.drawable.Icon;
import java.io.File;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.nio.charset.StandardCharsets;

/** Narrow ART gate using real Android Parcel and production store files, without UI. */
public final class NotificationIdentityDevice {
    static int checks;
    static final String PKG = "com.example.identity", A = "0|com.example.identity|7|Aa|10000",
            B = "0|com.example.identity|7|BB|10000";
    static void check(boolean ok, String why) { ++checks; if (!ok) throw new AssertionError(why); }
    static File file(String key) throws Exception {
        Method method = NotificationStore.class.getDeclaredMethod("file", String.class, String.class);
        method.setAccessible(true); return (File)method.invoke(null, PKG, key);
    }
    static void restored(String key, String tag) {
        NotificationStore.Stored value = NotificationStore.load(PKG, key);
        check(value != null && key.equals(value.key) && tag.equals(value.tag), "Stored notification identity lost");
    }
    static File largeIcon(String pkg, String key, Bitmap bitmap) throws Exception {
        Method prepare;
        try {
            prepare = NotificationIcons.class.getDeclaredMethod("prepareLargeIcon",
                    String.class, String.class, Object.class, Icon.class);
        } catch (NoSuchMethodException beforePreparedOwner) {
            // Keep this identity oracle runnable against historical source refs.
            // The image-owner fixture separately requires the new preparation API.
            Method old = NotificationIcons.class.getDeclaredMethod("largeIcon", String.class, String.class, Icon.class);
            old.setAccessible(true); return (File)old.invoke(null, pkg, key, Icon.createWithBitmap(bitmap));
        }
        prepare.setAccessible(true);
        try (AutoCloseable prepared = (AutoCloseable)prepare.invoke(null, pkg, key, null, Icon.createWithBitmap(bitmap))) {
            if (prepared == null) return null;
            Method publish = prepared.getClass().getDeclaredMethod("publish"); publish.setAccessible(true);
            return (File)publish.invoke(prepared);
        }
    }
    static File imageReceipt(String key) {
        return new File(Device.dataDir(), "notification-image-" + NotificationIdentity.digest(key) + ".receipt");
    }
    static void rememberImage(String key, File file) throws Exception {
        NotificationPngFixture.Saved png = new NotificationPngFixture.Saved(file);
        Files.write(imageReceipt(key).toPath(), png.receipt().getBytes(StandardCharsets.UTF_8));
    }
    static void retainedImage(String key, int color) throws Exception {
        Method path = NotificationIcons.class.getDeclaredMethod("largeFile", String.class, String.class);
        path.setAccessible(true);
        File file = (File)path.invoke(null, PKG, key);
        NotificationPngFixture.Saved png = new NotificationPngFixture.Saved(file);
        String written = new String(Files.readAllBytes(imageReceipt(key).toPath()), StandardCharsets.UTF_8);
        check(png.receipt().equals(written), "Retained toast image path or bytes changed after process restart");
        Bitmap bitmap = png.decode();
        try {
            check(bitmap.getWidth() == 2 && bitmap.getHeight() == 2,
                    "Retained toast image dimensions changed after process restart");
            for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) {
                check(bitmap.getPixel(x, y) == color, "Retained toast image pixels changed after process restart");
            }
        } finally { bitmap.recycle(); }
        System.out.println("NOTIFICATION_PNG_RESTART_VERIFIED path=" + png.path + " bytes=" + png.bytes.length
                + " sha256=" + png.sha256);
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        check(!A.equals(B) && A.hashCode() == B.hashCode(), "Invalid fixture");
        check(!file(A).equals(file(B)), "Colliding keys share storage path");
        if (args[0].equals("write")) {
            Bitmap red = Bitmap.createBitmap(2, 2, Bitmap.Config.ARGB_8888);
            Bitmap blue = Bitmap.createBitmap(2, 2, Bitmap.Config.ARGB_8888);
            try {
                red.eraseColor(0xffff0000); blue.eraseColor(0xff0000ff);
                File first = largeIcon(PKG, A, red);
                File second = largeIcon(PKG, B, blue);
                check(first != null && second != null && !first.equals(second), "Large icon paths alias");
                check(!java.util.Arrays.equals(Files.readAllBytes(first.toPath()), Files.readAllBytes(second.toPath())),
                        "Distinct notification icons overwrote image content");
                rememberImage(A, first); rememberImage(B, second);
                String longPackage = "com." + "a".repeat(200);
                check(android.content.pm.parsing.FrameworkParsingPackageUtils.validateName(longPackage, true, true) == null,
                        "Long-package fixture rejected by Android's package validator");
                File longImage = largeIcon(longPackage, "0|" + longPackage + "|7|Aa|10000", red);
                check(longImage != null && longImage.isFile() && longImage.getName().length() <= 255,
                        "Long valid Android package could not write its large icon");
            } finally { red.recycle(); blue.recycle(); }
            NotificationStore.save(null, A, PKG, "Aa", 7, new Notification());
            NotificationStore.save(null, B, PKG, "BB", 7, new Notification());
            restored(A, "Aa"); restored(B, "BB");
        } else {
            retainedImage(A, 0xffff0000); retainedImage(B, 0xff0000ff);
            restored(A, "Aa"); restored(B, "BB");
            File legacy = new File(file(A).getParentFile(), Integer.toHexString(A.hashCode()) + ".n");
            Files.copy(file(A).toPath(), legacy.toPath()); Files.delete(file(A).toPath());
            restored(A, "Aa");
            NotificationStore.delete(PKG, B);
            check(legacy.isFile(), "Canceled peer removed another key's legacy record");
            restored(A, "Aa");
            NotificationStore.save(null, B, PKG, "BB", 7, new Notification());
            check(legacy.isFile(), "Saved peer removed another key's legacy record");
            NotificationStore.save(null, A, PKG, "Aa", 7, new Notification());
            check(!legacy.exists(), "Same-key update failed to retire legacy record");
            restored(A, "Aa"); restored(B, "BB");
            NotificationStore.delete(PKG, A);
            check(NotificationStore.load(PKG, A) == null, "Canceled record survived");
            restored(B, "BB");
        }
        System.out.println("NotificationIdentityDevice " + args[0] + " passed: " + checks + " checks");
        ToastIdentityDevice.contracts(args[0]);
        NotificationStoreFormatDevice.contracts(args[0]);
    }
}
