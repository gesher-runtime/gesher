/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app.shell;

import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.BitmapShader;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Shader;
import android.graphics.drawable.AdaptiveIconDrawable;
import android.graphics.drawable.BitmapDrawable;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.os.LocaleList;
import android.util.DisplayMetrics;
import android.util.TypedValue;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Locale;

/**
 * An installed app's label and launcher icon, as a launcher shows them, made into a Windows
 * icon: the APK's resources are read with the framework's own AssetManager and Resources (the
 * default locale, xxxhdpi, so the best density of a mipmap is picked), the icon is inflated
 * and drawn with android.graphics (hwui's Skia: PNG, WebP and JPEG decoders, vector
 * drawables), an adaptive icon through {@link AdaptiveIconDrawable} (its foreground over its
 * background, in the system's icon mask), and the pictures go into a multi-size .ico.
 *
 * <p>If the framework cannot inflate or draw the icon, its layers are composed here (an
 * adaptive icon's background and foreground, decoded with BitmapFactory if need be, in a
 * circle); an app without an icon gets the framework's default one (sym_def_app_icon), as on
 * Android.
 */
public final class AppIcon {
    /** The icon's sizes: Explorer's and the Start menu's at 100-200% scaling. */
    public static final int[] SIZES = {16, 20, 24, 32, 40, 48, 64, 96, 128, 256};
    private static final int DENSITY = DisplayMetrics.DENSITY_XXXHIGH;
    private static final String ANDROID_NS = "http://schemas.android.com/apk/res/android";

    public final String packageName;
    /** The app's label in the default locale (the package name if it has none). */
    public final String label;
    /** What was drawn, for logs: the resource's file, or the layers of an adaptive icon. */
    public final String source;
    /** The pictures, one per {@link #SIZES}; null when nothing could be drawn. */
    private final Bitmap[] mImages;

    private AppIcon(String packageName, String label, String source, Bitmap[] images) {
        this.packageName = packageName;
        this.label = label;
        this.source = source;
        mImages = images;
    }

    public boolean hasImages() {
        return mImages != null;
    }

    /**
     * Reads the label and draws the icon of the app in {@code apk}: the application's icon,
     * else the launcher activity's, else the framework's default.
     */
    public static AppIcon load(String apk, ApplicationInfo app, ActivityInfo launcher) {
        Resources res = resources(apk);
        String label = label(res, app, launcher);
        int icon = app.icon != 0 ? app.icon : launcher != null ? launcher.icon : 0;
        StringBuilder source = new StringBuilder();
        Bitmap[] images = null;
        if (icon != 0) {
            try {
                images = draw(res, icon, source);
            } catch (Throwable t) {
                source.append(" (not drawn: ").append(t).append(')');
            }
        }
        if (images == null) {
            // As PackageItemInfo.loadIcon: the default activity icon.
            String why = icon == 0 ? "no icon" : source.toString();
            source.setLength(0);
            try {
                images = draw(res, android.R.mipmap.sym_def_app_icon, source);
                source.insert(0, "the framework's default icon (" + why + "): ");
            } catch (Throwable t) {
                source.setLength(0);
                source.append(why).append("; the framework's default icon not drawn: ")
                        .append(t);
            }
        }
        return new AppIcon(app.packageName, label, source.toString(), images);
    }

    /** The app's resources with the system's, for the default locale at xxxhdpi. */
    private static Resources resources(String apk) {
        AssetManager assets = new AssetManager();
        if (assets.addAssetPath(apk) == 0) {
            throw new IllegalStateException("cannot read the resources of " + apk);
        }
        DisplayMetrics metrics = new DisplayMetrics();
        metrics.setToDefaults();
        metrics.densityDpi = DENSITY;
        metrics.density = DENSITY / (float) DisplayMetrics.DENSITY_DEFAULT;
        metrics.scaledDensity = metrics.density;
        Configuration config = new Configuration();
        config.setToDefaults();
        config.densityDpi = DENSITY;
        config.setLocales(new LocaleList(Locale.getDefault()));
        return new Resources(assets, metrics, config);
    }

