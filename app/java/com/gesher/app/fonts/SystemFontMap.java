/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app.fonts;

import android.graphics.FontListParser;
import android.graphics.Typeface;
import android.graphics.fonts.FontFamily;
import android.graphics.fonts.SystemFonts;
import android.os.SharedMemory;
import android.text.FontConfig;

import java.io.File;
import java.util.Map;

/**
 * The system font map, from {@code $ANDROID_ROOT/etc/fonts.xml} and the font files in
 * {@code $ANDROID_ROOT/fonts} (docs/fonts.md).
 *
 * <p>SystemFonts and Typeface name the device's {@code /system/etc/} and
 * {@code /system/fonts/} as constants; on Windows those are paths on the current drive
 * (docs/decisions/0010-paths.md). So Gesher does here what the system server and
 * ActivityThread do on a device, with $ANDROID_ROOT's paths: the font configuration is
 * parsed (FontListParser, as SystemFonts.getSystemFontConfig), the fallback chains and
 * typefaces built (SystemFonts.buildSystemFallback, buildSystemTypefaces), the map
 * serialized into a SharedMemory (Typeface.serializeFontMap, as FontManagerService does)
 * and installed from it (Typeface.setSystemFontMap(SharedMemory), as
 * ActivityThread.handleBindApplication does). That initializes Typeface.DEFAULT, the
 * other static typefaces and the native default; and once the map comes from a buffer, a
 * later setSystemFontMap(null) (bindApplication without a buffer) leaves it alone. See
 * docs/decisions/0022-fonts-system-font-map.md.
 */
public final class SystemFontMap {
    private static String sLoaded;

    private SystemFontMap() {
    }

    /** $ANDROID_ROOT, the device's /system. */
    public static String androidRoot() {
        String root = System.getenv("ANDROID_ROOT");
        return root == null || root.isEmpty() ? "/system" : root;
    }

    /** The font configuration: $ANDROID_ROOT/etc/fonts.xml. */
    public static String fontsXml() {
        return androidRoot() + "/etc/fonts.xml";
    }

    /** The font files' directory, with a trailing '/': $ANDROID_ROOT/fonts/. */
    public static String fontDir() {
        return androidRoot() + "/fonts/";
    }

    /**
     * Installs the system font map once per process. Returns a description ("N typefaces
     * from FILE (...)"), or null when there is no fonts.xml (no fonts installed: the
     * framework keeps its empty map). libandroid_runtime must be loaded.
     */
    public static synchronized String load() throws Exception {
        if (sLoaded != null) {
            return sLoaded;
        }
        File xml = new File(fontsXml());
        if (!xml.isFile()) {
            return null;
        }
        FontConfig config = FontListParser.parse(xml.getPath(), fontDir(),
                null /* no OEM customization */, null, null /* no updated fonts */,
                xml.lastModified(), 0 /* configVersion */);
        Map<String, FontFamily[]> fallback = SystemFonts.buildSystemFallback(config);
        Map<String, Typeface> map = SystemFonts.buildSystemTypefaces(config, fallback);
        if (!map.containsKey(Typeface.DEFAULT_FAMILY)) {
            throw new IllegalStateException(xml + " defines no " + Typeface.DEFAULT_FAMILY
                    + " family (are the fonts in " + fontDir() + "?)");
        }
        String how;
        SharedMemory buffer = null;
        try {
            buffer = Typeface.serializeFontMap(map);
            how = "font map through SharedMemory, " + buffer.getSize() + " bytes";
        } catch (Exception | LinkageError e) {
            how = "font map in this process (SharedMemory failed: " + e + ")";
        }
        if (buffer != null) {
            Typeface.setSystemFontMap(buffer);
        } else {
            Typeface.setSystemFontMap(map);
            Typeface.loadNativeSystemFonts();
        }
        sLoaded = map.size() + " typefaces from " + xml.getPath() + " ("
                + config.getNamedFamilyLists().size() + " named families, "
                + config.getFontFamilies().size() + " fallback families; " + how + ")";
        return sLoaded;
    }
}
