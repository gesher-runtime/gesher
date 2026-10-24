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
import java.util.ArrayList;
import java.util.List;
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
 *
 * <p>The fallback families of {@code $ANDROID_ROOT/etc/windows-fonts.xml}, fonts Windows
 * has (CJK, color emoji, symbols, further scripts), read in place from its fonts folder
 * ({@link WindowsFonts}), are appended to fonts.xml's fallback list; files Windows lacks
 * are left out (docs/decisions/0044-rtl-scripts-and-locale.md).
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

    /** The Windows fonts' fallback families: $ANDROID_ROOT/etc/windows-fonts.xml. */
    public static String windowsFontsXml() {
        return androidRoot() + "/etc/windows-fonts.xml";
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
        String windows = "";
        File windowsXml = new File(windowsFontsXml());
        if (windowsXml.isFile()) {
            String dir = WindowsFonts.directory();
            if (dir == null) {
                windows = "; no Windows fonts: " + WindowsFonts.whyNot();
            } else {
                FontConfig extra = FontListParser.parse(windowsXml.getPath(), dir, null, null,
                        null, windowsXml.lastModified(), 0);
                List<FontConfig.FontFamily> families = new ArrayList<>(config.getFontFamilies());
                families.addAll(extra.getFontFamilies());
                config = new FontConfig(families, config.getAliases(),
                        config.getNamedFamilyLists(), config.getLocaleFallbackCustomizations(),
                        config.getLastModifiedTimeMillis(), config.getConfigVersion());
                int fonts = 0;
                for (FontConfig.FontFamily family : extra.getFontFamilies()) {
                    fonts += family.getFontList().size();
                }
                windows = "; " + extra.getFontFamilies().size() + " fallback families ("
                        + fonts + " fonts) of Windows' from " + dir;
            }
        }
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
                + config.getFontFamilies().size() + " fallback families" + windows + "; " + how
                + ")";
        return sLoaded;
    }
}
