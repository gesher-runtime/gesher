/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.Notification;
import android.graphics.Bitmap;
import android.graphics.drawable.Icon;
import java.io.File;
import java.lang.reflect.Method;
import java.nio.file.Files;

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
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        check(!A.equals(B) && A.hashCode() == B.hashCode(), "Invalid fixture");
        check(!file(A).equals(file(B)), "Colliding keys share storage path");
        if (args[0].equals("write")) {
            Bitmap red = Bitmap.createBitmap(2, 2, Bitmap.Config.ARGB_8888);
            Bitmap blue = Bitmap.createBitmap(2, 2, Bitmap.Config.ARGB_8888);
            try {
                red.eraseColor(0xffff0000); blue.eraseColor(0xff0000ff);
                File first = NotificationIcons.largeIcon(PKG, A, Icon.createWithBitmap(red));
                File second = NotificationIcons.largeIcon(PKG, B, Icon.createWithBitmap(blue));
                check(first != null && second != null && !first.equals(second), "Large icon paths alias");
                check(!java.util.Arrays.equals(Files.readAllBytes(first.toPath()), Files.readAllBytes(second.toPath())),
                        "Distinct notification icons overwrote image content");
                String longPackage = "com." + "a".repeat(200);
                check(android.content.pm.parsing.FrameworkParsingPackageUtils.validateName(longPackage, true, true) == null,
                        "Long-package fixture rejected by Android's package validator");
                File longImage = NotificationIcons.largeIcon(longPackage,
                        "0|" + longPackage + "|7|Aa|10000", Icon.createWithBitmap(red));
                check(longImage != null && longImage.isFile() && longImage.getName().length() <= 255,
                        "Long valid Android package could not write its large icon");
            } finally { red.recycle(); blue.recycle(); }
            NotificationStore.save(null, A, PKG, "Aa", 7, new Notification());
            NotificationStore.save(null, B, PKG, "BB", 7, new Notification());
            restored(A, "Aa"); restored(B, "BB");
        } else {
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
