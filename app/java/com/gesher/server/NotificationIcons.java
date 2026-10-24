/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.graphics.Bitmap;
import android.graphics.drawable.Icon;

import com.gesher.app.shell.AppIcon;

import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.HashMap;
import java.util.Map;

/**
 * Pictures for an app's toasts (devsvc track): the app's launcher icon as a PNG, drawn
 * by the shell track's {@link AppIcon} (the framework's resources and drawables, as the
 * Start menu shortcut's icon), and a notification's large icon when it is a bitmap. In
 * {@code $ANDROID_DATA/system/notification-icons}; Windows gets them by file path.
 */
final class NotificationIcons {
    private NotificationIcons() {
    }

    /** An app's label and icon, as drawn for a version of its APK. */
    static final class AppPicture {
        final String label;
        final File png;  // null: none could be drawn

        AppPicture(String label, File png) {
            this.label = label;
            this.png = png;
        }
    }

    // Guarded by NotificationIcons.class: by package, with the "<apk path>|<apk time>" key.
    private static final Map<String, AppPicture> sDrawn = new HashMap<>();
    private static final Map<String, String> sStamps = new HashMap<>();

    static File dir() {
        return new File(Device.dataDir(), "system/notification-icons");
    }

    /** The app's label and icon (a PNG file, drawn again when its APK changed). */
    static synchronized AppPicture app(ApkPackage pkg) {
        String stamp = pkg.apkPath + "|" + new File(pkg.apkPath).lastModified();
        AppPicture cached = sDrawn.get(pkg.packageName);
        if (cached != null && stamp.equals(sStamps.get(pkg.packageName))) return cached;
        String label = pkg.packageName;
        File png = null;
        try {
            ApplicationInfo ai = pkg.applicationInfo(0);
            ActivityInfo launcher = null;
            try {
                launcher = pkg.launcherActivity(null);
            } catch (RuntimeException e) {
                // no launcher activity: the application's icon or the default
            }
            AppIcon icon = AppIcon.load(pkg.apkPath, ai, launcher);
            label = icon.label;
            byte[] data = icon.hasImages() ? largestPng(icon.ico()) : null;
            if (data != null) {
                png = new File(dir(), pkg.packageName + ".png");
                write(png, data);
            }
        } catch (Throwable t) {
            SystemServer.log("notification: no icon for " + pkg.packageName + ": " + t);
        }
        AppPicture p = new AppPicture(label, png);
        sDrawn.put(pkg.packageName, p);
        sStamps.put(pkg.packageName, stamp);
        return p;
    }

    /** A notification's large icon, if it is a bitmap, as a PNG file; else null. */
    static File largeIcon(String packageName, String key, Icon icon) {
        if (icon == null || icon.getType() != Icon.TYPE_BITMAP) return null;
        try {
            Bitmap b = icon.getBitmap();
            if (b == null) return null;
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            if (!b.compress(Bitmap.CompressFormat.PNG, 100, out)) return null;
            File png = new File(dir(), packageName + "-" + Integer.toHexString(key.hashCode())
                    + ".png");
            write(png, out.toByteArray());
            return png;
        } catch (Throwable t) {
            return null;
        }
    }

    /** The largest PNG image of an .ico (AppIcon writes PNGs from 64 pixels on). */
    private static byte[] largestPng(byte[] ico) {
        ByteBuffer b = ByteBuffer.wrap(ico).order(ByteOrder.LITTLE_ENDIAN);
        int count = b.getShort(4) & 0xffff;
        byte[] best = null;
        int bestSize = 0;
        for (int i = 0; i < count; i++) {
            int entry = 6 + 16 * i;
            int w = ico[entry] & 0xff;
            int size = w == 0 ? 256 : w;
            int length = b.getInt(entry + 8);
            int offset = b.getInt(entry + 12);
            boolean png = length > 8 && ico[offset] == (byte) 0x89 && ico[offset + 1] == 'P';
            if (png && size > bestSize) {
                best = java.util.Arrays.copyOfRange(ico, offset, offset + length);
                bestSize = size;
            }
        }
        return best;
    }

    private static void write(File file, byte[] data) throws java.io.IOException {
        file.getParentFile().mkdirs();
        File tmp = new File(file.getPath() + ".tmp");
        try (OutputStream out = new FileOutputStream(tmp)) {
            out.write(data);
        }
        if (!tmp.renameTo(file)) {
            file.delete();
            if (!tmp.renameTo(file)) throw new java.io.IOException("cannot write " + file);
        }
    }
}
