/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app.fonts;

import java.io.File;

/**
 * The fonts Windows has, used in place as fallback families of the system font map
 * (docs/fonts.md, docs/decisions/0044-rtl-scripts-and-locale.md): CJK, color emoji,
 * symbols and the scripts Gesher's bundled Noto fonts leave out. Which files make which
 * family is {@code $ANDROID_ROOT/etc/windows-fonts.xml}; this class says where the files
 * are.
 *
 * <p>The folder is {@code %WINDIR%\Fonts} (the native asks Windows), or
 * {@code $GESHER_WINDOWS_FONTS} when it is set ({@code none} turns the Windows fonts off).
 * It must be reachable as a path on the current drive: the framework keeps a font's
 * absolute path ({@code File.getAbsolutePath}, which on Gesher's paths means
 * drive-less, ADR 0010) and maps the file again by it when the font map comes through a
 * SharedMemory. When Windows' fonts are on another drive than the current directory (a
 * test root on D:), {@code $GESHER_WINDOWS_FONTS} can name a junction to them on this
 * drive; otherwise they are not used.
 */
public final class WindowsFonts {
    /** The environment variable that overrides the folder, or turns it off ({@code none}). */
    public static final String ENV = "GESHER_WINDOWS_FONTS";

    private static String sWhyNot;

    private WindowsFonts() {
    }

    /**
     * The Windows fonts folder, with a trailing '/', as a path on the current drive; null
     * when there is none to use ({@link #whyNot} says why).
     */
    public static synchronized String directory() {
        String dir = System.getenv(ENV);
        if (dir != null && dir.trim().equalsIgnoreCase("none")) {
            sWhyNot = "$" + ENV + " is none";
            return null;
        }
        if (dir == null || dir.isEmpty()) {
            try {
                dir = nativeFontsDirectory();
            } catch (LinkageError e) {
                sWhyNot = "no natives (" + e + ")";
                return null;
            }
            if (dir == null) {
                sWhyNot = "Windows did not say where its fonts are";
                return null;
            }
        }
        dir = dir.replace('\\', '/');
        if (!dir.startsWith("/")) {
            sWhyNot = dir + " is not on the current drive (set $" + ENV
                    + " to a junction to it on this drive)";
            return null;
        }
        if (!new File(dir).isDirectory()) {
            sWhyNot = dir + " is not a directory";
            return null;
        }
        sWhyNot = null;
        return dir.endsWith("/") ? dir : dir + "/";
    }

    /** Why {@link #directory} found none, or null. */
    public static synchronized String whyNot() {
        return sWhyNot;
    }

    /**
     * {@code %WINDIR%\Fonts} in Gesher's path form: '/' separators, the drive left out
     * when it is the current one (as gesher-base's paths come out, ADR 0010).
     */
    private static native String nativeFontsDirectory();
}