    private static String label(Resources res, ApplicationInfo app, ActivityInfo launcher) {
        CharSequence label = null;
        if (app.labelRes != 0) {
            try {
                label = res.getText(app.labelRes);
            } catch (Resources.NotFoundException e) {
                // the non-localized label, or the package name
            }
        }
        if (isBlank(label)) label = app.nonLocalizedLabel;
        if (isBlank(label) && launcher != null && launcher.labelRes != 0) {
            try {
                label = res.getText(launcher.labelRes);
            } catch (Resources.NotFoundException e) {
                // the package name
            }
        }
        return isBlank(label) ? app.packageName : label.toString().trim();
    }

    private static boolean isBlank(CharSequence s) {
        return s == null || s.toString().trim().isEmpty();
    }

    // --- drawing ---------------------------------------------------------------------------

    /** Draws resource {@code id} at every size; {@code source} gets what it is. */
    private static Bitmap[] draw(Resources res, int id, StringBuilder source) throws Exception {
        source.append(file(res, id));
        Drawable d = null;
        Throwable failure = null;
        try {
            d = drawable(res, id);
        } catch (Throwable t) {
            failure = t;
        }
        Layers layers = null;
        if (d instanceof AdaptiveIconDrawable) {
            AdaptiveIconDrawable a = (AdaptiveIconDrawable) d;
            layers = new Layers(a.getBackground(), a.getForeground());
        } else if (d == null) {
            // The framework could not inflate it: an adaptive icon's layers one by one.
            layers = adaptiveLayers(res, id);
            if (layers == null) throw new IllegalStateException("not inflated", failure);
        }
        if (layers != null) {
            source.append(" (adaptive icon: background ").append(layers.describe(res, id,
                    "background")).append(", foreground ").append(layers.describe(res, id,
                    "foreground")).append(')');
        }
        Bitmap master;
        if (d != null) {
            try {
                master = render(d, 256);
            } catch (Throwable t) {
                if (layers == null) throw t;
                master = layers.compose(256);
                source.append(", composed by Gesher (").append(t).append(')');
            }
        } else {
            master = layers.compose(256);
            source.append(", composed by Gesher");
        }
        if (isEmpty(master)) throw new IllegalStateException("the icon drew nothing");
        Bitmap[] images = new Bitmap[SIZES.length];
        for (int i = 0; i < SIZES.length; i++) {
            images[i] = scaled(master, SIZES[i]);
        }
        return images;
    }

    /** The resource's file in the APK at the chosen density (or its color). */
    private static String file(Resources res, int id) {
        try {
            TypedValue v = new TypedValue();
            res.getValueForDensity(id, DENSITY, v, true);
            if (v.string != null) return v.string.toString();
            if (v.type >= TypedValue.TYPE_FIRST_COLOR_INT
                    && v.type <= TypedValue.TYPE_LAST_COLOR_INT) {
                return String.format("#%08x", v.data);
            }
            return res.getResourceName(id);
        } catch (Throwable t) {
            return "resource 0x" + Integer.toHexString(id);
        }
    }

    /**
     * Resource {@code id} as a drawable, at xxxhdpi; a bitmap the framework's decoder does not
     * take is decoded with BitmapFactory.
     */
    private static Drawable drawable(Resources res, int id) throws Exception {
        try {
            return res.getDrawableForDensity(id, DENSITY, null);
        } catch (Exception | LinkageError e) {
            TypedValue v = new TypedValue();
            res.getValueForDensity(id, DENSITY, v, true);
            if (v.string == null || v.string.toString().endsWith(".xml")) throw e;
            try (InputStream in = res.openRawResource(id, v)) {
                Bitmap b = BitmapFactory.decodeStream(in);
                if (b == null) throw e;
                return new BitmapDrawable(res, b);
            }
        }
    }

