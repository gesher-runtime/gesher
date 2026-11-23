/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.graphics.Bitmap;
import android.graphics.ColorSpace;
import android.graphics.drawable.Icon;

import com.gesher.app.shell.AppIcon;

import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.HashMap;
import java.util.Map;
import java.util.Objects;
import java.security.MessageDigest;

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

    /**
     * Prepare before taking the notification publication fence. The returned
     * owner must be closed even when a package/channel/cancellation became stale.
     * A null image still participates in the same-key lane, so an update removing
     * its large icon cannot invalidate another post's prepared cache hit.
     */
    static NotificationImageStore.Prepared prepareLargeIcon(String packageName, String key,
            Object owner, Icon icon) {
        try {
            Bitmap bitmap = icon != null && icon.getType() == Icon.TYPE_BITMAP ? icon.getBitmap() : null;
            return NotificationImageStore.prepare(largeFile(packageName, key), owner,
                    bitmap == null ? null : new BitmapImage(bitmap));
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            return null;
        } catch (Throwable t) {
            return null;
        }
    }

    private static final class BitmapImage implements NotificationImageStore.Image {
        private final Bitmap bitmap;
        private int generation, width, height;
        private Bitmap.Config config;
        private ColorSpace color;
        private boolean alpha, premultiplied;

        BitmapImage(Bitmap bitmap) { this.bitmap = bitmap; }

        @Override public byte[] signature() {
            generation = bitmap.getGenerationId();
            width = bitmap.getWidth(); height = bitmap.getHeight();
            config = bitmap.getConfig(); color = bitmap.getColorSpace();
            alpha = bitmap.hasAlpha(); premultiplied = bitmap.isPremultiplied();
            // getPixels is an exact content key for the ordinary sRGB formats.
            // It converts wide-gamut/HDR values, so never use that lossy view to
            // establish equality for those sources. Encoding still supports them.
            if ((config != Bitmap.Config.ARGB_8888 && config != Bitmap.Config.RGB_565)
                    || color != null && !ColorSpace.get(ColorSpace.Named.SRGB).equals(color)
                    || bitmap.hasGainmap()) return null;
            MessageDigest digest = NotificationImageStore.sha256();
            ByteBuffer metadata = ByteBuffer.allocate(24).order(ByteOrder.BIG_ENDIAN);
            metadata.putInt(width).putInt(height).putInt(config.ordinal())
                    .putInt(alpha ? 1 : 0).putInt(premultiplied ? 1 : 0)
                    .putInt(color == null ? -1 : color.getId());
            digest.update(metadata.array());
            // Fixed scratch regardless of dimensions; no full-image or PNG copy.
            int[] pixels = new int[1024];
            byte[] bytes = new byte[pixels.length * 4];
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; x += pixels.length) {
                    int count = Math.min(pixels.length, width - x);
                    bitmap.getPixels(pixels, 0, count, x, y, count, 1);
                    for (int i = 0; i < count; ++i) {
                        int pixel = pixels[i], at = i * 4;
                        bytes[at] = (byte)(pixel >>> 24); bytes[at + 1] = (byte)(pixel >>> 16);
                        bytes[at + 2] = (byte)(pixel >>> 8); bytes[at + 3] = (byte)pixel;
                    }
                    digest.update(bytes, 0, count * 4);
                }
            }
            return unchanged() ? digest.digest() : null;
        }

        @Override public boolean encode(OutputStream out) throws IOException {
            return bitmap.compress(Bitmap.CompressFormat.PNG, 100, out);
        }

        @Override public boolean unchanged() {
            // Generation tracks pixels; format flags can change independently.
            return !bitmap.isRecycled() && bitmap.getGenerationId() == generation
                    && bitmap.getWidth() == width && bitmap.getHeight() == height && bitmap.getConfig() == config
                    && bitmap.hasAlpha() == alpha && bitmap.isPremultiplied() == premultiplied
                    && Objects.equals(bitmap.getColorSpace(), color) && !bitmap.hasGainmap();
        }
    }

    static void removeLargeIcon(String packageName, String key) {
        removeCurrentLargeIcon(packageName, key);
        NotificationImageStore.remove(legacyLargeFile(packageName, key));
    }

    static void removeCurrentLargeIcon(String packageName, String key) {
        NotificationImageStore.remove(largeFile(packageName, key));
    }

    static void removeLargeIcons(String packageName) {
        File[] files = largeDir(packageName).listFiles();
        if (files == null) return;
        for (File file : files) {
            // A blocked native encoder retains its .part file until it returns.
            if (file.getName().startsWith("large-sha256-") && file.getName().endsWith(".png")) {
                NotificationImageStore.remove(file);
            }
        }
    }

    static void removeLegacyLargeIcon(String packageName, String key) {
        NotificationImageStore.remove(legacyLargeFile(packageName, key));
    }

    private static File largeDir(String packageName) {
        return new File(dir(), "large-" + NotificationIdentity.digest(packageName));
    }

    private static File largeFile(String packageName, String key) {
        // Keep the basename bounded even for long valid Android package names.
        // Prefix the package's code-unit length to delimit it from the key.
        return new File(largeDir(packageName), "large-sha256-"
                + NotificationIdentity.digest(packageName.length() + ":" + packageName + key) + ".png");
    }

    private static File legacyLargeFile(String packageName, String key) {
        return new File(dir(), largeFile(packageName, key).getName());
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