    /** An adaptive icon's layers, read from its XML: null if it is not one. */
    private static Layers adaptiveLayers(Resources res, int id) throws Exception {
        TypedValue v = new TypedValue();
        res.getValueForDensity(id, DENSITY, v, true);
        if (v.string == null || !v.string.toString().endsWith(".xml")) return null;
        Drawable background = null;
        Drawable foreground = null;
        boolean adaptive = false;
        try (XmlResourceParser p = res.getXml(id)) {
            for (int e = p.next(); e != XmlResourceParser.END_DOCUMENT; e = p.next()) {
                if (e != XmlResourceParser.START_TAG) continue;
                switch (p.getName()) {
                    case "adaptive-icon": adaptive = true; break;
                    case "background": background = layer(res, p); break;
                    case "foreground": foreground = layer(res, p); break;
                    default: break;
                }
            }
        }
        return adaptive ? new Layers(background, foreground) : null;
    }

    private static Drawable layer(Resources res, XmlResourceParser p) throws Exception {
        int id = p.getAttributeResourceValue(ANDROID_NS, "drawable", 0);
        return id == 0 ? null : drawable(res, id);
    }

    /** An adaptive icon's background and foreground. */
    private static final class Layers {
        final Drawable background;
        final Drawable foreground;

        Layers(Drawable background, Drawable foreground) {
            this.background = background;
            this.foreground = foreground;
        }

        String describe(Resources res, int id, String which) {
            Drawable d = which.equals("background") ? background : foreground;
            if (d == null) return "none";
            String what = d instanceof ColorDrawable
                    ? String.format("#%08x", ((ColorDrawable) d).getColor())
                    : d.getClass().getSimpleName();
            String file = layerFile(res, id, which);
            return file == null ? what : file.equals(what) ? file : file + " (" + what + ")";
        }

        /**
         * The layers drawn as AdaptiveIconDrawable does: each 108dp, the inner 72dp visible
         * (a quarter of the picture's size outside it on every side), in a circle (the
         * framework's config_icon_mask).
         */
        Bitmap compose(int size) {
            Bitmap layers = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
            Canvas c = new Canvas(layers);
            int out = size / 4;
            for (Drawable d : new Drawable[] {background, foreground}) {
                if (d == null) continue;
                d.setBounds(-out, -out, size + out, size + out);
                d.draw(c);
            }
            Bitmap result = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
            Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
            paint.setShader(new BitmapShader(layers, Shader.TileMode.CLAMP,
                    Shader.TileMode.CLAMP));
            new Canvas(result).drawCircle(size / 2f, size / 2f, size / 2f, paint);
            return result;
        }
    }

    /** The file of an adaptive icon's layer, from the icon's XML; null if unknown. */
    private static String layerFile(Resources res, int id, String which) {
        try (XmlResourceParser p = res.getXml(id)) {
            for (int e = p.next(); e != XmlResourceParser.END_DOCUMENT; e = p.next()) {
                if (e == XmlResourceParser.START_TAG && which.equals(p.getName())) {
                    int layer = p.getAttributeResourceValue(ANDROID_NS, "drawable", 0);
                    return layer == 0 ? null : file(res, layer);
                }
            }
        } catch (Throwable t) {
            // unknown
        }
        return null;
    }

    private static Bitmap render(Drawable d, int size) {
        Bitmap b = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(b);
        d.setBounds(0, 0, size, size);
        d.draw(c);
        return b;
    }

    /** {@code master} scaled down to {@code size}, halving first (a smoother result). */
    private static Bitmap scaled(Bitmap master, int size) {
        Bitmap b = master;
        while (b.getWidth() / 2 >= size && b.getWidth() / 2 > 0) {
            b = Bitmap.createScaledBitmap(b, b.getWidth() / 2, b.getHeight() / 2, true);
        }
        if (b.getWidth() != size) b = Bitmap.createScaledBitmap(b, size, size, true);
        return b;
    }

    private static boolean isEmpty(Bitmap b) {
        int w = b.getWidth();
        int h = b.getHeight();
        int[] px = new int[w * h];
        b.getPixels(px, 0, w, 0, 0, w, h);
        for (int p : px) {
            if ((p >>> 24) != 0) return false;
        }
        return true;
    }

    // --- the .ico --------------------------------------------------------------------------

    /**
     * The .ico: one image per size, 32-bit DIBs (BGRA with an AND mask, which every reader
     * takes) up to 48 pixels, PNGs (which Windows reads since Vista) from 64 on.
     */
    public byte[] ico() throws IOException {
        if (mImages == null) throw new IllegalStateException("no icon");
        byte[][] data = new byte[mImages.length][];
        for (int i = 0; i < mImages.length; i++) {
            data[i] = mImages[i].getWidth() >= 64 ? png(mImages[i]) : dib(mImages[i]);
        }
        int offset = 6 + 16 * mImages.length;
        int total = offset;
        for (byte[] d : data) total += d.length;
        ByteBuffer out = ByteBuffer.allocate(total).order(ByteOrder.LITTLE_ENDIAN);
        out.putShort((short) 0).putShort((short) 1).putShort((short) mImages.length);
        for (int i = 0; i < mImages.length; i++) {
            int w = mImages[i].getWidth();
            int h = mImages[i].getHeight();
            out.put((byte) (w >= 256 ? 0 : w)).put((byte) (h >= 256 ? 0 : h));
            out.put((byte) 0).put((byte) 0);              // no palette, reserved
            out.putShort((short) 1).putShort((short) 32);  // planes, bits per pixel
            out.putInt(data[i].length).putInt(offset);
            offset += data[i].length;
        }
        for (byte[] d : data) out.put(d);
        return out.array();
    }

    public void writeIco(File file) throws IOException {
        byte[] ico = ico();
        file.getParentFile().mkdirs();
        File tmp = new File(file.getPath() + ".tmp");
        try (OutputStream out = new FileOutputStream(tmp)) {
            out.write(ico);
        }
        if (!tmp.renameTo(file)) {
            file.delete();
            if (!tmp.renameTo(file)) throw new IOException("cannot write " + file);
        }
    }

    private static byte[] png(Bitmap b) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        if (!b.compress(Bitmap.CompressFormat.PNG, 100, out)) {
            throw new IOException("PNG compression failed");
        }
        return out.toByteArray();
    }

    /** A BITMAPINFOHEADER, bottom-up BGRA rows, then the 1-bit AND mask (transparent: 1). */
    private static byte[] dib(Bitmap b) {
        int w = b.getWidth();
        int h = b.getHeight();
        int maskStride = (w + 31) / 32 * 4;
        int[] px = new int[w * h];
        b.getPixels(px, 0, w, 0, 0, w, h);  // unpremultiplied ARGB
        ByteBuffer out = ByteBuffer.allocate(40 + w * h * 4 + maskStride * h)
                .order(ByteOrder.LITTLE_ENDIAN);
        out.putInt(40).putInt(w).putInt(h * 2).putShort((short) 1).putShort((short) 32);
        out.putInt(0 /* BI_RGB */).putInt(w * h * 4 + maskStride * h);
        out.putInt(0).putInt(0).putInt(0).putInt(0);
        for (int y = h - 1; y >= 0; y--) {
            for (int x = 0; x < w; x++) {
                int c = px[y * w + x];
                out.put((byte) c).put((byte) (c >> 8)).put((byte) (c >> 16))
                        .put((byte) (c >>> 24));
            }
        }
        for (int y = h - 1; y >= 0; y--) {
            byte[] row = new byte[maskStride];
            for (int x = 0; x < w; x++) {
                if ((px[y * w + x] >>> 24) == 0) row[x >> 3] |= (byte) (0x80 >> (x & 7));
            }
            out.put(row);
        }
        return out.array();
    }
}
